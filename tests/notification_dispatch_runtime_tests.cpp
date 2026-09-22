#include <sentum/operations/NotificationDispatchConfiguration.hpp>
#include <sentum/operations/NotificationDispatchRuntime.hpp>
#include <sentum/operations/NotificationIncidentWorkflowBridge.hpp>
#include <sentum/operations/NotificationOperationsObservability.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using sentum::operations::INotificationProvider;
using sentum::operations::NotificationDeliveryEvidenceOpenMode;
using sentum::operations::NotificationDeliveryEvidenceRecord;
using sentum::operations::NotificationDeliveryEvidenceRepository;
using sentum::operations::NotificationDeliveryState;
using sentum::operations::NotificationDispatchConfiguration;
using sentum::operations::NotificationDispatchRequest;
using sentum::operations::NotificationDispatchResult;
using sentum::operations::NotificationDispatchRuntime;
using sentum::operations::NotificationIncidentCandidateState;
using sentum::operations::NotificationOperationsHealth;
using sentum::operations::NotificationProviderRegistry;
using sentum::operations::NotificationRouteIntent;
using sentum::operations::NotificationRoutingPlan;

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

std::filesystem::path temporary_path(const char* suffix) {
	static std::atomic<std::uint64_t> counter{0};
	const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() /
		("sentum_notification_runtime_" + std::to_string(tick) + "_" +
		 std::to_string(counter.fetch_add(1)) + suffix);
}

void cleanup_database(const std::filesystem::path& path) {
	std::error_code error;
	std::filesystem::remove(path, error);
	std::filesystem::remove(path.string() + "-wal", error);
	std::filesystem::remove(path.string() + "-shm", error);
}

nlohmann::json routing_policy() {
	return {
		{"schema_version", 1},
		{"authority", "NOTIFICATION_ROUTING_ONLY"},
		{"default_action", "BLOCK"},
		{"allowed_channels", nlohmann::json::array({"PAGER", "EMAIL"})},
		{"routes", nlohmann::json::array()}
	};
}

NotificationDispatchConfiguration configuration(std::size_t queue_capacity = 8, std::size_t max_attempts = 3) {
	NotificationDispatchConfiguration config;
	config.enabled = true;
	config.queue_capacity = queue_capacity;
	config.worker_count = 1;
	config.max_attempts = max_attempts;
	config.snapshot_poll_interval_ms = 100;
	config.recovery_limit = 64;
	config.enabled_channels = {"PAGER", "EMAIL"};
	config.execution_authorized = false;
	return config;
}

NotificationRoutingPlan plan(std::initializer_list<std::pair<std::string, std::string>> routes) {
	NotificationRoutingPlan value;
	value.policy_valid = true;
	value.policy_status = "VALID";
	for (const auto& route : routes) {
		NotificationRouteIntent intent;
		intent.alert_id = route.first;
		intent.generation = 1;
		intent.severity = "CRITICAL";
		intent.escalation_level = 3;
		intent.channel = route.second;
		intent.audience = "OPERATIONS_ON_CALL";
		intent.dedup_key = route.first + ":g1:l3:" + route.second + ":OPERATIONS_ON_CALL";
		intent.status = "ELIGIBLE";
		intent.reason = "test eligible route";
		intent.delivery_authorized = true;
		intent.execution_authorized = false;
		value.intents.push_back(std::move(intent));
		++value.eligible;
	}
	return value;
}

class ScriptedProvider final : public INotificationProvider {
public:
	explicit ScriptedProvider(std::vector<NotificationDispatchResult> results)
		: results_(std::move(results)) {}

	NotificationDispatchResult dispatch(
		const NotificationDispatchRequest& request,
		const std::atomic<bool>&) override {
		require(!request.execution_authorized, "provider request granted execution authority");
		const auto index = calls_.fetch_add(1);
		std::lock_guard<std::mutex> lock(mutex_);
		if (results_.empty()) throw std::runtime_error("scripted provider has no result");
		return results_[std::min<std::size_t>(index, results_.size() - 1)];
	}

	std::size_t calls() const noexcept { return calls_.load(); }

private:
	std::vector<NotificationDispatchResult> results_;
	std::atomic<std::size_t> calls_{0};
	std::mutex mutex_;
};

class ThrowingProvider final : public INotificationProvider {
public:
	NotificationDispatchResult dispatch(const NotificationDispatchRequest&, const std::atomic<bool>&) override {
		calls_.fetch_add(1);
		throw std::runtime_error("provider exploded");
	}
	std::size_t calls() const noexcept { return calls_.load(); }
private:
	std::atomic<std::size_t> calls_{0};
};

class GateProvider final : public INotificationProvider {
public:
	NotificationDispatchResult dispatch(
		const NotificationDispatchRequest&,
		const std::atomic<bool>& cancelled) override {
		active_.store(true);
		calls_.fetch_add(1);
		while (!release_.load() && !cancelled.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
		NotificationDispatchResult result;
		if (cancelled.load(std::memory_order_acquire)) {
			result.retryable = true;
			result.failure_code = "CANCELLED";
			result.failure_reason = "cancelled by runtime";
			return result;
		}
		result.accepted = true;
		result.delivered = true;
		result.provider_reference = "gate-delivered";
		return result;
	}
	void release() noexcept { release_.store(true); }
	bool active() const noexcept { return active_.load(); }
	std::size_t calls() const noexcept { return calls_.load(); }
private:
	std::atomic<bool> active_{false};
	std::atomic<bool> release_{false};
	std::atomic<std::size_t> calls_{0};
};

NotificationDispatchResult delivered(const char* reference = "provider-ok") {
	NotificationDispatchResult result;
	result.accepted = true;
	result.delivered = true;
	result.provider_reference = reference;
	result.execution_authorized = false;
	return result;
}

NotificationDispatchResult failed(const char* code, bool retryable) {
	NotificationDispatchResult result;
	result.retryable = retryable;
	result.failure_code = code;
	result.failure_reason = "scripted failure";
	result.execution_authorized = false;
	return result;
}

NotificationDeliveryEvidenceRecord wait_latest(
	const std::filesystem::path& path,
	const std::string& key,
	const std::string& state,
	std::chrono::milliseconds timeout = 2500ms) {
	NotificationDeliveryEvidenceRepository reader(path.string(), NotificationDeliveryEvidenceOpenMode::ReadOnly);
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		const auto batch = reader.load_latest_per_dedup_key(64);
		for (const auto& item : batch.records) {
			if (item.record.dedup_key == key && item.record.state == state) return item.record;
		}
		std::this_thread::sleep_for(10ms);
	}
	throw std::runtime_error("timed out waiting for notification evidence state " + state);
}

void seed(const std::filesystem::path& path, const NotificationDeliveryEvidenceRecord& record) {
	NotificationDeliveryEvidenceRepository writer(path.string());
	require(writer.append(record), "failed to seed notification evidence");
}

NotificationDeliveryEvidenceRecord evidence(
	std::string key,
	std::string state,
	std::size_t attempt,
	bool terminal) {
	NotificationDeliveryEvidenceRecord record;
	record.dedup_key = std::move(key);
	record.alert_id = "seed";
	record.generation = 1;
	record.channel = "PAGER";
	record.audience = "OPERATIONS_ON_CALL";
	record.state = std::move(state);
	record.attempt = attempt;
	record.max_attempts = 3;
	record.retry_backoff_seconds = record.state == "FAILED" && !terminal ? 5 : 0;
	record.terminal = terminal;
	record.delivery_authorized = true;
	record.execution_authorized = false;
	return record;
}

void test_success_and_duplicate_suppression() {
	const auto path = temporary_path(".sqlite3");
	auto provider = std::make_shared<ScriptedProvider>(std::vector<NotificationDispatchResult>{delivered()});
	NotificationProviderRegistry registry;
	registry.add("PAGER", provider);
	{
		NotificationDispatchRuntime runtime(configuration(), routing_policy(), std::move(registry), path.string());
		runtime.start();
		const auto routes = plan({{"success", "PAGER"}});
		require(runtime.submit(routes) == 1, "eligible notification was not accepted");
		require(runtime.submit(routes) == 0, "duplicate notification intent was accepted");
		const auto record = wait_latest(path, routes.intents[0].dedup_key, "DELIVERED");
		require(record.terminal && !record.execution_authorized, "delivered evidence was not safely terminal");
		runtime.stop();
	}
	require(provider->calls() == 1, "duplicate intent caused a second provider call");
	cleanup_database(path);
}

void test_provider_rejection_and_unknown_provider_are_terminal() {
	for (const bool configured : {true, false}) {
		const auto path = temporary_path(configured ? "_reject.sqlite3" : "_unknown.sqlite3");
		NotificationProviderRegistry registry;
		if (configured) registry.add("PAGER", std::make_shared<ScriptedProvider>(
			std::vector<NotificationDispatchResult>{failed("REJECTED", false)}));
		NotificationDispatchRuntime runtime(configuration(), routing_policy(), std::move(registry), path.string());
		runtime.start();
		const auto routes = plan({{configured ? "rejected" : "unknown", "PAGER"}});
		runtime.submit(routes);
		const auto record = wait_latest(path, routes.intents[0].dedup_key, "FAILED");
		require(record.terminal, "non-retryable provider failure was not terminal");
		require(!record.execution_authorized, "terminal provider failure granted execution authority");
		runtime.stop();
		cleanup_database(path);
	}
}

void test_timeout_retry_and_max_attempts() {
	{
		const auto path = temporary_path("_retry.sqlite3");
		auto provider = std::make_shared<ScriptedProvider>(
			std::vector<NotificationDispatchResult>{failed("TIMEOUT", true), delivered("retry-ok")});
		NotificationProviderRegistry registry;
		registry.add("PAGER", provider);
		NotificationDispatchRuntime runtime(configuration(), routing_policy(), std::move(registry), path.string());
		runtime.start();
		const auto routes = plan({{"retry", "PAGER"}});
		runtime.submit(routes);
		const auto first = wait_latest(path, routes.intents[0].dedup_key, "FAILED");
		require(!first.terminal && first.retry_backoff_seconds == 5, "retryable timeout did not retain 5 second backoff");
		const auto final = wait_latest(path, routes.intents[0].dedup_key, "DELIVERED", 7000ms);
		require(final.attempt == 2, "retry did not advance provider attempt");
		require(runtime.metrics_json()["timeout_count"] == 1, "timeout metric was not recorded");
		runtime.stop();
		cleanup_database(path);
	}
	{
		const auto path = temporary_path("_max.sqlite3");
		NotificationProviderRegistry registry;
		registry.add("PAGER", std::make_shared<ScriptedProvider>(
			std::vector<NotificationDispatchResult>{failed("TEMP", true)}));
		NotificationDispatchRuntime runtime(configuration(8, 1), routing_policy(), std::move(registry), path.string());
		runtime.start();
		const auto routes = plan({{"max", "PAGER"}});
		runtime.submit(routes);
		const auto record = wait_latest(path, routes.intents[0].dedup_key, "FAILED");
		require(record.terminal && record.attempt == 1, "maximum attempt budget was not enforced");
		runtime.stop();
		cleanup_database(path);
	}
}

void test_restart_recovery_and_delivered_suppression() {
	{
		const auto path = temporary_path("_pending.sqlite3");
		const auto routes = plan({{"pending-restart", "PAGER"}});
		seed(path, evidence(routes.intents[0].dedup_key, "PENDING", 0, false));
		auto provider = std::make_shared<ScriptedProvider>(std::vector<NotificationDispatchResult>{delivered("recovered")});
		NotificationProviderRegistry registry;
		registry.add("PAGER", provider);
		NotificationDispatchRuntime runtime(configuration(), routing_policy(), std::move(registry), path.string());
		runtime.start();
		const auto record = wait_latest(path, routes.intents[0].dedup_key, "DELIVERED");
		require(record.attempt == 1, "pending restart recovery changed the initial attempt");
		runtime.stop();
		cleanup_database(path);
	}
	{
		const auto path = temporary_path("_failed-restart.sqlite3");
		const auto routes = plan({{"failed-restart", "PAGER"}});
		auto retryable = evidence(routes.intents[0].dedup_key, "FAILED", 1, false);
		retryable.failure_code = "TIMEOUT";
		retryable.failure_reason = "temporary provider timeout";
		seed(path, retryable);
		auto provider = std::make_shared<ScriptedProvider>(std::vector<NotificationDispatchResult>{delivered("retry-recovered")});
		NotificationProviderRegistry registry;
		registry.add("PAGER", provider);
		NotificationDispatchRuntime runtime(configuration(), routing_policy(), std::move(registry), path.string());
		runtime.start();
		const auto record = wait_latest(path, routes.intents[0].dedup_key, "DELIVERED", 7000ms);
		require(record.attempt == 2, "restart after retryable failure did not resume the next attempt");
		runtime.stop();
		require(provider->calls() == 1, "restart after retryable failure dispatched an unexpected number of attempts");
		cleanup_database(path);
	}
	{
		const auto path = temporary_path("_dispatched.sqlite3");
		const auto routes = plan({{"dispatched-restart", "PAGER"}});
		seed(path, evidence(routes.intents[0].dedup_key, "DISPATCHED", 1, false));
		NotificationProviderRegistry registry;
		registry.add("PAGER", std::make_shared<ScriptedProvider>(std::vector<NotificationDispatchResult>{delivered("after-restart")}));
		NotificationDispatchRuntime runtime(configuration(), routing_policy(), std::move(registry), path.string());
		runtime.start();
		const auto interrupted = wait_latest(path, routes.intents[0].dedup_key, "FAILED");
		require(interrupted.failure_code == "INTERRUPTED" && interrupted.retry_backoff_seconds == 5,
			"interrupted dispatch was not converted to restart-safe retry evidence");
		runtime.stop();
		cleanup_database(path);
	}
	{
		const auto path = temporary_path("_exhausted-restart.sqlite3");
		const auto routes = plan({{"exhausted-restart", "PAGER"}});
		seed(path, evidence(routes.intents[0].dedup_key, "DISPATCHED", 3, false));
		auto provider = std::make_shared<ScriptedProvider>(std::vector<NotificationDispatchResult>{delivered()});
		NotificationProviderRegistry registry;
		registry.add("PAGER", provider);
		NotificationDispatchRuntime runtime(configuration(), routing_policy(), std::move(registry), path.string());
		runtime.start();
		const auto record = wait_latest(path, routes.intents[0].dedup_key, "FAILED");
		require(record.terminal && record.failure_code == "INTERRUPTED", "restart exceeded the persisted attempt budget");
		std::this_thread::sleep_for(50ms);
		runtime.stop();
		require(provider->calls() == 0, "exhausted restart state called provider again");
		cleanup_database(path);
	}
	{
		const auto path = temporary_path("_delivered.sqlite3");
		const auto routes = plan({{"delivered-restart", "PAGER"}});
		auto delivered_record = evidence(routes.intents[0].dedup_key, "DELIVERED", 1, true);
		delivered_record.provider_reference = "already-done";
		seed(path, delivered_record);
		auto provider = std::make_shared<ScriptedProvider>(std::vector<NotificationDispatchResult>{delivered()});
		NotificationProviderRegistry registry;
		registry.add("PAGER", provider);
		NotificationDispatchRuntime runtime(configuration(), routing_policy(), std::move(registry), path.string());
		runtime.start();
		require(runtime.submit(routes) == 0, "delivered restart state accepted duplicate work");
		std::this_thread::sleep_for(50ms);
		runtime.stop();
		require(provider->calls() == 0, "delivered restart state called provider again");
		cleanup_database(path);
	}
}

void test_queue_saturation_creates_explicit_failure() {
	const auto path = temporary_path("_queue.sqlite3");
	auto provider = std::make_shared<GateProvider>();
	NotificationProviderRegistry registry;
	registry.add("PAGER", provider);
	NotificationDispatchRuntime runtime(configuration(1), routing_policy(), std::move(registry), path.string());
	runtime.start();
	const auto first = plan({{"queue-active", "PAGER"}});
	runtime.submit(first);
	const auto active_deadline = std::chrono::steady_clock::now() + 1s;
	while (!provider->active() && std::chrono::steady_clock::now() < active_deadline) std::this_thread::sleep_for(1ms);
	require(provider->active(), "gate provider did not become active");

	const auto overflow = plan({{"queue-waiting", "PAGER"}, {"queue-rejected", "PAGER"}});
	require(runtime.submit(overflow) == 1, "bounded queue did not accept exactly one waiting item");
	const auto rejected = wait_latest(path, overflow.intents[1].dedup_key, "FAILED");
	require(rejected.terminal && rejected.failure_code == "QUEUE_SATURATED", "queue rejection did not create terminal evidence");
	require(runtime.metrics_json()["queue_rejections"] == 1, "queue rejection metric mismatch");
	provider->release();
	(void)wait_latest(path, first.intents[0].dedup_key, "DELIVERED");
	runtime.stop();
	cleanup_database(path);
}

void test_provider_exception_and_shutdown_cancellation() {
	{
		const auto path = temporary_path("_exception.sqlite3");
		NotificationProviderRegistry registry;
		registry.add("PAGER", std::make_shared<ThrowingProvider>());
		NotificationDispatchRuntime runtime(configuration(), routing_policy(), std::move(registry), path.string());
		runtime.start();
		const auto routes = plan({{"exception", "PAGER"}});
		runtime.submit(routes);
		const auto record = wait_latest(path, routes.intents[0].dedup_key, "FAILED");
		require(!record.terminal && record.failure_code == "PROVIDER_EXCEPTION", "provider exception was not explicit retriable evidence");
		runtime.stop();
		cleanup_database(path);
	}
	{
		const auto path = temporary_path("_cancel.sqlite3");
		auto provider = std::make_shared<GateProvider>();
		NotificationProviderRegistry registry;
		registry.add("PAGER", provider);
		NotificationDispatchRuntime runtime(configuration(), routing_policy(), std::move(registry), path.string());
		runtime.start();
		const auto routes = plan({{"cancel", "PAGER"}});
		runtime.submit(routes);
		const auto deadline = std::chrono::steady_clock::now() + 1s;
		while (!provider->active() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
		require(provider->active(), "cancellation provider did not become active");
		runtime.stop();
		const auto record = wait_latest(path, routes.intents[0].dedup_key, "FAILED");
		require(!record.terminal && record.failure_code == "CANCELLED", "shutdown cancellation did not remain restart-recoverable");
		cleanup_database(path);
	}
}

void test_configuration_validation() {
	const auto path = temporary_path(".json");
	{
		std::ofstream file(path);
		file << R"({
			"schema_version":1,
			"enabled":true,
			"queue_capacity":4,
			"worker_count":1,
			"max_attempts":3,
			"snapshot_poll_interval_ms":100,
			"recovery_limit":16,
			"enabled_channels":["PAGER"],
			"providers":[{"channel":"PAGER","type":"HTTP_JSON","endpoint":"http://insecure.invalid","connect_timeout_ms":100,"request_timeout_ms":200}]
		})";
	}
	bool rejected = false;
	try {
		(void)sentum::operations::load_notification_dispatch_configuration(path.string());
	} catch (const std::runtime_error&) {
		rejected = true;
	}
	require(rejected, "insecure notification provider endpoint passed validation");
	std::error_code error;
	std::filesystem::remove(path, error);
}

void test_final_evidence_drives_existing_operations_and_incident_projections() {
	const auto path = temporary_path("_projection.sqlite3");
	NotificationProviderRegistry registry;
	NotificationDispatchRuntime runtime(configuration(), routing_policy(), std::move(registry), path.string());
	runtime.start();
	const auto routes = plan({{"projection", "PAGER"}});
	runtime.submit(routes);
	(void)wait_latest(path, routes.intents[0].dedup_key, "FAILED");
	runtime.stop();

	NotificationDeliveryEvidenceRepository reader(path.string(), NotificationDeliveryEvidenceOpenMode::ReadOnly);
	const auto operations = sentum::operations::derive_notification_operations_view(reader);
	require(operations.health == NotificationOperationsHealth::IncidentCandidate && operations.terminal_failed == 1,
		"final dispatch evidence did not reach notification operations projection");
	const auto candidate = sentum::operations::derive_notification_incident_candidate(reader);
	require(candidate.state == NotificationIncidentCandidateState::ProposalReady,
		"final dispatch evidence did not reach governed incident candidate projection");
	require(!candidate.incident_authorized && !candidate.execution_authorized,
		"notification projection gained incident or execution authority");
	cleanup_database(path);
}

} // namespace

int main() {
	try {
		test_success_and_duplicate_suppression();
		test_provider_rejection_and_unknown_provider_are_terminal();
		test_timeout_retry_and_max_attempts();
		test_restart_recovery_and_delivered_suppression();
		test_queue_saturation_creates_explicit_failure();
		test_provider_exception_and_shutdown_cancellation();
		test_configuration_validation();
		test_final_evidence_drives_existing_operations_and_incident_projections();
		std::cout << "notification dispatch runtime tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "notification dispatch runtime test failure: " << error.what() << '\n';
		return 1;
	}
}
