#include <sentum/ui/OperatorAlertPolicy.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

const sentum::ui::OperatorAlert* find_alert(
	const std::vector<sentum::ui::OperatorAlert>& alerts,
	const std::string& id) {
	for (const auto& alert : alerts) {
		if (alert.id == id) return &alert;
	}
	return nullptr;
}

nlohmann::json healthy_snapshot() {
	return {
		{"health", "healthy"},
		{"kill_switch_active", false},
		{"market_data_connected", true},
		{"entries_paused", false},
		{"performance", {{"queue_pressure", "normal"}}},
		{"operations_control_plane", {
			{"governance_state", "CONTROLLED"},
			{"evidence_status", "AVAILABLE"},
			{"incident_state", "NONE"},
			{"pending_approvals", 0}
		}}
	};
}

void test_healthy_snapshot_has_no_alerts() {
	const auto alerts = sentum::ui::derive_operator_alerts(healthy_snapshot());
	require(alerts.empty(), "healthy snapshot unexpectedly produced alerts");
}

void test_kill_switch_is_critical_and_acknowledged_out_of_band() {
	auto snapshot = healthy_snapshot();
	snapshot["kill_switch_active"] = true;
	const auto alerts = sentum::ui::derive_operator_alerts(snapshot);
	const auto* alert = find_alert(alerts, "runtime.kill_switch");
	require(alert != nullptr, "kill-switch alert missing");
	require(alert->severity == sentum::ui::OperatorAlertSeverity::Critical, "kill-switch alert is not critical");
	require(alert->acknowledgement_required, "kill-switch alert must require acknowledgement");
	require(alert->escalation_level == 3, "kill-switch alert must be escalation level 3");
	require(alert->notification_candidate, "kill-switch alert should be notification candidate");
	require(!alert->execution_authorized, "alert presentation must never authorize execution");
}

void test_runtime_and_market_failures_are_prioritized() {
	auto snapshot = healthy_snapshot();
	snapshot["health"] = "unhealthy";
	snapshot["market_data_connected"] = false;
	const auto alerts = sentum::ui::derive_operator_alerts(snapshot);
	require(alerts.size() >= 2, "expected runtime and market alerts");
	require(alerts.front().severity == sentum::ui::OperatorAlertSeverity::Critical, "critical alert was not ordered first");
	const auto* market = find_alert(alerts, "market.disconnected");
	require(market != nullptr && market->escalation_level == 2, "market disconnect alert escalation mismatch");
}

void test_stale_governance_is_fail_closed_notification_candidate() {
	auto snapshot = healthy_snapshot();
	snapshot["operations_control_plane"]["evidence_status"] = "STALE";
	const auto alerts = sentum::ui::derive_operator_alerts(snapshot);
	const auto* alert = find_alert(alerts, "governance.evidence_stale");
	require(alert != nullptr, "stale governance alert missing");
	require(alert->severity == sentum::ui::OperatorAlertSeverity::Warning, "stale governance must be warning");
	require(alert->acknowledgement_required, "stale governance must require acknowledgement");
	require(alert->notification_candidate, "stale governance should be a notification candidate");
	require(alert->guidance.find("Do not rely on stale approval state") != std::string::npos, "stale guidance lost fail-closed wording");
}

void test_missing_governance_is_visible() {
	auto snapshot = healthy_snapshot();
	snapshot.erase("operations_control_plane");
	const auto alerts = sentum::ui::derive_operator_alerts(snapshot);
	const auto* alert = find_alert(alerts, "governance.unavailable");
	require(alert != nullptr, "missing governance alert missing");
	require(alert->severity == sentum::ui::OperatorAlertSeverity::Warning, "missing governance severity mismatch");
}

void test_pending_approvals_and_paused_entries_are_attention_only() {
	auto snapshot = healthy_snapshot();
	snapshot["entries_paused"] = true;
	snapshot["operations_control_plane"]["pending_approvals"] = 2;
	const auto alerts = sentum::ui::derive_operator_alerts(snapshot);
	const auto* approvals = find_alert(alerts, "operations.approvals_pending");
	const auto* paused = find_alert(alerts, "runtime.entries_paused");
	require(approvals != nullptr && paused != nullptr, "attention alerts missing");
	require(approvals->severity == sentum::ui::OperatorAlertSeverity::Attention, "pending approvals severity mismatch");
	require(paused->severity == sentum::ui::OperatorAlertSeverity::Attention, "paused entries severity mismatch");
	require(!approvals->notification_candidate && !paused->notification_candidate, "attention alerts should not notify by default");
	require(!approvals->acknowledgement_required && !paused->acknowledgement_required, "attention alerts should not require acknowledgement");
}

void test_incident_is_warning_without_local_resolution_authority() {
	auto snapshot = healthy_snapshot();
	snapshot["operations_control_plane"]["incident_state"] = "OPEN";
	const auto alerts = sentum::ui::derive_operator_alerts(snapshot);
	const auto* alert = find_alert(alerts, "operations.incident");
	require(alert != nullptr, "incident alert missing");
	require(alert->acknowledgement_required, "incident should require acknowledgement");
	require(!alert->execution_authorized, "incident alert presentation must not authorize resolution");
}

} // namespace

int main() {
	try {
		test_healthy_snapshot_has_no_alerts();
		test_kill_switch_is_critical_and_acknowledged_out_of_band();
		test_runtime_and_market_failures_are_prioritized();
		test_stale_governance_is_fail_closed_notification_candidate();
		test_missing_governance_is_visible();
		test_pending_approvals_and_paused_entries_are_attention_only();
		test_incident_is_warning_without_local_resolution_authority();
		std::cout << "operator alert policy tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "operator alert policy test failure: " << error.what() << '\n';
		return 1;
	}
}
