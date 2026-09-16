#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <sentum/ui/OperatorActionFlow.hpp>
#include <sentum/ui/OperatorAlertCenterView.hpp>
#include <sentum/ui/OperatorAlertEscalationTimeline.hpp>
#include <sentum/ui/OperatorAuditQueueView.hpp>
#include <sentum/ui/OperatorNavigationPolicy.hpp>
#include <sentum/ui/OperatorWorkflowView.hpp>
#include <sentum/ui/TerminalWorkspacePolicy.hpp>

namespace sentum::ui {

inline std::string operator_surface_frame_lines(
	const nlohmann::json& snapshot,
	std::string_view active_workspace,
	const OperatorNavigationState& navigation = OperatorNavigationState{}) {
	const auto surface = derive_operator_control_surface(snapshot, active_workspace);
	std::string frame;
	frame.reserve(
		surface.banner.size() + surface.navigation.size() + surface.workspace_help.size() +
		surface.governance_state.size() + surface.maintenance_state.size() +
		surface.incident_state.size() + surface.recovery_state.size() +
		surface.approval_summary.size() + surface.audit_summary.size() + 3800);

	frame += "OPERATOR ";
	frame += surface.banner;
	frame += '\n';
	frame += surface.navigation;
	frame += '\n';
	frame += surface.workspace_help;
	frame += '\n';
	frame += "Governance ";
	frame += surface.governance_state;
	frame += "  Maintenance ";
	frame += surface.maintenance_state;
	frame += "  Incident ";
	frame += surface.incident_state;
	frame += "  Recovery ";
	frame += surface.recovery_state;
	frame += '\n';
	frame += surface.approval_summary;
	frame += "  |  ";
	frame += surface.audit_summary;
	frame += '\n';

	const auto alert_center = derive_operator_alert_center_view(snapshot, 6);
	frame += "ALERT CENTER total=";
	frame += std::to_string(alert_center.total);
	frame += " active=";
	frame += std::to_string(alert_center.active);
	frame += " ack=";
	frame += std::to_string(alert_center.acknowledged);
	frame += " cleared=";
	frame += std::to_string(alert_center.cleared);
	frame += " critical=";
	frame += std::to_string(alert_center.critical);
	frame += " warning=";
	frame += std::to_string(alert_center.warning);
	frame += " attention=";
	frame += std::to_string(alert_center.attention);
	frame += " suppressed=";
	frame += std::to_string(alert_center.suppressed);
	frame += " flapping=";
	frame += std::to_string(alert_center.flapping);
	if (alert_center.storm_limited) frame += " storm-limited";
	frame += '\n';
	if (alert_center.items.empty()) {
		frame += "   No visible operator alerts\n";
	} else {
		for (const auto& item : alert_center.items) {
			frame += "   Alert ";
			frame += operator_alert_center_item_text(item);
			frame += '\n';
		}
	}
	if (alert_center.suppressed > 0) {
		frame += "   ";
		frame += std::to_string(alert_center.suppressed);
		frame += " low-severity alert(s) suppressed by attention policy\n";
	}

	frame += operator_alert_escalation_timeline_text(snapshot, 6);

	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	const auto action_state = control_plane.value("operator_action", nlohmann::json::object());
	const auto action = json_text(action_state, "action");
	if (!action.empty()) {
		const auto classification = json_text(action_state, "classification");
		const auto flow = begin_operator_action_flow(action, classification);
		frame += "Action ";
		frame += operator_action_flow_text(flow);
		frame += '\n';
	}

	frame += operator_workflows_text(control_plane);

	const auto audit_queue = derive_operator_audit_queue_view(snapshot, 4, 5);
	const auto reconciled_navigation = reconcile_operator_navigation(navigation, audit_queue);
	frame += "EVIDENCE ";
	frame += audit_queue.evidence_status;
	if (audit_queue.evidence_state == OperatorEvidenceState::Stale) {
		frame += " - operator approvals are fail-closed";
	} else if (audit_queue.evidence_state == OperatorEvidenceState::Missing) {
		frame += " - operator evidence unavailable";
	}
	frame += '\n';

	frame += "NAVIGATION ";
	frame += operator_navigation_text(reconciled_navigation);
	frame += '\n';

	frame += "APPROVAL QUEUE ";
	frame += std::to_string(audit_queue.approval_total);
	frame += '\n';
	if (audit_queue.approvals.empty()) {
		frame += "   No approval evidence available\n";
	} else {
		for (std::size_t index = 0; index < audit_queue.approvals.size(); ++index) {
			frame += reconciled_navigation.focus == OperatorFocusRegion::ApprovalQueue && reconciled_navigation.approval_index == index
				? " > Approval "
				: "   Approval ";
			frame += operator_approval_item_text(audit_queue.approvals[index]);
			frame += '\n';
		}
	}

	frame += "AUDIT TIMELINE ";
	frame += std::to_string(audit_queue.audit_total);
	frame += '\n';
	if (audit_queue.audit.empty()) {
		frame += "   No audit evidence available\n";
	} else {
		for (std::size_t index = 0; index < audit_queue.audit.size(); ++index) {
			frame += reconciled_navigation.focus == OperatorFocusRegion::AuditTimeline && reconciled_navigation.audit_index == index
				? " > Audit "
				: "   Audit ";
			frame += operator_audit_item_text(audit_queue.audit[index]);
			frame += '\n';
		}
	}

	if (reconciled_navigation.confirmation_open) {
		frame += "CONFIRMATION OPEN | request ";
		frame += reconciled_navigation.selected_request_id;
		frame += " | action ";
		frame += reconciled_navigation.selected_action;
		frame += " | Enter cannot execute; Esc cancels\n";
	}
	if (reconciled_navigation.status == "BLOCKED" ||
		reconciled_navigation.status.find("BLOCKED") != std::string::npos ||
		reconciled_navigation.status.find("MISSING") != std::string::npos ||
		reconciled_navigation.status.find("CANCELLED") != std::string::npos) {
		frame += "OPERATOR NOTICE ";
		frame += reconciled_navigation.status;
		frame += '\n';
	}

	if (audit_queue.truncated) frame += "  Additional operator evidence omitted from terminal view\n";
	return frame;
}

} // namespace sentum::ui
