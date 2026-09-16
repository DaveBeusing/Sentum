#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/ui/CrossSurfaceOperationsView.hpp>

namespace sentum::ui {

inline constexpr int kCrossSurfaceOperationsSchemaVersion = 1;
inline constexpr const char* kCrossSurfaceOperationsContract = "sentum.operations.v1";

struct CrossSurfaceSemanticState {
	std::string runtime_label;
	std::string runtime_message;
	std::string recommended_workspace;
	std::string governance_state;
	std::string evidence_state;
	std::string maintenance_state;
	std::string incident_state;
	std::string recovery_state;
	std::size_t pending_approvals = 0;
	std::vector<std::string> approval_semantics;
	std::vector<std::string> workflow_semantics;
	bool schema_valid = true;
};

inline CrossSurfaceSemanticState derive_terminal_semantic_state(
	const nlohmann::json& snapshot,
	std::size_t approval_limit = 8) {
	CrossSurfaceSemanticState state;
	const auto surface = derive_operator_control_surface(snapshot, "SYSTEM");
	const auto evidence = derive_operator_audit_queue_view(snapshot, approval_limit, 0);
	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());

	state.runtime_label = surface.status.label;
	state.runtime_message = surface.status.message;
	state.recommended_workspace = surface.status.recommended_workspace;
	state.governance_state = surface.governance_state;
	state.evidence_state = evidence.evidence_status;
	state.maintenance_state = surface.maintenance_state;
	state.incident_state = surface.incident_state;
	state.recovery_state = surface.recovery_state;
	state.pending_approvals = surface.pending_approvals;

	for (const auto& item : evidence.approvals) {
		state.approval_semantics.push_back(item.request_id + "|" + item.classification + "|" + item.status);
	}
	for (const auto& workflow : {
		maintenance_operator_workflow(control_plane),
		incident_operator_workflow(control_plane),
		recovery_operator_workflow(control_plane)}) {
		state.workflow_semantics.push_back(
			workflow.title + "|" + workflow.state + "|" + workflow.classification + "|" +
			(workflow.blocked ? "BLOCKED" : workflow.approval_required ? "APPROVAL_REQUIRED" : "READ_ONLY"));
	}
	return state;
}

inline CrossSurfaceSemanticState derive_web_semantic_state(const nlohmann::json& view) {
	CrossSurfaceSemanticState state;
	state.schema_valid = view.is_object() &&
		view.value("schema_version", -1) == kCrossSurfaceOperationsSchemaVersion &&
		view.value("contract", std::string{}) == kCrossSurfaceOperationsContract &&
		view.value("authority", std::string{}) == "READ_ONLY_PRESENTATION";
	if (!state.schema_valid) {
		state.runtime_label = "CRITICAL";
		state.governance_state = "UNAVAILABLE";
		state.evidence_state = "MISSING";
		return state;
	}

	const auto runtime = view.value("runtime", nlohmann::json::object());
	const auto governance = view.value("governance", nlohmann::json::object());
	state.runtime_label = json_text(runtime, "label", "CRITICAL");
	state.runtime_message = json_text(runtime, "message");
	state.recommended_workspace = json_text(runtime, "recommended_workspace", "SYSTEM");
	state.governance_state = json_text(governance, "state", "UNAVAILABLE");
	state.evidence_state = json_text(governance, "evidence_state", "MISSING");
	state.maintenance_state = json_text(governance, "maintenance_state", "UNAVAILABLE");
	state.incident_state = json_text(governance, "incident_state", "UNAVAILABLE");
	state.recovery_state = json_text(governance, "recovery_state", "UNAVAILABLE");
	state.pending_approvals = json_size(governance, "pending_approvals");

	const auto approvals = view.value("approval_queue", nlohmann::json::object()).value("items", nlohmann::json::array());
	if (approvals.is_array()) {
		for (const auto& item : approvals) {
			state.approval_semantics.push_back(
				json_text(item, "request_id", "-") + "|" + json_text(item, "classification", "FORBIDDEN") + "|" +
				json_text(item, "status", "BLOCKED"));
		}
	}

	const auto workflows = view.value("workflows", nlohmann::json::object());
	for (const auto* name : {"maintenance", "incident", "recovery"}) {
		const auto workflow = workflows.value(name, nlohmann::json::object());
		const bool blocked = workflow.value("blocked", true);
		const bool approval_required = workflow.value("approval_required", false);
		state.workflow_semantics.push_back(
			json_text(workflow, "title", "WORKFLOW") + "|" + json_text(workflow, "state", "UNAVAILABLE") + "|" +
			json_text(workflow, "classification", "FORBIDDEN") + "|" +
			(blocked ? "BLOCKED" : approval_required ? "APPROVAL_REQUIRED" : "READ_ONLY"));
	}
	return state;
}

inline std::vector<std::string> cross_surface_semantic_drift(
	const CrossSurfaceSemanticState& terminal,
	const CrossSurfaceSemanticState& web) {
	std::vector<std::string> drift;
	if (!web.schema_valid) drift.push_back("schema/authority contract invalid");
	if (terminal.runtime_label != web.runtime_label) drift.push_back("runtime.label");
	if (terminal.runtime_message != web.runtime_message) drift.push_back("runtime.message");
	if (terminal.recommended_workspace != web.recommended_workspace) drift.push_back("runtime.recommended_workspace");
	if (terminal.governance_state != web.governance_state) drift.push_back("governance.state");
	if (terminal.evidence_state != web.evidence_state) drift.push_back("governance.evidence_state");
	if (terminal.maintenance_state != web.maintenance_state) drift.push_back("governance.maintenance_state");
	if (terminal.incident_state != web.incident_state) drift.push_back("governance.incident_state");
	if (terminal.recovery_state != web.recovery_state) drift.push_back("governance.recovery_state");
	if (terminal.pending_approvals != web.pending_approvals) drift.push_back("governance.pending_approvals");
	if (terminal.approval_semantics != web.approval_semantics) drift.push_back("approval_queue.semantic_projection");
	if (terminal.workflow_semantics != web.workflow_semantics) drift.push_back("workflows.semantic_projection");
	return drift;
}

inline std::vector<std::string> detect_cross_surface_semantic_drift(
	const nlohmann::json& snapshot,
	const nlohmann::json& web_view,
	std::size_t approval_limit = 8) {
	return cross_surface_semantic_drift(
		derive_terminal_semantic_state(snapshot, approval_limit),
		derive_web_semantic_state(web_view));
}

} // namespace sentum::ui
