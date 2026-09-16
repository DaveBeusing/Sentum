#include <sentum/ui/OperatorAlertCenterView.hpp>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

nlohmann::json healthy_snapshot() {
	return {
		{"health", "healthy"},
		{"market_data_connected", true},
		{"entries_paused", false},
		{"kill_switch_active", false},
		{"performance", {{"queue_pressure", "normal"}}},
		{"operations_control_plane", {
			{"governance_state", "CONTROLLED"},
			{"evidence_status", "AVAILABLE"},
			{"incident_state", "NONE"},
			{"pending_approvals", 0}
		}}
	};
}

void test_current_alerts_are_presented_read_only() {
	auto snapshot = healthy_snapshot();
	snapshot["kill_switch_active"] = true;
	const auto view = sentum::ui::derive_operator_alert_center_view(snapshot);
	require(view.total == 1, "kill-switch alert missing");
	require(view.active == 1, "active alert count mismatch");
	require(view.critical == 1, "critical count mismatch");
	require(view.items.front().state == "ACTIVE", "current alert should render active");
	require(!view.items.front().execution_authorized && !view.execution_authorized, "alert center granted execution authority");
}

void test_acknowledgement_evidence_is_visible() {
	auto snapshot = healthy_snapshot();
	snapshot["kill_switch_active"] = true;
	snapshot["operations_control_plane"]["alert_acknowledgements"] = nlohmann::json::array({{
		{"alert_id", "runtime.kill_switch"},
		{"generation", 1},
		{"actor", "operator-a"},
		{"reason", "incident under review"}
	}});
	const auto view = sentum::ui::derive_operator_alert_center_view(snapshot);
	require(view.acknowledged == 1, "acknowledged count mismatch");
	require(view.items.front().state == "ACKNOWLEDGED", "acknowledgement evidence not presented");
	require(view.items.front().acknowledged_by == "operator-a", "ack actor missing");
}

void test_persisted_lifecycle_can_show_cleared_history() {
	auto snapshot = healthy_snapshot();
	snapshot["operations_control_plane"]["alert_lifecycle"] = nlohmann::json::array({
		{
			{"id", "market.disconnected"},
			{"source", "MARKET_DATA"},
			{"severity", "WARNING"},
			{"state", "CLEARED"},
			{"generation", 2},
			{"title", "Market data disconnected"},
			{"message", "Connectivity restored"},
			{"recommended_workspace", "SYSTEM"}
		},
		{
			{"id", "runtime.kill_switch"},
			{"source", "RUNTIME"},
			{"severity", "CRITICAL"},
			{"state", "ACKNOWLEDGED"},
			{"generation", 3},
			{"title", "Kill switch active"},
			{"acknowledged_by", "operator-b"},
			{"acknowledgement_reason", "triage"}
		}
	});
	const auto view = sentum::ui::derive_operator_alert_center_view(snapshot);
	require(view.total == 2, "persisted lifecycle total mismatch");
	require(view.active == 1, "persisted active count mismatch");
	require(view.acknowledged == 1, "persisted acknowledged count mismatch");
	require(view.cleared == 1, "cleared history count mismatch");
	require(view.items.front().id == "runtime.kill_switch", "active alert should sort before cleared history");
}

void test_bounded_view_reports_truncation() {
	auto snapshot = healthy_snapshot();
	snapshot["operations_control_plane"]["alert_lifecycle"] = nlohmann::json::array({
		{{"id", "a"}, {"severity", "WARNING"}, {"state", "ACTIVE"}},
		{{"id", "b"}, {"severity", "ATTENTION"}, {"state", "ACTIVE"}},
		{{"id", "c"}, {"severity", "INFO"}, {"state", "CLEARED"}}
	});
	const auto view = sentum::ui::derive_operator_alert_center_view(snapshot, 2);
	require(view.total == 3, "total must include truncated items");
	require(view.items.size() == 2, "visible alert center limit ignored");
	require(view.truncated, "truncation flag missing");
}

} // namespace

int main() {
	try {
		test_current_alerts_are_presented_read_only();
		test_acknowledgement_evidence_is_visible();
		test_persisted_lifecycle_can_show_cleared_history();
		test_bounded_view_reports_truncation();
		std::cout << "operator alert center view tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "operator alert center view test failure: " << error.what() << '\n';
		return 1;
	}
}
