#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <sentum/ui/OperatorActionFlow.hpp>

namespace sentum::ui {

enum class OperatorWorkflowKind {
	Maintenance,
	Incident,
	Recovery
};

struct OperatorWorkflowView {
	OperatorWorkflowKind kind = OperatorWorkflowKind::Maintenance;
	std::string title;
	std::string state = "IDLE";
	std::string action;
	std::string classification = "FORBIDDEN";
	std::string request_id;
	std::string reason;
	std::string actor;
	std::string summary;
	bool approval_required = false;
	bool blocked = false;
	bool execution_authorized = false;
};

inline std::string workflow_title(OperatorWorkflowKind kind) {
	switch (kind) {
		case OperatorWorkflowKind::Maintenance: return "MAINTENANCE";
		case OperatorWorkflowKind::Incident: return "INCIDENT";
		case OperatorWorkflowKind::Recovery: return "RECOVERY";
	}
	return "WORKFLOW";
}

inline OperatorWorkflowView derive_operator_workflow_view(
	OperatorWorkflowKind kind,
	const nlohmann::json& workflow) {
	OperatorWorkflowView view;
	view.kind = kind;
	view.title = workflow_title(kind);
	if (!workflow.is_object() || workflow.empty()) {
		view.summary = view.title + " | IDLE";
		return view;
	}

	view.state = json_text(workflow, "state", "UNAVAILABLE");
	view.action = json_text(workflow, "action");
	view.classification = json_text(workflow, "classification");
	view.request_id = json_text(workflow, "request_id");
	view.reason = json_text(workflow, "reason");
	view.actor = json_text(workflow, "actor");

	if (!view.action.empty()) {
		const auto flow = begin_operator_action_flow(view.action, view.classification);
		view.classification = flow.presentation.classification;
		view.approval_required = flow.state == OperatorActionFlowState::ConfirmationRequired ||
			flow.state == OperatorActionFlowState::ApprovalRequested;
		view.blocked = flow.state == OperatorActionFlowState::Blocked;
	}

	if (!view.action.empty() && json_text(workflow, "classification").empty()) {
		view.classification = "FORBIDDEN";
		view.blocked = true;
		view.approval_required = false;
	}

	view.execution_authorized = false;
	view.summary = view.title + " | " + view.state;
	if (!view.action.empty()) view.summary += " | " + view.action;
	if (!view.classification.empty()) view.summary += " | " + view.classification;
	if (!view.request_id.empty()) view.summary += " | request " + view.request_id;
	if (!view.reason.empty()) view.summary += " | " + view.reason;
	if (!view.actor.empty()) view.summary += " | actor " + view.actor;
	if (view.blocked) view.summary += " | BLOCKED";
	else if (view.approval_required) view.summary += " | APPROVAL REQUIRED";
	return view;
}

inline OperatorWorkflowView maintenance_operator_workflow(const nlohmann::json& control_plane) {
	return derive_operator_workflow_view(
		OperatorWorkflowKind::Maintenance,
		control_plane.value("maintenance_workflow", nlohmann::json::object()));
}

inline OperatorWorkflowView incident_operator_workflow(const nlohmann::json& control_plane) {
	return derive_operator_workflow_view(
		OperatorWorkflowKind::Incident,
		control_plane.value("incident_workflow", nlohmann::json::object()));
}

inline OperatorWorkflowView recovery_operator_workflow(const nlohmann::json& control_plane) {
	return derive_operator_workflow_view(
		OperatorWorkflowKind::Recovery,
		control_plane.value("recovery_workflow", nlohmann::json::object()));
}

inline std::string operator_workflows_text(const nlohmann::json& control_plane) {
	const auto maintenance = maintenance_operator_workflow(control_plane);
	const auto incident = incident_operator_workflow(control_plane);
	const auto recovery = recovery_operator_workflow(control_plane);
	return maintenance.summary + '\n' + incident.summary + '\n' + recovery.summary + '\n';
}

} // namespace sentum::ui
