#include <sentum/ui/TerminalWorkspacePolicy.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using sentum::ui::OperatorSeverity;
using sentum::ui::derive_operator_control_surface;
using sentum::ui::derive_operator_status;
using sentum::ui::operator_action_presentation;
using sentum::ui::operator_banner_text;
using sentum::ui::workspace_for_key;
using sentum::ui::workspace_help_text;
using sentum::ui::workspace_navigation_text;

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

nlohmann::json healthy_snapshot() {
	return {
		{"health", "healthy"},
		{"kill_switch_active", false},
		{"market_data_connected", true},
		{"entries_paused", false},
		{"performance", {{"queue_pressure", "normal"}}}
	};
}

void test_operator_priority_order() {
	auto snapshot = healthy_snapshot();
	auto status = derive_operator_status(snapshot);
	require(status.severity == OperatorSeverity::Normal, "healthy snapshot must be normal");

	snapshot["entries_paused"] = true;
	status = derive_operator_status(snapshot);
	require(status.severity == OperatorSeverity::Attention, "paused entries must require attention");
	require(status.recommended_workspace == "MARKET", "paused entries should direct operator to market workspace");

	snapshot["performance"]["queue_pressure"] = "critical";
	status = derive_operator_status(snapshot);
	require(status.severity == OperatorSeverity::Warning, "critical persistence pressure must outrank paused entries");
	require(status.recommended_workspace == "SYSTEM", "persistence pressure should direct operator to system workspace");

	snapshot["market_data_connected"] = false;
	status = derive_operator_status(snapshot);
	require(status.message == "Market data disconnected", "market disconnect must outrank persistence pressure");

	snapshot["kill_switch_active"] = true;
	status = derive_operator_status(snapshot);
	require(status.severity == OperatorSeverity::Critical, "kill switch must be critical");
	require(status.message == "Kill switch active", "kill switch message mismatch");
}

void test_workspace_contract() {
	const auto* market = workspace_for_key('1');
	const auto* system = workspace_for_key('7');
	require(market != nullptr && market->name == "MARKET", "market workspace key mismatch");
	require(system != nullptr && system->name == "SYSTEM", "system workspace key mismatch");
	require(workspace_for_key('8') == nullptr, "unexpected workspace for unsupported key");
}

void test_operator_presentation_contract() {
	auto status = derive_operator_status(healthy_snapshot());
	require(operator_banner_text(status) == "NORMAL", "normal banner text mismatch");

	auto warning = healthy_snapshot();
	warning["market_data_connected"] = false;
	status = derive_operator_status(warning);
	require(operator_banner_text(status) == "WARNING | Market data disconnected | Inspect SYSTEM", "warning banner text mismatch");

	const auto navigation = workspace_navigation_text("TRADES");
	require(navigation.find("[1] MARKET") != std::string::npos, "market shortcut missing from navigation");
	require(navigation.find("[4] TRADES*") != std::string::npos, "active workspace marker missing from navigation");
	require(navigation.find("[7] SYSTEM") != std::string::npos, "system shortcut missing from navigation");

	require(workspace_help_text("SYSTEM") == "SYSTEM | latency, queue, persistence and runtime health", "system workspace help mismatch");
	require(workspace_help_text("UNKNOWN").empty(), "unknown workspace must not produce help text");
}

void test_control_surface_contract() {
	auto snapshot = healthy_snapshot();
	snapshot["operations_control_plane"] = {
		{"governance_state", "CONTROLLED"},
		{"maintenance_state", "NORMAL"},
		{"incident_state", "NONE"},
		{"recovery_state", "IDLE"},
		{"pending_approvals", 2},
		{"last_audit_action", "enter_maintenance"},
		{"last_audit_actor", "operator-a"}
	};

	const auto surface = derive_operator_control_surface(snapshot, "SYSTEM");
	require(surface.status.severity == OperatorSeverity::Normal, "healthy control surface must preserve normal runtime status");
	require(surface.active_workspace == "SYSTEM", "control surface active workspace mismatch");
	require(surface.navigation.find("[7] SYSTEM*") != std::string::npos, "control surface active workspace marker missing");
	require(surface.governance_state == "CONTROLLED", "governance state mismatch");
	require(surface.pending_approvals == 2, "pending approval count mismatch");
	require(surface.approval_summary == "2 approval(s) pending", "approval summary mismatch");
	require(surface.audit_summary == "Last action: enter_maintenance by operator-a", "audit summary mismatch");
}

void test_control_surface_missing_governance_fails_visible() {
	const auto surface = derive_operator_control_surface(healthy_snapshot(), "MARKET");
	require(surface.governance_state == "UNAVAILABLE", "missing governance evidence must remain visible");
	require(surface.approval_summary == "Governance evidence unavailable", "missing governance evidence summary mismatch");
}

void test_action_presentation_uses_upstream_classification() {
	const auto automated = operator_action_presentation("AUTOMATED");
	require(!automated.blocked && !automated.confirmation_required, "automated action presentation mismatch");

	const auto approval = operator_action_presentation("APPROVAL_REQUIRED");
	require(!approval.blocked && approval.confirmation_required, "approval-required presentation mismatch");
	require(approval.label == "APPROVAL REQUIRED", "approval-required label mismatch");

	const auto forbidden = operator_action_presentation("FORBIDDEN");
	require(forbidden.blocked, "forbidden action must render blocked");

	const auto unknown = operator_action_presentation("UNKNOWN");
	require(unknown.blocked, "unknown action class must fail closed");
	require(unknown.classification == "FORBIDDEN", "unknown action class must present forbidden default");
}

} // namespace

int main() {
	try {
		test_operator_priority_order();
		test_workspace_contract();
		test_operator_presentation_contract();
		test_control_surface_contract();
		test_control_surface_missing_governance_fails_visible();
		test_action_presentation_uses_upstream_classification();
		std::cout << "terminal workspace policy tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "terminal workspace policy test failure: " << error.what() << '\n';
		return 1;
	}
}
