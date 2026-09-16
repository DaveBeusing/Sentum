#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include <sentum/ui/OperatorAuditQueueView.hpp>
#include <sentum/ui/OperatorWorkflowView.hpp>
#include <sentum/ui/TerminalWorkspacePolicy.hpp>

namespace sentum::ui {

inline const char* operator_severity_text(OperatorSeverity severity) noexcept {
	switch (severity) {
		case OperatorSeverity::Normal: return "NORMAL";
		case OperatorSeverity::Attention: return "ATTENTION";
		case OperatorSeverity::Warning: return "WARNING";
		case OperatorSeverity::Critical: return "CRITICAL";
	}
	return "CRITICAL";
}

inline nlohmann::json operator_status_json(const OperatorStatus& status) {
	return {
		{"label", status.label},
		{"message", status.message},
		{"recommended_workspace", status.recommended_workspace},
		{"severity", operator_severity_text(status.severity)}
	};
}

inline nlohmann::json operator_workflow_json(const OperatorWorkflowView& workflow) {
	return {
		{"title", workflow.title},
		{"state", workflow.state},
		{"action", workflow.action},
		{"classification", workflow.classification},
		{"request_id", workflow.request_id},
		{"reason", workflow.reason},
		{"actor", workflow.actor},
		{"summary", workflow.summary},
		{"approval_required", workflow.approval_required},
		{"blocked", workflow.blocked},
		{"execution_authorized", false}
	};
}

inline nlohmann::json derive_cross_surface_operations_view(
	const nlohmann::json& snapshot,
	std::size_t approval_limit = 8,
	std::size_t audit_limit = 12) {
	const auto surface = derive_operator_control_surface(snapshot, "SYSTEM");
	const auto evidence = derive_operator_audit_queue_view(snapshot, approval_limit, audit_limit);
	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	const auto maintenance = maintenance_operator_workflow(control_plane);
	const auto incident = incident_operator_workflow(control_plane);
	const auto recovery = recovery_operator_workflow(control_plane);

	nlohmann::json approvals = nlohmann::json::array();
	for (const auto& item : evidence.approvals) {
		approvals.push_back({
			{"request_id", item.request_id},
			{"action", item.action},
			{"classification", item.classification},
			{"actor", item.actor},
			{"reason", item.reason},
			{"status", item.status},
			{"execution_authorized", false}
		});
	}

	nlohmann::json audit = nlohmann::json::array();
	for (const auto& item : evidence.audit) {
		audit.push_back({
			{"request_id", item.request_id},
			{"action", item.action},
			{"actor", item.actor},
			{"reason", item.reason},
			{"outcome", item.outcome},
			{"timestamp_utc", item.timestamp_utc}
		});
	}

	return {
		{"schema_version", 1},
		{"contract", "sentum.operations.v1"},
		{"authority", "READ_ONLY_PRESENTATION"},
		{"runtime", operator_status_json(surface.status)},
		{"governance", {
			{"state", surface.governance_state},
			{"evidence_state", evidence.evidence_status},
			{"maintenance_state", surface.maintenance_state},
			{"incident_state", surface.incident_state},
			{"recovery_state", surface.recovery_state},
			{"pending_approvals", surface.pending_approvals},
			{"approval_summary", surface.approval_summary},
			{"audit_summary", surface.audit_summary}
		}},
		{"workflows", {
			{"maintenance", operator_workflow_json(maintenance)},
			{"incident", operator_workflow_json(incident)},
			{"recovery", operator_workflow_json(recovery)}
		}},
		{"approval_queue", {
			{"total", evidence.approval_total},
			{"truncated", evidence.approval_total > evidence.approvals.size()},
			{"items", std::move(approvals)}
		}},
		{"audit_timeline", {
			{"total", evidence.audit_total},
			{"truncated", evidence.audit_total > evidence.audit.size()},
			{"items", std::move(audit)}
		}}
	};
}

} // namespace sentum::ui
