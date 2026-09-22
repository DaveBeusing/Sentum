#include <sentum/operations/NotificationDispatchRuntime.hpp>

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sentum::operations {
namespace {

std::string utc_now() {
	const auto now = std::chrono::system_clock::now();
	const std::time_t value = std::chrono::system_clock::to_time_t(now);
	std::tm tm{};
#if defined(_WIN32)
	gmtime_s(&tm, &value);
#else
	gmtime_r(&value, &tm);
#endif
	std::ostringstream out;
	out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
	return out.str();
}

NotificationDispatchResult provider_failure(
	std::string code,
	std::string reason,
	bool retryable) {
	NotificationDispatchResult result;
	result.retryable = retryable;
	result.failure_code = std::move(code);
	result.failure_reason = std::move(reason);
	result.execution_authorized = false;
	return result;
}

} // namespace

NotificationDispatchRuntime::NotificationDispatchRuntime(
	NotificationDispatchConfiguration configuration,
	nlohmann::json routing_policy,
	NotificationProviderRegistry providers,
	std::string evidence_database_path,
	SnapshotSource snapshot_source,
	MetricsSink metrics_sink)
	: configuration_(std::move(configuration)),
	  routing_policy_(std::move(routing_policy)),
	  providers_(std::move(providers)),
	  evidence_(std::move(evidence_database_path)),
	  snapshot_source_(std::move(snapshot_source)),
	  metrics_sink_(std::move(metrics_sink)) {
	if (configuration_.execution_authorized) {
		throw std::invalid_argument("notification dispatch runtime cannot authorize execution");
	}
	if (configuration_.enabled && !notification_policy_valid(routing_policy_)) {
		throw std::invalid_argument("notification routing policy is invalid");
	}
}

NotificationDispatchRuntime::~NotificationDispatchRuntime() {
	stop();
}

void NotificationDispatchRuntime::set_status(std::string status, std::string error) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	status_ = std::move(status);
	last_error_ = std::move(error);
}

void NotificationDispatchRuntime::persist(const NotificationDeliveryAttempt& attempt) {
	auto record = notification_delivery_evidence_record(attempt, utc_now());
	record.execution_authorized = false;
	(void)evidence_.append(record);
}

bool NotificationDispatchRuntime::enqueue(
	NotificationDeliveryAttempt attempt,
	std::chrono::steady_clock::time_point due,
	bool require_accepting) {
	std::lock_guard<std::mutex> lock(queue_mutex_);
	if (stop_requested_.load(std::memory_order_acquire)) return false;
	if (require_accepting && !accepting_.load(std::memory_order_acquire)) return false;
	if (queue_.size() >= configuration_.queue_capacity) return false;
	queue_.push_back({due, ++queue_sequence_, std::move(attempt)});
	queued_.store(queue_.size(), std::memory_order_release);
	queue_cv_.notify_one();
	return true;
}

void NotificationDispatchRuntime::record_terminal_queue_rejection(
	NotificationDeliveryAttempt attempt,
	const char* code,
	const char* reason) {
	attempt = mark_notification_terminal_failure(std::move(attempt), code, reason);
	persist(attempt);
	queue_rejections_.fetch_add(1, std::memory_order_relaxed);
	terminal_failed_total_.fetch_add(1, std::memory_order_relaxed);
	publish_metrics();
}

void NotificationDispatchRuntime::recover() {
	const auto latest = evidence_.load_latest_per_dedup_key(configuration_.recovery_limit);
	if (latest.truncated) {
		throw std::runtime_error("notification dispatch recovery evidence exceeds configured bound");
	}
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		known_keys_.clear();
		for (const auto& persisted : latest.records) known_keys_.insert(persisted.record.dedup_key);
	}

	auto active = evidence_.restore_active_delivery_state(configuration_.recovery_limit);
	if (active.size() > configuration_.queue_capacity) {
		throw std::runtime_error("notification dispatch recovery work exceeds queue capacity");
	}

	const auto now = std::chrono::steady_clock::now();
	for (auto& attempt : active) {
		if (attempt.state == NotificationDeliveryState::Dispatched) {
			attempt = mark_notification_failed(
				std::move(attempt),
				"INTERRUPTED",
				"dispatch interrupted before a durable provider result",
				true);
			persist(attempt);
			if (attempt.terminal) {
				terminal_failed_total_.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
		}
		const auto delay = attempt.state == NotificationDeliveryState::Failed
			? std::chrono::seconds(attempt.retry_backoff_seconds)
			: std::chrono::seconds(0);
		if (!enqueue(std::move(attempt), now + delay, false)) {
			throw std::runtime_error("notification dispatch recovery queue rejected durable work");
		}
	}
}

void NotificationDispatchRuntime::start() {
	bool expected = false;
	if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

	try {
		stop_requested_.store(false, std::memory_order_release);
		provider_cancellation_.store(false, std::memory_order_release);
		if (!configuration_.enabled) {
			accepting_.store(false, std::memory_order_release);
			set_status("DISABLED");
			publish_metrics();
			return;
		}

		recover();
		accepting_.store(true, std::memory_order_release);
		set_status("RUNNING");
		workers_.reserve(configuration_.worker_count);
		for (std::size_t index = 0; index < configuration_.worker_count; ++index) {
			workers_.emplace_back(&NotificationDispatchRuntime::worker_loop, this);
		}
		if (snapshot_source_) scheduler_thread_ = std::thread(&NotificationDispatchRuntime::scheduler_loop, this);
		publish_metrics();
	} catch (...) {
		accepting_.store(false, std::memory_order_release);
		stop_requested_.store(true, std::memory_order_release);
		provider_cancellation_.store(true, std::memory_order_release);
		queue_cv_.notify_all();
		control_cv_.notify_all();
		if (scheduler_thread_.joinable() && scheduler_thread_.get_id() != std::this_thread::get_id()) {
			scheduler_thread_.join();
		}
		for (auto& worker : workers_) {
			if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
		}
		workers_.clear();
		{
			std::lock_guard<std::mutex> lock(queue_mutex_);
			queue_.clear();
			queued_.store(0, std::memory_order_release);
		}
		started_.store(false, std::memory_order_release);
		throw;
	}
}

void NotificationDispatchRuntime::stop() noexcept {
	if (!started_.exchange(false, std::memory_order_acq_rel)) return;
	accepting_.store(false, std::memory_order_release);
	stop_requested_.store(true, std::memory_order_release);
	provider_cancellation_.store(true, std::memory_order_release);
	queue_cv_.notify_all();
	control_cv_.notify_all();

	if (scheduler_thread_.joinable() && scheduler_thread_.get_id() != std::this_thread::get_id()) {
		scheduler_thread_.join();
	}
	for (auto& worker : workers_) {
		if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
	}
	workers_.clear();

	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		queue_.clear();
		queued_.store(0, std::memory_order_release);
	}
	set_status(configuration_.enabled ? "STOPPED" : "DISABLED");
	publish_metrics();
}

std::size_t NotificationDispatchRuntime::submit(const NotificationRoutingPlan& plan) {
	if (!configuration_.enabled || !accepting_.load(std::memory_order_acquire)) return 0;
	std::size_t accepted = 0;
	for (const auto& intent : plan.intents) {
		if (!intent.delivery_authorized || intent.status != "ELIGIBLE" || intent.dedup_key.empty()) continue;

		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			if (!known_keys_.insert(intent.dedup_key).second) continue;
		}

		auto attempt = begin_notification_delivery(intent, configuration_.max_attempts);
		try {
			persist(attempt);
		} catch (...) {
			std::lock_guard<std::mutex> lock(state_mutex_);
			known_keys_.erase(intent.dedup_key);
			throw;
		}

		if (enqueue(attempt, std::chrono::steady_clock::now(), true)) {
			++accepted;
		} else {
			record_terminal_queue_rejection(
				std::move(attempt),
				"QUEUE_SATURATED",
				"bounded notification dispatch queue rejected eligible work");
		}
	}
	publish_metrics();
	return accepted;
}

bool NotificationDispatchRuntime::take_work(WorkItem& item) {
	std::unique_lock<std::mutex> lock(queue_mutex_);
	while (!stop_requested_.load(std::memory_order_acquire)) {
		if (queue_.empty()) {
			queue_cv_.wait(lock, [this] {
				return stop_requested_.load(std::memory_order_acquire) || !queue_.empty();
			});
			continue;
		}
		const auto earliest = std::min_element(queue_.begin(), queue_.end(), [](const auto& left, const auto& right) {
			if (left.due != right.due) return left.due < right.due;
			return left.sequence < right.sequence;
		});
		const auto now = std::chrono::steady_clock::now();
		if (earliest->due > now) {
			queue_cv_.wait_until(lock, earliest->due);
			continue;
		}
		item = std::move(*earliest);
		queue_.erase(earliest);
		queued_.store(queue_.size(), std::memory_order_release);
		return true;
	}
	return false;
}

void NotificationDispatchRuntime::worker_loop() {
	WorkItem work;
	while (take_work(work)) {
		if (stop_requested_.load(std::memory_order_acquire)) break;

		auto attempt = mark_notification_dispatched(std::move(work.attempt));
		try {
			persist(attempt);
		} catch (const std::exception& error) {
			set_status("DEGRADED", error.what());
			snapshot_errors_.fetch_add(1, std::memory_order_relaxed);
			publish_metrics();
			continue;
		}
		if (attempt.terminal) {
			terminal_failed_total_.fetch_add(1, std::memory_order_relaxed);
			publish_metrics();
			continue;
		}

		const auto request = notification_dispatch_request(attempt);
		NotificationDispatchResult result;
		const auto provider = providers_.find(attempt.channel);
		const auto started = std::chrono::steady_clock::now();
		dispatching_.fetch_add(1, std::memory_order_relaxed);
		provider_calls_.fetch_add(1, std::memory_order_relaxed);
		try {
			if (!provider) {
				result = provider_failure(
					"PROVIDER_UNAVAILABLE",
					"no configured notification provider for governed channel",
					false);
			} else {
				result = provider->dispatch(request, provider_cancellation_);
				result.execution_authorized = false;
			}
		} catch (const std::exception& error) {
			result = provider_failure("PROVIDER_EXCEPTION", error.what(), true);
		} catch (...) {
			result = provider_failure("PROVIDER_EXCEPTION", "notification provider raised an unknown exception", true);
		}
		dispatching_.fetch_sub(1, std::memory_order_relaxed);
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - started).count();
		const auto elapsed_us = static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed));
		provider_latency_us_last_.store(elapsed_us, std::memory_order_relaxed);
		provider_latency_us_total_.fetch_add(elapsed_us, std::memory_order_relaxed);
		if (result.failure_code == "TIMEOUT") timeout_count_.fetch_add(1, std::memory_order_relaxed);

		auto next = apply_notification_dispatch_result(std::move(attempt), result);
		try {
			persist(next);
		} catch (const std::exception& error) {
			set_status("DEGRADED", error.what());
			snapshot_errors_.fetch_add(1, std::memory_order_relaxed);
			publish_metrics();
			continue;
		}

		if (next.state == NotificationDeliveryState::Delivered) {
			delivered_total_.fetch_add(1, std::memory_order_relaxed);
		} else if (next.terminal) {
			terminal_failed_total_.fetch_add(1, std::memory_order_relaxed);
		} else if (notification_delivery_can_retry(next)) {
			retriable_failed_total_.fetch_add(1, std::memory_order_relaxed);
			if (!stop_requested_.load(std::memory_order_acquire)) {
				const auto due = std::chrono::steady_clock::now() + std::chrono::seconds(next.retry_backoff_seconds);
				if (!enqueue(next, due, true)) {
					record_terminal_queue_rejection(
						std::move(next),
						"RETRY_QUEUE_SATURATED",
						"bounded notification retry queue rejected retriable work");
				}
			}
		}
		publish_metrics();
	}
}

void NotificationDispatchRuntime::scheduler_loop() {
	std::uint64_t last_generation = 0;
	while (!stop_requested_.load(std::memory_order_acquire)) {
		try {
			const auto snapshot = snapshot_source_();
			if (snapshot.generation != 0 && snapshot.generation != last_generation) {
				const auto plan = derive_notification_routing_plan(snapshot.state, routing_policy_);
				(void)submit(plan);
				last_generation = snapshot.generation;
			}
		} catch (const std::exception& error) {
			snapshot_errors_.fetch_add(1, std::memory_order_relaxed);
			set_status("DEGRADED", error.what());
			publish_metrics();
		} catch (...) {
			snapshot_errors_.fetch_add(1, std::memory_order_relaxed);
			set_status("DEGRADED", "notification snapshot processing failed");
			publish_metrics();
		}

		std::unique_lock<std::mutex> lock(control_mutex_);
		control_cv_.wait_for(
			lock,
			std::chrono::milliseconds(configuration_.snapshot_poll_interval_ms),
			[this] { return stop_requested_.load(std::memory_order_acquire); });
	}
}

nlohmann::json NotificationDispatchRuntime::metrics_json() const {
	std::string status;
	std::string error;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		status = status_;
		error = last_error_;
	}
	const auto calls = provider_calls_.load(std::memory_order_relaxed);
	const auto latency_total = provider_latency_us_total_.load(std::memory_order_relaxed);
	return {
		{"status", status},
		{"enabled", configuration_.enabled},
		{"queue_capacity", configuration_.queue_capacity},
		{"worker_count", configuration_.worker_count},
		{"queued", queued_.load(std::memory_order_relaxed)},
		{"dispatching", dispatching_.load(std::memory_order_relaxed)},
		{"delivered", delivered_total_.load(std::memory_order_relaxed)},
		{"retriable_failed", retriable_failed_total_.load(std::memory_order_relaxed)},
		{"terminal_failed", terminal_failed_total_.load(std::memory_order_relaxed)},
		{"provider_calls", calls},
		{"provider_latency_ms_last", static_cast<double>(provider_latency_us_last_.load(std::memory_order_relaxed)) / 1000.0},
		{"provider_latency_ms_average", calls == 0 ? 0.0 : static_cast<double>(latency_total) / 1000.0 / static_cast<double>(calls)},
		{"timeout_count", timeout_count_.load(std::memory_order_relaxed)},
		{"queue_rejections", queue_rejections_.load(std::memory_order_relaxed)},
		{"snapshot_errors", snapshot_errors_.load(std::memory_order_relaxed)},
		{"last_error", error},
		{"execution_authorized", false}
	};
}

void NotificationDispatchRuntime::publish_metrics() {
	if (!metrics_sink_) return;
	try {
		const auto metrics = metrics_json();
		const auto serialized = metrics.dump();
		{
			std::lock_guard<std::mutex> lock(publish_mutex_);
			if (serialized == last_published_metrics_) return;
			last_published_metrics_ = serialized;
		}
		metrics_sink_(metrics);
	} catch (...) {
		// Metrics publication is observational and must never affect dispatch authority.
	}
}

} // namespace sentum::operations
