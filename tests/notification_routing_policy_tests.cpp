#include <sentum/operations/NotificationDispatchRuntime.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;
using namespace sentum::operations;

void require(bool condition, const std::string& message) {
	if (!condition) throw std::runtime_error(message);
}

struct TemporaryDatabase {
	std::filesystem::path path = std::filesystem::temp_directory_path() /
		("sentum_notification_policy_" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite");
	~TemporaryDatabase() {
		std::error_code ignored;
		for (const auto* suffix : {"", "-wal", "-shm"}) {
			std::filesystem::remove(path.string() + suffix, ignored);
		}
	}
};

class CountingProvider final : public INotificationProvider {
public:
	NotificationDispatchResult dispatch(const NotificationDispatchRequest& request, const std::atomic<bool>&) override {
		require(!request.execution_authorized, "routing granted execution authority");
		calls.fetch_add(1);
		NotificationDispatchResult result;
		result.accepted = true;
		result.delivered = true;
		return result;
	}
	std::atomic<unsigned> calls{0};
};

json policy() {
	return {
		{"schema_version", 1}, {"authority", "NOTIFICATION_ROUTING_ONLY"}, {"default_action", "BLOCK"},
		{"allowed_channels", {"PAGER", "EMAIL"}},
		{"routes", json::array({{
			{"severity", "WARNING"}, {"minimum_escalation_level", 2},
			{"channels", {"PAGER"}}, {"audiences", {"OPS"}}
		}})}
	};
}

json disconnected_snapshot() {
	return {{"health", "healthy"}, {"market_data_connected", false},
		{"operations_control_plane", {{"governance_state", "CONTROLLED"}, {"evidence_status", "AVAILABLE"}}}};
}

NotificationDispatchConfiguration configuration() {
	NotificationDispatchConfiguration result;
	result.enabled = true;
	result.worker_count = 1;
	result.snapshot_poll_interval_ms = 100;
	result.enabled_channels = {"PAGER"};
	return result;
}

void test_valid_policy_and_boundaries() {
	const auto valid = policy();
	require(notification_policy_valid(valid), "valid policy rejected");
	const auto plan = derive_notification_routing_plan(disconnected_snapshot(), valid);
	require(plan.policy_valid && plan.eligible == 1 && plan.blocked == 0, "valid governed route not selected");
	require(plan.intents.front().dedup_key == "market.disconnected:g1:l2:PAGER:OPS", "valid delivery identity changed");
	require(plan.intents.front().delivery_authorized && !plan.execution_authorized &&
		!plan.intents.front().execution_authorized, "valid policy authority changed");
	require(notification_route_for(valid, "WARNING", 1) == nullptr, "route matched below minimum escalation");
	require(notification_route_for(valid, "WARNING", 2) != nullptr, "route failed at minimum escalation");
	require(notification_route_for(valid, "CRITICAL", 3) == nullptr, "route matched wrong severity");
	auto overlapping = valid;
	auto alternative = valid["routes"][0];
	alternative["channels"] = {"EMAIL"};
	alternative["audiences"] = {"ON_CALL"};
	overlapping["routes"].push_back(alternative);
	require(derive_notification_routing_plan(disconnected_snapshot(), overlapping).intents.front().channel == "PAGER",
		"overlapping routes did not retain first-match precedence");
	std::swap(overlapping["routes"][0], overlapping["routes"][1]);
	const auto reordered = derive_notification_routing_plan(disconnected_snapshot(), overlapping);
	require(reordered.eligible == 1 && reordered.intents.front().channel == "EMAIL" &&
		reordered.intents.front().audience == "ON_CALL", "configured route order was ignored");
	for (const auto* severity : {"INFO", "ATTENTION", "WARNING", "CRITICAL"}) {
		for (int level : {0, 3}) {
			auto boundary = valid;
			boundary["routes"][0]["severity"] = severity;
			boundary["routes"][0]["minimum_escalation_level"] = level;
			require(notification_policy_valid(boundary), "valid severity/escalation boundary rejected");
			boundary["routes"][0]["minimum_escalation_level"] = static_cast<std::uint64_t>(level);
			require(notification_policy_valid(boundary), "bounded unsigned escalation rejected");
		}
	}
	auto block_all = valid;
	block_all["routes"] = json::array();
	require(notification_policy_valid(block_all), "empty routes must remain a block-all policy");
	require(derive_notification_routing_plan(disconnected_snapshot(), block_all).eligible == 0, "empty routes authorized delivery");
	block_all["allowed_channels"] = json::array();
	require(notification_policy_valid(block_all), "empty allow-list and routes rejected");

	std::ifstream shipped(std::filesystem::path(__FILE__).parent_path().parent_path() / "config/notification_routing_policy.json");
	require(static_cast<bool>(shipped), "cannot load shipped routing policy");
	json shipped_policy;
	shipped >> shipped_policy;
	require(notification_policy_valid(shipped_policy), "shipped routing policy rejected");
}

std::vector<std::pair<std::string, json>> invalid_policies() {
	std::vector<std::pair<std::string, json>> result;
	auto route_field = [&](const char* field, const json& value) {
		auto invalid = policy();
		invalid["routes"][0][field] = value;
		result.emplace_back(std::string(field) + "=" + value.dump(), std::move(invalid));
	};
	for (const auto* field : {"severity", "minimum_escalation_level", "channels", "audiences"}) {
		auto missing = policy();
		missing["routes"][0].erase(field);
		result.emplace_back(std::string("missing ") + field, std::move(missing));
		for (const auto& malformed : std::vector<json>{nullptr, false, json::object()}) route_field(field, malformed);
	}
	for (const auto& severity : std::vector<json>{2, 2.5, "", "warning", "UNKNOWN", json::array({"WARNING"})}) {
		route_field("severity", severity);
	}
	for (const auto& level : std::vector<json>{"invalid", -1, 4, 2.0, 2.5,
		std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::int64_t>::min(),
		json::array({2})}) route_field("minimum_escalation_level", level);
	for (const auto* field : {"channels", "audiences"}) {
		for (const auto& values : std::vector<json>{"PAGER", json::array(), json::array({2}), json::array({""}),
			json::array({" "}), json::array({"OPS\n"}), json::array({"A:B"}),
			json::array({"PAGER", "PAGER"}), json::array({"PAGER", nullptr})}) route_field(field, values);
	}
	route_field("channels", json::array({"UNLISTED"}));
	auto malformed_later_route = policy();
	auto later_route = malformed_later_route["routes"][0];
	later_route["severity"] = "CRITICAL";
	later_route["minimum_escalation_level"] = "invalid";
	malformed_later_route["routes"].push_back(std::move(later_route));
	result.emplace_back("valid matching route followed by malformed route", std::move(malformed_later_route));
	for (const auto& entry : std::vector<json>{nullptr, 2, "route", json::array()}) {
		auto invalid = policy();
		invalid["routes"] = json::array({entry});
		result.emplace_back("malformed route entry", std::move(invalid));
	}
	for (const auto& channels : std::vector<json>{nullptr, "PAGER", json::array(), json::array({false}),
		json::array({""}), json::array({"PAGER", "PAGER"}), json::array({"PAGER", "BAD:CHANNEL"})}) {
		auto invalid = policy();
		invalid["allowed_channels"] = channels;
		result.emplace_back("malformed allowed_channels", std::move(invalid));
	}
	for (const auto& schema : std::vector<json>{nullptr, false, 1.0, "1", 0, 2, std::numeric_limits<std::uint64_t>::max()}) {
		auto invalid = policy();
		invalid["schema_version"] = schema;
		result.emplace_back("invalid schema version", std::move(invalid));
	}
	for (const auto* field : {"schema_version", "authority", "default_action", "allowed_channels", "routes"}) {
		auto missing = policy();
		missing.erase(field);
		result.emplace_back(std::string("missing ") + field, std::move(missing));
	}
	for (const auto* field : {"authority", "default_action", "routes"}) {
		auto invalid = policy();
		invalid[field] = 1;
		result.emplace_back(std::string("malformed ") + field, std::move(invalid));
	}
	for (const auto& invalid : std::vector<json>{nullptr, false, 1, "policy", json::array()}) {
		result.emplace_back("non-object policy", invalid);
	}
	return result;
}

void test_malformed_policies_cannot_abort_or_dispatch() {
	TemporaryDatabase database;
	auto provider = std::make_shared<CountingProvider>();
	std::atomic<unsigned> snapshots{0};
	const auto malformed = invalid_policies();
	for (const auto& entry : malformed) {
		const auto& name = entry.first;
		const auto& invalid = entry.second;
		require(!notification_policy_valid(invalid), name + ": invalid policy accepted");
		require(notification_route_for(invalid, "WARNING", 2) == nullptr, name + ": invalid lookup returned route");
		const auto plan = derive_notification_routing_plan(disconnected_snapshot(), invalid);
		require(!plan.policy_valid && plan.eligible == 0 && plan.intents.empty(), name + ": invalid plan authorized work");
		NotificationProviderRegistry registry;
		registry.add("PAGER", provider);
		bool rejected = false;
		try {
			NotificationDispatchRuntime runtime(configuration(), invalid, std::move(registry), database.path.string(), [&] {
				snapshots.fetch_add(1);
				return NotificationRuntimeSnapshot{1, disconnected_snapshot()};
			});
			runtime.start();
			runtime.stop();
		} catch (const std::invalid_argument& error) {
			rejected = std::string(error.what()).find("routing policy") != std::string::npos;
		}
		require(rejected, name + ": enabled runtime did not reject invalid routing configuration");
		require(provider->calls.load() == 0 && snapshots.load() == 0, name + ": invalid configuration started runtime work");
	}
	std::cout << "Rejected " << malformed.size() << " malformed routing policies without starting workers\n";
}

void test_valid_scheduler_still_dispatches_once() {
	TemporaryDatabase database;
	auto provider = std::make_shared<CountingProvider>();
	NotificationProviderRegistry registry;
	registry.add("PAGER", provider);
	NotificationDispatchRuntime runtime(configuration(), policy(), std::move(registry), database.path.string(), [] {
		return NotificationRuntimeSnapshot{1, disconnected_snapshot()};
	});
	runtime.start();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (runtime.metrics_json().value("delivered", 0U) == 0 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	runtime.stop();
	require(provider->calls.load() == 1, "valid scheduler did not dispatch exactly once");
	require(runtime.metrics_json().value("delivered", 0U) == 1, "valid provider result was not persisted");
}

} // namespace

int main() {
	try {
		test_valid_policy_and_boundaries();
		test_malformed_policies_cannot_abort_or_dispatch();
		test_valid_scheduler_still_dispatches_once();
		std::cout << "notification routing policy tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "notification routing policy test failure: " << error.what() << '\n';
		return 1;
	}
}
