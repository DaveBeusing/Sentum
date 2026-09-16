#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <sentum/ui/OperatorActionFlow.hpp>
#include <sentum/ui/OperatorAuditQueueView.hpp>
#include <sentum/ui/OperatorNavigationPolicy.hpp>
#include <sentum/ui/OperatorWorkflowView.hpp>
#include <sentum/ui/TerminalWorkspacePolicy.hpp>

namespace sentum::ui {

inline std::string operator_surface_frame_lines(
	const nlohmann::json& snapshot,
	std::string_view active_workspace) {
	const auto surface = derive_operator_control_surface(snapshot, active_workspace);
	std::string frame;
	frame.reserve(
		surface.banner.size() + surface.navigation.size() + surface.workspace_help.size() +
		surface.governance_state.size() + surface.maintenance_state.size() +
		surface.incident_state.size() + surface.recovery_state.size() +
		surface.approval_summary.size() + surface.audit_summary.size() + 1280);

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
	frame += "NAVIGATION ";
	frame += operator_navigation_text(OperatorNavigationState{});
	frame += '\n';
	if (!audit_queue.approvals.empty()) {
		frame += "APPROVAL QUEUE ";
		frame += std::to_string(audit_queue.approval_total);
		frame += '\n';
		for (const auto& item : audit_queue.approvals) {
			frame += "  Approval ";
			frame += operator_approval_item_text(item);
			frame += '\n';
		}
	}
	if (!audit_queue.audit.empty()) {
		frame += "AUDIT TIMELINE ";
		frame += std::to_string(audit_queue.audit_total);
		frame += '\n';
		for (const auto& item : audit_queue.audit) {
			frame += "  Audit ";
			frame += operator_audit_item_text(item);
			frame += '\n';
		}
	}
	if (audit_queue.truncated) frame += "  Additional operator evidence omitted from terminal view\n";
	return frame;
}

} // namespace sentum::ui
