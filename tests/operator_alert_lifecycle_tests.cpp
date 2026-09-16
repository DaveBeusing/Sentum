#include <sentum/ui/OperatorAlertLifecycle.hpp>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

const sentum::ui::OperatorAlertLifecycleItem* find_item(
	const std::vector<sentum::ui::OperatorAlertLifecycleItem>& items,
	const char* id) {
	for (const auto& item : items) {
		if (item.alert.id == id) return &item;
	}
	return nullptr;
}

void test_new_active_and_clear_lifecycle() {
	using namespace sentum::ui;
	const auto alert = make_operator_alert(
		"runtime.health", "RUNTIME", OperatorAlertSeverity::Critical,
		"Runtime health degraded", "degraded", "inspect", "SYSTEM", true, 3, true);

	auto state = reconcile_operator_alert_lifecycle({}, {alert});
	require(state.size() == 1, "new alert missing");
	require(state[0].state == OperatorAlertLifecycleState::New, "first observation must be NEW");
	require(state[0].generation == 1, "first generation must be one");

	state = reconcile_operator_alert_lifecycle(state, {alert});
	require(state[0].state == OperatorAlertLifecycleState::Active, "repeated alert must become ACTIVE");
	require(state[0].observations == 2, "observation count not incremented");

	state = reconcile_operator_alert_lifecycle(state, {});
	require(state[0].state == OperatorAlertLifecycleState::Cleared, "disappeared alert must become CLEARED");
	require(!state[0].active, "cleared alert remained active");
}

void test_deduplicates_by_stable_alert_id() {
	using namespace sentum::ui;
	const auto first = make_operator_alert(
		"market.disconnected", "MARKET_DATA", OperatorAlertSeverity::Warning,
		"Market data disconnected", "first", "inspect", "SYSTEM", true, 2, true);
	const auto updated = make_operator_alert(
		"market.disconnected", "MARKET_DATA", OperatorAlertSeverity::Warning,
		"Market data disconnected", "updated", "inspect", "SYSTEM", true, 2, true);

	auto state = reconcile_operator_alert_lifecycle({}, {first});
	state = reconcile_operator_alert_lifecycle(state, {updated});
	require(state.size() == 1, "stable alert id was duplicated");
	require(state[0].alert.message == "updated", "latest presentation was not retained");
	require(state[0].observations == 2, "deduplicated alert did not advance observations");
}

void test_acknowledgement_requires_matching_generation() {
	using namespace sentum::ui;
	const auto alert = make_operator_alert(
		"runtime.kill_switch", "RUNTIME", OperatorAlertSeverity::Critical,
		"Kill switch active", "active", "inspect", "MARKET", true, 3, true);

	auto state = reconcile_operator_alert_lifecycle({}, {alert}, {
		{"runtime.kill_switch", 2, "operator-a", "wrong generation"}
	});
	require(state[0].state == OperatorAlertLifecycleState::New, "wrong generation acknowledgement was accepted");

	state = reconcile_operator_alert_lifecycle(state, {alert}, {
		{"runtime.kill_switch", 1, "operator-a", "seen"}
	});
	require(state[0].state == OperatorAlertLifecycleState::Acknowledged, "matching acknowledgement was not presented");
	require(state[0].acknowledged_by == "operator-a", "acknowledgement actor missing");
	require(!state[0].execution_authorized, "acknowledgement granted execution authority");
}

void test_reactivation_creates_new_generation_and_drops_old_ack() {
	using namespace sentum::ui;
	const auto alert = make_operator_alert(
		"operations.incident", "INCIDENT", OperatorAlertSeverity::Warning,
		"Operational incident active", "active", "inspect", "SYSTEM", true, 2, true);

	auto state = reconcile_operator_alert_lifecycle({}, {alert}, {
		{"operations.incident", 1, "operator-a", "acknowledged"}
	});
	require(state[0].state == OperatorAlertLifecycleState::Acknowledged, "initial acknowledgement missing");

	state = reconcile_operator_alert_lifecycle(state, {});
	require(state[0].state == OperatorAlertLifecycleState::Cleared, "alert did not clear");

	state = reconcile_operator_alert_lifecycle(state, {alert}, {
		{"operations.incident", 1, "operator-a", "old acknowledgement"}
	});
	require(state[0].generation == 2, "reactivation did not increment generation");
	require(state[0].state == OperatorAlertLifecycleState::New, "reactivation must return to NEW");
	require(state[0].acknowledged_by.empty(), "old acknowledgement leaked into new generation");
}

void test_snapshot_acknowledgement_evidence_is_read_only() {
	using namespace sentum::ui;
	nlohmann::json snapshot = {
		{"health", "unhealthy"},
		{"market_data_connected", true},
		{"operations_control_plane", {
			{"governance_state", "CONTROLLED"},
			{"evidence_status", "AVAILABLE"},
			{"alert_acknowledgements", nlohmann::json::array({
				{{"alert_id", "runtime.health"}, {"generation", 1}, {"actor", "operator-b"}, {"reason", "investigating"}}
			})}
		}}
	};

	const auto state = derive_operator_alert_lifecycle(snapshot);
	const auto* health = find_item(state, "runtime.health");
	require(health != nullptr, "runtime health alert missing");
	require(health->state == OperatorAlertLifecycleState::Acknowledged, "control-plane acknowledgement not projected");
	require(health->acknowledged_by == "operator-b", "projected acknowledgement actor missing");
	require(!health->execution_authorized, "projected acknowledgement authorized execution");
}

} // namespace

int main() {
	try {
		test_new_active_and_clear_lifecycle();
		test_deduplicates_by_stable_alert_id();
		test_acknowledgement_requires_matching_generation();
		test_reactivation_creates_new_generation_and_drops_old_ack();
		test_snapshot_acknowledgement_evidence_is_read_only();
		std::cout << "operator alert lifecycle tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "operator alert lifecycle test failure: " << error.what() << '\n';
		return 1;
	}
}
