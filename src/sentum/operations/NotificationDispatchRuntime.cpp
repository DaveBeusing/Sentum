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

class ActiveDispatch {
public:
	explicit ActiveDispatch(std::atomic<std::size_t>& count) : count_(count) {
		count_.fetch_add(1, std::memory_order_relaxed);
	}
	~ActiveDispatch() { count_.fetch_sub(1, std::memory_order_relaxed); }
private:
	std::atomic<std::size_t>& count_;
};

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
	// Snapshot processing cannot clear an unresolved delivery-storage failure.
	if (recovery_required_.load(std::memory_order_acquire)) return;
	status_ = std::move(status);
	last_error_ = std::move(error);
}

void NotificationDispatchRuntime::latch_failure(const char* error) noexcept {
	accepting_.store(false, std::memory_order_release);
	const bool first = !recovery_required_.exchange(true, std::memory_order_acq_rel);
	try {
		std::lock_guard<std::mutex> lock(state_mutex_);
		status_ = "DEGRADED";
		if (first) last_error_ = error;
	} catch (...) {
		// The atomic latch still blocks intake if diagnostic allocation fails.
	}
	publish_metrics();
}

void NotificationDispatchRuntime::persist(const NotificationDeliveryAttempt& attempt) {
	try {
		auto record = notification_delivery_evidence_record(attempt, utc_now());
		record.execution_authorized = false;
		(void)evidence_.append(record);
	} catch (const std::exception& error) {
		persistence_errors_.fetch_add(1, std::memory_order_relaxed);
		latch_failure(error.what());
		throw;
	} catch (...) {
		persistence_errors_.fetch_add(1, std::memory_order_relaxed);
		latch_failure("notification evidence persistence raised an unknown exception");
		throw;
	}
}

NotificationDispatchRuntime::EnqueueResult NotificationDispatchRuntime::enqueue(
	NotificationDeliveryAttempt attempt,
	std::chrono::steady_clock::time_point due,
	bool require_accepting) {
	std::lock_guard<std::mutex> lock(queue_mutex_);
	if (stop_requested_.load(std::memory_order_acquire)) return EnqueueResult::Unavailable;
	if (require_accepting && !accepting_.load(std::memory_order_acquire)) return EnqueueResult::Unavailable;
	if (queue_.size() >= configuration_.queue_capacity) return EnqueueResult::Full;
	queue_.push_back({due, ++queue_sequence_, std::move(attempt)});
	queued_.store(queue_.size(), std::memory_order_release);
	queue_cv_.notify_one();
	return EnqueueResult::Accepted;
}

void NotificationDispatchRuntime::record_terminal_queue_rejection(
	NotificationDeliveryAttempt attempt,
	const char* code,
	const char* reason) {
	// Rejection is an observed queue event. Terminal failure is durable only
	// after the transition below commits successfully.
	queue_rejections_.fetch_add(1, std::memory_order_relaxed);
	attempt = mark_notification_terminal_failure(std::move(attempt), code, reason);
	persist(attempt);
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
	std::vector<WorkItem> recovered;
	recovered.reserve(active.size());
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
		recovered.push_back({now + delay, ++queue_sequence_, std::move(attempt)});
	}
	std::lock_guard<std::mutex> lock(queue_mutex_);
	recovery_queue_ = std::move(recovered);
	refill_recovery_queue_locked();
}

void NotificationDispatchRuntime::refill_recovery_queue_locked() {
	while (queue_.size() < configuration_.queue_capacity && !recovery_queue_.empty()) {
		const auto earliest = std::min_element(recovery_queue_.begin(), recovery_queue_.end(), [](const auto& left, const auto& right) {
			if (left.due != right.due) return left.due < right.due;
			return left.sequence < right.sequence;
		});
		queue_.push_back(std::move(*earliest));
		recovery_queue_.erase(earliest);
	}
	queued_.store(queue_.size(), std::memory_order_release);
	recovery_pending_.store(recovery_queue_.size(), std::memory_order_release);
}

void NotificationDispatchRuntime::request_stop() {
	accepting_.store(false, std::memory_order_release);
	{
		// Both condition-variable predicates change under their waiters' locks.
		std::scoped_lock lock(queue_mutex_, control_mutex_);
		stop_requested_.store(true, std::memory_order_release);
	}
	provider_cancellation_.store(true, std::memory_order_release);
	queue_cv_.notify_all();
	control_cv_.notify_all();
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
		recovery_required_.store(false, std::memory_order_release);
		accepting_.store(true, std::memory_order_release);
		set_status("RUNNING");
		workers_.reserve(configuration_.worker_count);
		for (std::size_t index = 0; index < configuration_.worker_count; ++index) {
			workers_.emplace_back(&NotificationDispatchRuntime::worker_loop, this);
		}
		if (snapshot_source_) scheduler_thread_ = std::thread(&NotificationDispatchRuntime::scheduler_loop, this);
		publish_metrics();
	} catch (...) {
		request_stop();
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
			recovery_queue_.clear();
			queued_.store(0, std::memory_order_release);
			recovery_pending_.store(0, std::memory_order_release);
		}
		started_.store(false, std::memory_order_release);
		throw;
	}
}

void NotificationDispatchRuntime::stop() noexcept {
	if (!started_.exchange(false, std::memory_order_acq_rel)) return;
	request_stop();

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
		recovery_queue_.clear();
		queued_.store(0, std::memory_order_release);
		recovery_pending_.store(0, std::memory_order_release);
	}
	set_status(configuration_.enabled ? "STOPPED" : "DISABLED");
	publish_metrics();
}

std::size_t NotificationDispatchRuntime::submit(const NotificationRoutingPlan& plan) {
	if (!configuration_.enabled || !accepting_.load(std::memory_order_acquire)) return 0;
	std::size_t accepted = 0;
	for (const auto& intent : plan.intents) {
		if (!accepting_.load(std::memory_order_acquire)) break;
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

		const auto result = enqueue(attempt, std::chrono::steady_clock::now(), true);
		if (result == EnqueueResult::Accepted) {
			++accepted;
		} else if (result == EnqueueResult::Full) {
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
		refill_recovery_queue_locked();
		return true;
	}
	return false;
}

void NotificationDispatchRuntime::worker_loop() noexcept {
	try {
		WorkItem work;
		while (take_work(work)) {
			if (stop_requested_.load(std::memory_order_acquire)) break;
			try {
				process_work(std::move(work.attempt));
			} catch (const std::exception& error) {
				worker_errors_.fetch_add(1, std::memory_order_relaxed);
				latch_failure(error.what());
			} catch (...) {
				worker_errors_.fetch_add(1, std::memory_order_relaxed);
				latch_failure("notification worker raised an unknown exception");
			}
		}
	} catch (const std::exception& error) {
		worker_errors_.fetch_add(1, std::memory_order_relaxed);
		latch_failure(error.what());
	} catch (...) {
		worker_errors_.fetch_add(1, std::memory_order_relaxed);
		latch_failure("notification worker boundary raised an unknown exception");
	}
}

void NotificationDispatchRuntime::process_work(NotificationDeliveryAttempt attempt) {
	attempt = mark_notification_dispatched(std::move(attempt));
	persist(attempt);
	if (attempt.terminal) {
		terminal_failed_total_.fetch_add(1, std::memory_order_relaxed);
		publish_metrics();
		return;
	}

	const auto request = notification_dispatch_request(attempt);
	NotificationDispatchResult result;
	const auto provider = providers_.find(attempt.channel);
	const auto started = std::chrono::steady_clock::now();
	provider_calls_.fetch_add(1, std::memory_order_relaxed);
	{
		ActiveDispatch active(dispatching_);
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
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - started).count();
	const auto elapsed_us = static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed));
	provider_latency_us_last_.store(elapsed_us, std::memory_order_relaxed);
	provider_latency_us_total_.fetch_add(elapsed_us, std::memory_order_relaxed);
	if (result.failure_code == "TIMEOUT") timeout_count_.fetch_add(1, std::memory_order_relaxed);

	auto next = apply_notification_dispatch_result(std::move(attempt), result);
	persist(next);
	if (next.state == NotificationDeliveryState::Delivered) {
		delivered_total_.fetch_add(1, std::memory_order_relaxed);
	} else if (next.terminal) {
		terminal_failed_total_.fetch_add(1, std::memory_order_relaxed);
	} else if (notification_delivery_can_retry(next)) {
		retriable_failed_total_.fetch_add(1, std::memory_order_relaxed);
		const auto due = std::chrono::steady_clock::now() + std::chrono::seconds(next.retry_backoff_seconds);
		// Paused intake concerns new intents. Already accepted retries retain
		// their authority, including when another worker hit a storage fault.
		if (enqueue(next, due, false) == EnqueueResult::Full) {
			record_terminal_queue_rejection(
				std::move(next),
				"RETRY_QUEUE_SATURATED",
				"bounded notification retry queue rejected retriable work");
		}
	}
	publish_metrics();
}

void NotificationDispatchRuntime::scheduler_loop() {
	std::uint64_t last_generation = 0;
	while (!stop_requested_.load(std::memory_order_acquire)) {
		bool lifecycle_observed = false;
		try {
			const auto snapshot = snapshot_source_();
			if (snapshot.generation != 0 && snapshot.generation != last_generation) {
				const auto lifecycle = evidence_.observe_alert_lifecycle(snapshot.state, configuration_.recovery_limit);
				const auto lifecycle_json = sentum::ui::operator_alert_lifecycle_json(lifecycle);
				lifecycle_observed = true;
				{
					std::lock_guard<std::mutex> lock(state_mutex_);
					alert_lifecycle_ = lifecycle_json;
					alert_lifecycle_available_ = true;
				}
				auto routed_snapshot = snapshot.state;
				auto& control_plane = routed_snapshot["operations_control_plane"];
				if (!control_plane.is_object()) control_plane = nlohmann::json::object();
				control_plane["alert_lifecycle"] = lifecycle_json;
				control_plane["alert_lifecycle_status"] = "AVAILABLE";
				const auto plan = derive_notification_routing_plan(routed_snapshot, routing_policy_);
				(void)submit(plan);
				last_generation = snapshot.generation;
			}
		} catch (const std::exception& error) {
			if (!lifecycle_observed) {
				std::lock_guard<std::mutex> lock(state_mutex_);
				alert_lifecycle_available_ = false;
			}
			snapshot_errors_.fetch_add(1, std::memory_order_relaxed);
			set_status("DEGRADED", error.what());
			publish_metrics();
		} catch (...) {
			if (!lifecycle_observed) {
				std::lock_guard<std::mutex> lock(state_mutex_);
				alert_lifecycle_available_ = false;
			}
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
	nlohmann::json lifecycle;
	bool lifecycle_available;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		status = status_;
		error = last_error_;
		lifecycle = alert_lifecycle_;
		lifecycle_available = alert_lifecycle_available_;
	}
	const auto calls = provider_calls_.load(std::memory_order_relaxed);
	const auto latency_total = provider_latency_us_total_.load(std::memory_order_relaxed);
	return {
		{"status", recovery_required_.load(std::memory_order_acquire) ? "DEGRADED" : status},
		{"enabled", configuration_.enabled},
		{"running", started_.load(std::memory_order_acquire)},
		{"accepting", accepting_.load(std::memory_order_acquire)},
		{"recovery_required", recovery_required_.load(std::memory_order_acquire)},
		{"queue_capacity", configuration_.queue_capacity},
		{"worker_count", configuration_.worker_count},
		{"queued", queued_.load(std::memory_order_relaxed)},
		{"recovery_pending", recovery_pending_.load(std::memory_order_relaxed)},
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
		{"persistence_errors", persistence_errors_.load(std::memory_order_relaxed)},
		{"worker_errors", worker_errors_.load(std::memory_order_relaxed)},
		{"last_error", error},
		{"alert_lifecycle", lifecycle_available ? std::move(lifecycle) : nlohmann::json::array()},
		{"alert_lifecycle_status", lifecycle_available ? "AVAILABLE" : "UNAVAILABLE"},
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
