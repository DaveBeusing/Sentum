#include <sentum/operations/NotificationDispatchRuntime.hpp>
#include <sentum/operations/NotificationOperationsObservability.hpp>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace sentum::operations;

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
void eventually(Predicate predicate, const char* message, std::chrono::milliseconds timeout = 3s) {
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!predicate()) {
		if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
		std::this_thread::sleep_for(5ms);
	}
}

struct TemporaryDatabase {
	std::filesystem::path directory;
	std::string path;
	TemporaryDatabase() {
		auto pattern = (std::filesystem::temp_directory_path() / "sentum-notification-writes-XXXXXX").string();
		const auto* created = ::mkdtemp(pattern.data());
		require(created != nullptr, "cannot create notification fixture directory");
		directory = created;
		path = (directory / "evidence.sqlite3").string();
	}
	~TemporaryDatabase() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
};

class SqliteControl {
public:
	explicit SqliteControl(const std::string& path) {
		require(sqlite3_open(path.c_str(), &database_) == SQLITE_OK, "cannot open notification fixture database");
	}
	~SqliteControl() { sqlite3_close(database_); }
	void execute(const char* sql) {
		require(sqlite3_exec(database_, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "notification fixture SQL failed");
	}
private:
	sqlite3* database_ = nullptr;
};

nlohmann::json policy() {
	return {{"schema_version", 1}, {"authority", "NOTIFICATION_ROUTING_ONLY"}, {"default_action", "BLOCK"},
		{"allowed_channels", {"PAGER"}}, {"routes", nlohmann::json::array()}};
}

NotificationDispatchConfiguration configuration(std::size_t capacity = 1) {
	NotificationDispatchConfiguration result;
	result.enabled = true;
	result.queue_capacity = capacity;
	result.worker_count = 1;
	result.max_attempts = 3;
	result.recovery_limit = 16;
	result.snapshot_poll_interval_ms = 100;
	result.enabled_channels = {"PAGER"};
	return result;
}

NotificationRoutingPlan plan(const std::string& id) {
	NotificationRouteIntent intent;
	intent.alert_id = id;
	intent.severity = "WARNING";
	intent.escalation_level = 2;
	intent.channel = "PAGER";
	intent.audience = "OPERATIONS";
	intent.dedup_key = notification_dedup_key(id, 1, 2, intent.channel, intent.audience);
	intent.status = "ELIGIBLE";
	intent.delivery_authorized = true;
	NotificationRoutingPlan result;
	result.policy_valid = true;
	result.eligible = 1;
	result.intents.push_back(std::move(intent));
	return result;
}

NotificationDeliveryEvidenceRecord latest(const std::string& path, const std::string& key) {
	NotificationDeliveryEvidenceRepository reader(path, NotificationDeliveryEvidenceOpenMode::ReadOnly);
	for (const auto& item : reader.load_latest_per_dedup_key(16).records) {
		if (item.record.dedup_key == key) return item.record;
	}
	return {};
}

class FixtureProvider final : public INotificationProvider {
public:
	enum class Mode { Success, RetryFirstAfterRelease, ThrowUnknownFirst };
	FixtureProvider(std::string path, Mode mode = Mode::Success) : path_(std::move(path)), mode_(mode) {}

	NotificationDispatchResult dispatch(const NotificationDispatchRequest& request,
		const std::atomic<bool>& cancelled) override {
		const auto record = latest(path_, request.dedup_key);
		if (record.state != "DISPATCHED" || record.attempt != request.attempt || request.execution_authorized) {
			durable_violation_.store(true);
		}
		const auto index = calls_.fetch_add(1);
		{
			std::lock_guard<std::mutex> lock(mutex_);
			requests_.push_back(request);
			call_times_.push_back(std::chrono::steady_clock::now());
		}
		if (mode_ == Mode::ThrowUnknownFirst && index == 0) throw 42;
		if (mode_ == Mode::RetryFirstAfterRelease && index == 0) {
			std::unique_lock<std::mutex> lock(mutex_);
			blocked_.store(true);
			while (!released_ && !cancelled.load(std::memory_order_acquire)) cv_.wait_for(lock, 5ms);
			NotificationDispatchResult failed;
			failed.retryable = true;
			failed.failure_code = cancelled.load() ? "CANCELLED" : "TRANSIENT";
			failed.failure_reason = "controlled provider failure";
			return failed;
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			// A compatible provider applies a repeated delivery key once.
			effects_.insert(request.dedup_key);
		}
		NotificationDispatchResult result;
		result.accepted = result.delivered = true;
		result.provider_reference = "confirmed:" + request.dedup_key;
		return result;
	}

	void release() { std::lock_guard<std::mutex> lock(mutex_); released_ = true; cv_.notify_all(); }
	bool blocked() const { return blocked_.load(); }
	std::size_t calls() const { return calls_.load(); }
	bool durable_violation() const { return durable_violation_.load(); }
	std::size_t effects() const { std::lock_guard<std::mutex> lock(mutex_); return effects_.size(); }
	std::vector<NotificationDispatchRequest> requests() const { std::lock_guard<std::mutex> lock(mutex_); return requests_; }
	std::vector<std::chrono::steady_clock::time_point> call_times() const {
		std::lock_guard<std::mutex> lock(mutex_); return call_times_;
	}
private:
	std::string path_;
	Mode mode_;
	std::atomic<std::size_t> calls_{0};
	std::atomic<bool> blocked_{false}, durable_violation_{false};
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	bool released_ = false;
	std::vector<NotificationDispatchRequest> requests_;
	std::vector<std::chrono::steady_clock::time_point> call_times_;
	std::unordered_set<std::string> effects_;
};

NotificationProviderRegistry registry(const std::shared_ptr<FixtureProvider>& provider) {
	NotificationProviderRegistry result;
	result.add("PAGER", provider);
	return result;
}

void require_degraded(const NotificationDispatchRuntime& runtime) {
	const auto metrics = runtime.metrics_json();
	require(metrics.at("status") == "DEGRADED" && metrics.at("recovery_required") == true &&
		metrics.at("accepting") == false, "storage failure did not pause notification intake visibly");
	require(!metrics.at("last_error").get<std::string>().empty(), "storage failure lost its diagnostic");
	require(metrics.at("execution_authorized") == false, "notification failure acquired execution authority");
	const auto view = derive_notification_operations_view_from_snapshot({{"notification_dispatch_runtime", metrics}});
	const auto projected = notification_operations_json(view).at("dispatch_runtime");
	require(projected.at("status") == "DEGRADED" && projected.at("recovery_required") == true &&
		projected.at("accepting") == false && projected.at("running") == metrics.at("running") &&
		projected.at("persistence_errors") == metrics.at("persistence_errors") &&
		projected.at("worker_errors") == metrics.at("worker_errors") && projected.at("last_error") == metrics.at("last_error"),
		"operator projection discarded notification failure or recovery diagnostics");
}

void retry_rejection_failure_survives_and_recovers() {
	TemporaryDatabase database;
	auto provider = std::make_shared<FixtureProvider>(database.path, FixtureProvider::Mode::RetryFirstAfterRelease);
	const auto first = plan("retrying");
	const auto waiting = plan("waiting");
	const auto first_key = first.intents.front().dedup_key;
	const auto waiting_key = waiting.intents.front().dedup_key;
	std::atomic<bool> snapshot_failure{false};
	{
		NotificationDispatchRuntime runtime(configuration(), policy(), registry(provider), database.path,
			[&] {
				if (snapshot_failure.load()) throw std::runtime_error("later snapshot failure");
				return NotificationRuntimeSnapshot{};
			});
		SqliteControl control(database.path);
		control.execute("CREATE TRIGGER reject_retry_terminal BEFORE INSERT ON notification_delivery_evidence "
			"WHEN NEW.failure_code='RETRY_QUEUE_SATURATED' BEGIN SELECT RAISE(ABORT,'retry terminal write failed'); END;");
		control.execute("CREATE TRIGGER reject_waiting_dispatch BEFORE INSERT ON notification_delivery_evidence "
			"WHEN NEW.alert_id='waiting' AND NEW.state='DISPATCHED' BEGIN SELECT RAISE(ABORT,'waiting dispatch write failed'); END;");
		runtime.start();
		require(runtime.submit(first) == 1, "first delivery was not admitted");
		eventually([&] { return provider->blocked(); }, "provider did not reach the saturation gate");
		require(runtime.submit(waiting) == 1, "waiting delivery did not fill the bounded queue");
		provider->release();
		eventually([&] { return runtime.metrics_json().at("worker_errors") == 2; },
			"worker did not survive retry-rejection persistence failure and process the queued tail");
		require(runtime.running(), "notification storage failure stopped the enclosing runtime");
		require_degraded(runtime);
		const auto metrics = runtime.metrics_json();
		require(metrics.at("persistence_errors") == 2 && metrics.at("queue_rejections") == 1 &&
			metrics.at("terminal_failed") == 0 && metrics.at("delivered") == 0 && metrics.at("dispatching") == 0,
			"failed evidence write was counted as a durable terminal result");
		const auto retry = latest(database.path, first_key);
		const auto pending = latest(database.path, waiting_key);
		require(retry.state == "FAILED" && !retry.terminal && retry.attempt == 1 && retry.retry_backoff_seconds == 5 &&
			retry.failure_code == "TRANSIENT", "retry rejection destroyed the prior durable retry authority");
		require(pending.state == "PENDING" && pending.attempt == 0 && provider->calls() == 1,
			"provider was called before DISPATCHED committed");
		require(runtime.submit(plan("after-failure")) == 0, "failed evidence storage still accepted new delivery work");
		snapshot_failure.store(true);
		eventually([&] { return runtime.metrics_json().at("snapshot_errors") != 0; }, "scheduler did not observe later snapshot failure");
		require(runtime.metrics_json().at("last_error").get<std::string>().find("retry terminal write failed") != std::string::npos,
			"scheduler replaced the latched storage failure diagnostic");
		const auto before_stop = std::chrono::steady_clock::now();
		runtime.stop();
		require(std::chrono::steady_clock::now() - before_stop < 1s, "failed notification worker did not stop within one second");
		require_degraded(runtime);
		require(runtime.metrics_json().at("running") == false, "stopped degraded runtime still reported worker ownership");
		control.execute("DROP TRIGGER reject_retry_terminal;");
		control.execute("DROP TRIGGER reject_waiting_dispatch;");
	}

	const auto restarted = std::chrono::steady_clock::now();
	{
		NotificationDispatchRuntime runtime(configuration(), policy(), registry(provider), database.path);
		runtime.start();
		eventually([&] { return latest(database.path, waiting_key).state == "DELIVERED"; },
			"restart did not stage more durable work than the single queue slot");
		require(provider->calls() == 2, "restart ignored the retained retry backoff");
		eventually([&] {
			const auto metrics = runtime.metrics_json();
			require(metrics.at("queued").get<std::size_t>() <= 1 && metrics.at("recovery_pending").get<std::size_t>() <= 16,
				"recovery exceeded its queue or staging bound");
			return metrics.at("delivered") == 2;
		}, "restart did not recover both durable attempts", 8s);
		runtime.stop();
		require(runtime.metrics_json().at("status") == "STOPPED" && runtime.metrics_json().at("recovery_required") == false,
			"successful restart retained an unresolved failure latch");
	}
	const auto requests = provider->requests();
	const auto times = provider->call_times();
	require(requests.size() == 3 && requests[1].dedup_key == waiting_key && requests[1].attempt == 1 &&
		requests[2].dedup_key == first_key && requests[2].attempt == 2 && times[2] - restarted >= 4900ms,
		"restart changed delivery identity, attempt order or persisted backoff");
	require(provider->effects() == 2 && !provider->durable_violation(), "recovered dispatch violated durable provider gating");
	{
		NotificationDispatchRuntime runtime(configuration(), policy(), registry(provider), database.path);
		runtime.start();
		require(runtime.submit(first) == 0 && runtime.submit(waiting) == 0, "delivered recovery keys lost deduplication");
		runtime.stop();
		require(provider->calls() == 3, "completed recovery dispatched a duplicate provider call");
	}
}

void pending_write_failure_rejects_intake_until_recovery() {
	TemporaryDatabase database;
	auto provider = std::make_shared<FixtureProvider>(database.path);
	NotificationDispatchRuntime runtime(configuration(), policy(), registry(provider), database.path);
	SqliteControl control(database.path);
	control.execute("CREATE TRIGGER reject_pending BEFORE INSERT ON notification_delivery_evidence "
		"WHEN NEW.state='PENDING' BEGIN SELECT RAISE(ABORT,'pending write failed'); END;");
	runtime.start();
	const auto routes = plan("pending");
	bool rejected = false;
	try { (void)runtime.submit(routes); } catch (const std::runtime_error&) { rejected = true; }
	require(rejected && provider->calls() == 0 && latest(database.path, routes.intents.front().dedup_key).state.empty(),
		"failed initial write admitted or dispatched undurable work");
	require_degraded(runtime);
	require(runtime.submit(routes) == 0 && runtime.metrics_json().at("persistence_errors") == 1,
		"paused admission repeatedly attempted the failed initial write");
	runtime.stop();
	control.execute("DROP TRIGGER reject_pending;");
	runtime.start();
	require(runtime.submit(routes) == 1, "successful recovery retained a key that never committed");
	eventually([&] { return runtime.metrics_json().at("delivered") == 1; }, "recovered initial write did not deliver");
	runtime.stop();
	require(provider->calls() == 1 && !provider->durable_violation(), "initial-write recovery violated dispatch authority");
}

void provider_result_failure_reuses_delivery_identity() {
	TemporaryDatabase database;
	auto provider = std::make_shared<FixtureProvider>(database.path);
	const auto routes = plan("uncertain-result");
	const auto key = routes.intents.front().dedup_key;
	{
		NotificationDispatchRuntime runtime(configuration(), policy(), registry(provider), database.path);
		SqliteControl control(database.path);
		control.execute("CREATE TRIGGER reject_delivered BEFORE INSERT ON notification_delivery_evidence "
			"WHEN NEW.state='DELIVERED' BEGIN SELECT RAISE(ABORT,'provider result write failed'); END;");
		runtime.start();
		require(runtime.submit(routes) == 1, "provider-result fixture was not admitted");
		eventually([&] { return runtime.metrics_json().at("worker_errors") == 1; }, "provider-result failure escaped its worker boundary");
		require_degraded(runtime);
		require(provider->effects() == 1 && runtime.metrics_json().at("delivered") == 0 && latest(database.path, key).state == "DISPATCHED",
			"undurable provider result was reported as confirmed delivery");
		runtime.stop();
		control.execute("DROP TRIGGER reject_delivered;");
	}
	{
		NotificationDispatchRuntime runtime(configuration(), policy(), registry(provider), database.path);
		runtime.start();
		const auto interrupted = latest(database.path, key);
		require(interrupted.state == "FAILED" && !interrupted.terminal && interrupted.failure_code == "INTERRUPTED" &&
			interrupted.retry_backoff_seconds == 5, "uncertain provider result did not recover through durable interruption state");
		eventually([&] { return runtime.metrics_json().at("delivered") == 1; }, "interrupted delivery was not recovered", 8s);
		runtime.stop();
	}
	const auto requests = provider->requests();
	require(requests.size() == 2 && requests[0].dedup_key == requests[1].dedup_key && requests[1].attempt == 2 &&
		provider->effects() == 1 && !provider->durable_violation(), "uncertain-result recovery lost provider idempotency identity");
}

void unknown_provider_exception_does_not_escape_worker() {
	TemporaryDatabase database;
	auto provider = std::make_shared<FixtureProvider>(database.path, FixtureProvider::Mode::ThrowUnknownFirst);
	auto config = configuration(4);
	config.max_attempts = 1;
	NotificationDispatchRuntime runtime(config, policy(), registry(provider), database.path);
	runtime.start();
	const auto failed = plan("unknown-exception");
	require(runtime.submit(failed) == 1 && runtime.submit(plan("after-exception")) == 1, "unknown-exception fixture was not admitted");
	eventually([&] { return runtime.metrics_json().at("delivered") == 1; }, "unknown provider exception terminated its worker");
	runtime.stop();
	const auto record = latest(database.path, failed.intents.front().dedup_key);
	require(record.terminal && record.failure_code == "PROVIDER_EXCEPTION" && runtime.metrics_json().at("dispatching") == 0 &&
		runtime.metrics_json().at("terminal_failed") == 1 && runtime.metrics_json().at("persistence_errors") == 0,
		"unknown provider exception corrupted durable counters or active dispatch ownership");
}

void recovery_staging_preserves_bounds_and_cancellation() {
	TemporaryDatabase database;
	{
		NotificationDeliveryEvidenceRepository writer(database.path);
		for (int index = 0; index < 3; ++index) {
			auto attempt = begin_notification_delivery(plan("staged-" + std::to_string(index)).intents.front());
			attempt = mark_notification_dispatched(std::move(attempt));
			attempt = mark_notification_failed(std::move(attempt), "TRANSIENT", "retained retry", true);
			require(writer.append(notification_delivery_evidence_record(attempt, "2026-10-23T00:00:00Z")),
				"cannot seed bounded recovery fixture");
		}
	}
	auto provider = std::make_shared<FixtureProvider>(database.path);
	{
		auto config = configuration();
		config.recovery_limit = 2;
		NotificationDispatchRuntime runtime(config, policy(), registry(provider), database.path);
		bool rejected = false;
		try { runtime.start(); } catch (const std::runtime_error&) { rejected = true; }
		require(rejected && !runtime.running() && provider->calls() == 0,
			"recovery staging weakened the durable query bound");
	}
	{
		NotificationDispatchRuntime runtime(configuration(), policy(), registry(provider), database.path);
		runtime.start();
		const auto metrics = runtime.metrics_json();
		require(metrics.at("queued") == 1 && metrics.at("recovery_pending") == 2 && provider->calls() == 0,
			"bounded recovery staging did not retain delayed attempts separately");
		const auto before_stop = std::chrono::steady_clock::now();
		runtime.stop();
		require(std::chrono::steady_clock::now() - before_stop < 1s, "shutdown waited for staged retry deadlines");
		require(runtime.metrics_json().at("queued") == 0 && runtime.metrics_json().at("recovery_pending") == 0,
			"shutdown retained in-memory recovery ownership");
	}
	NotificationDeliveryEvidenceRepository reader(database.path, NotificationDeliveryEvidenceOpenMode::ReadOnly);
	const auto active = reader.restore_active_delivery_state(16);
	require(active.size() == 3 && provider->calls() == 0, "staged shutdown discarded durable retry authority");
	for (const auto& attempt : active) require(attempt.attempt == 1 && !attempt.terminal && attempt.retry_backoff_seconds == 5,
		"staged shutdown changed a durable retry budget");
}
}

int main() {
	try {
		retry_rejection_failure_survives_and_recovers();
		pending_write_failure_rejects_intake_until_recovery();
		provider_result_failure_reuses_delivery_identity();
		unknown_provider_exception_does_not_escape_worker();
		recovery_staging_preserves_bounds_and_cancellation();
		std::cout << "Notification persistence failure tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "Notification persistence failure test: " << error.what() << '\n';
		return 1;
	}
}
