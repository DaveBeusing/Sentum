#include <sentum/ui/OperatorAlertEscalationTimeline.hpp>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

nlohmann::json base_snapshot() {
	return {
		{"health", "healthy"},
		{"market_data_connected", false},
		{"entries_paused", false},
		{"kill_switch_active", false},
		{"performance", {{"queue_pressure", "normal"}}},
		{"operations_control_plane", {
			{"governance_state", "CONTROLLED"},
			{"evidence_status", "AVAILABLE"},
			{"incident_state", "NONE"},
			{"pending_approvals", 0},
			{"observed_at_utc", "2026-09-16T20:10:00Z"}
		}}
	};
}

void test_overdue_is_derived_from_canonical_evidence() {
	auto snapshot = base_snapshot();
	snapshot["operations_control_plane"]["alert_escalation"] = nlohmann::json::array({{
		{"alert_id", "market.disconnected"},
		{"first_seen_utc", "2026-09-16T20:00:00Z"},
		{"last_seen_utc", "2026-09-16T20:09:00Z"},
		{"next_escalation_at_utc", "2026-09-16T20:05:00Z"},
		{"timeline", nlohmann::json::array({{{"state", "LEVEL_2"}, {"actor", "policy"}, {"reason", "unacknowledged"}}})}
	}});
	const auto view = sentum::ui::derive_operator_alert_escalation_timeline(snapshot);
	require(view.overdue == 1, "overdue count mismatch");
	require(view.items.front().aging_state == "OVERDUE", "overdue state missing");
	require(view.items.front().latest_escalation == "LEVEL_2", "timeline evidence missing");
	require(!view.execution_authorized && !view.items.front().execution_authorized, "aging granted authority");
}

void test_due_and_fresh_states() {
	auto snapshot = base_snapshot();
	snapshot["operations_control_plane"]["alert_lifecycle"] = nlohmann::json::array({
		{{"id", "a"}, {"severity", "WARNING"}, {"state", "ACTIVE"}, {"title", "A"}},
		{{"id", "b"}, {"severity", "ATTENTION"}, {"state", "ACTIVE"}, {"title", "B"}}
	});
	snapshot["operations_control_plane"]["alert_escalation"] = nlohmann::json::array({
		{{"alert_id", "a"}, {"next_escalation_at_utc", "2026-09-16T20:10:00Z"}},
		{{"alert_id", "b"}, {"next_escalation_at_utc", "2026-09-16T20:20:00Z"}}
	});
	const auto view = sentum::ui::derive_operator_alert_escalation_timeline(snapshot);
	require(view.due == 1, "due count mismatch");
	require(view.items[0].aging_state == "DUE", "due state missing");
	require(view.items[1].aging_state == "FRESH", "fresh state missing");
}

void test_missing_or_invalid_timestamp_is_unavailable() {
	auto snapshot = base_snapshot();
	snapshot["operations_control_plane"]["observed_at_utc"] = "not-a-time";
	const auto view = sentum::ui::derive_operator_alert_escalation_timeline(snapshot);
	require(view.unavailable == view.total && view.total > 0, "invalid time must fail unavailable");
}

void test_acknowledged_and_cleared_override_age() {
	auto snapshot = base_snapshot();
	snapshot["operations_control_plane"]["alert_lifecycle"] = nlohmann::json::array({
		{{"id", "ack"}, {"severity", "CRITICAL"}, {"state", "ACKNOWLEDGED"}, {"title", "Ack"}},
		{{"id", "clear"}, {"severity", "WARNING"}, {"state", "CLEARED"}, {"title", "Clear"}}
	});
	snapshot["operations_control_plane"]["alert_escalation"] = nlohmann::json::array({
		{{"alert_id", "ack"}, {"next_escalation_at_utc", "2026-09-16T20:00:00Z"}},
		{{"alert_id", "clear"}, {"next_escalation_at_utc", "2026-09-16T20:00:00Z"}}
	});
	const auto view = sentum::ui::derive_operator_alert_escalation_timeline(snapshot);
	require(view.items[0].aging_state == "ACKNOWLEDGED", "ack override missing");
	require(view.items[1].aging_state == "CLEARED", "cleared override missing");
	require(view.overdue == 0, "ack/cleared must not count overdue");
}

} // namespace

int main() {
	try {
		test_overdue_is_derived_from_canonical_evidence();
		test_due_and_fresh_states();
		test_missing_or_invalid_timestamp_is_unavailable();
		test_acknowledged_and_cleared_override_age();
		std::cout << "operator alert escalation timeline tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "operator alert escalation timeline test failure: " << error.what() << '\n';
		return 1;
	}
}
