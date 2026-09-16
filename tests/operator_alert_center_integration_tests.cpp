#include <sentum/ui/CrossSurfaceOperationsView.hpp>
#include <sentum/ui/OperatorAlertCenterView.hpp>
#include <sentum/ui/TerminalOperatorSurfaceRenderer.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

nlohmann::json snapshot_with_alerts() {
	return {
		{"health", "healthy"},
		{"market_data_connected", false},
		{"entries_paused", true},
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

void test_cross_surface_alert_contract_matches_shared_view() {
	const auto snapshot = snapshot_with_alerts();
	const auto center = sentum::ui::derive_operator_alert_center_view(snapshot, 12);
	const auto operations = sentum::ui::derive_cross_surface_operations_view(snapshot, 8, 12, 12);
	const auto alerts = operations.at("alerts");
	require(alerts.at("total").get<std::size_t>() == center.total, "alert total drifted across surfaces");
	require(alerts.at("active").get<std::size_t>() == center.active, "active count drifted across surfaces");
	require(alerts.at("warning").get<std::size_t>() == center.warning, "warning count drifted across surfaces");
	require(alerts.at("attention").get<std::size_t>() == center.attention, "attention count drifted across surfaces");
	require(alerts.at("items").size() == center.items.size(), "alert item count drifted across surfaces");
	for (std::size_t index = 0; index < center.items.size(); ++index) {
		const auto& expected = center.items[index];
		const auto& actual = alerts.at("items").at(index);
		require(actual.at("id") == expected.id, "alert id drifted across surfaces");
		require(actual.at("severity") == expected.severity, "alert severity drifted across surfaces");
		require(actual.at("state") == expected.state, "alert state drifted across surfaces");
		require(actual.at("execution_authorized") == false, "web alert contract granted execution authority");
	}
}

void test_terminal_renders_same_alerts() {
	const auto snapshot = snapshot_with_alerts();
	const auto center = sentum::ui::derive_operator_alert_center_view(snapshot, 6);
	const auto frame = sentum::ui::operator_surface_frame_lines(snapshot, "SYSTEM");
	require(frame.find("ALERT CENTER total=" + std::to_string(center.total)) != std::string::npos, "terminal alert center count missing");
	for (const auto& item : center.items) {
		require(frame.find(item.id) != std::string::npos, "terminal omitted shared alert id");
		require(frame.find(item.state) != std::string::npos, "terminal omitted shared alert lifecycle state");
	}
}

void test_acknowledged_and_cleared_states_cross_surfaces() {
	auto snapshot = snapshot_with_alerts();
	snapshot["operations_control_plane"]["alert_lifecycle"] = nlohmann::json::array({
		{{"id", "runtime.health"}, {"source", "RUNTIME"}, {"severity", "CRITICAL"}, {"state", "ACKNOWLEDGED"}, {"generation", 2}, {"title", "Runtime health degraded"}, {"acknowledged_by", "operator-c"}},
		{{"id", "market.disconnected"}, {"source", "MARKET_DATA"}, {"severity", "WARNING"}, {"state", "CLEARED"}, {"generation", 1}, {"title", "Market data disconnected"}}
	});
	const auto operations = sentum::ui::derive_cross_surface_operations_view(snapshot);
	const auto alerts = operations.at("alerts");
	require(alerts.at("acknowledged") == 1, "acknowledged alert not exposed to web contract");
	require(alerts.at("cleared") == 1, "cleared alert not exposed to web contract");
	const auto frame = sentum::ui::operator_surface_frame_lines(snapshot, "SYSTEM");
	require(frame.find("ACKNOWLEDGED") != std::string::npos, "terminal did not render acknowledged state");
	require(frame.find("CLEARED") != std::string::npos, "terminal did not render cleared state");
}

} // namespace

int main() {
	try {
		test_cross_surface_alert_contract_matches_shared_view();
		test_terminal_renders_same_alerts();
		test_acknowledged_and_cleared_states_cross_surfaces();
		std::cout << "operator alert center integration tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "operator alert center integration test failure: " << error.what() << '\n';
		return 1;
	}
}
