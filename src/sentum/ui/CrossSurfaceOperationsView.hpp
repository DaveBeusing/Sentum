#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include <sentum/operations/NotificationIncidentWorkflowIntegration.hpp>
#include <sentum/operations/NotificationOperationsObservability.hpp>
#include <sentum/ui/OperatorAlertCenterView.hpp>
#include <sentum/ui/OperatorAlertEscalationTimeline.hpp>
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
	std::size_t audit_limit = 12,
	std::size_t alert_limit = 12) {
	const auto surface = derive_operator_control_surface(snapshot, "SYSTEM");
	const auto evidence = derive_operator_audit_queue_view(snapshot, approval_limit, audit_limit);
	const auto alerts = derive_operator_alert_center_view(snapshot, alert_limit);
	const auto escalation = derive_operator_alert_escalation_timeline(snapshot, alert_limit);
	const auto notification_operations = sentum::operations::derive_notification_operations_view_from_snapshot(snapshot);
	const auto notification_incident = sentum::operations::derive_notification_incident_workflow_integration(snapshot);
	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	const auto maintenance = maintenance_operator_workflow(control_plane);
	const auto incident = incident_operator_workflow(control_plane);
	const auto recovery = recovery_operator_workflow(control_plane);

	nlohmann::json approvals = nlohmann::json::array();
	for (const auto& item : evidence.approvals) {
		approvals.push_back({
			{"request_id", item.request_id}, {"action", item.action}, {"classification", item.classification},
			{"actor", item.actor}, {"reason", item.reason}, {"status", item.status}, {"execution_authorized", false}
		});
	}

	nlohmann::json audit = nlohmann::json::array();
	for (const auto& item : evidence.audit) {
		audit.push_back({
			{"request_id", item.request_id}, {"action", item.action}, {"actor", item.actor},
			{"reason", item.reason}, {"outcome", item.outcome}, {"timestamp_utc", item.timestamp_utc}
		});
	}

	nlohmann::json alert_items = nlohmann::json::array();
	for (const auto& item : alerts.items) {
		alert_items.push_back({
			{"id", item.id}, {"source", item.source}, {"severity", item.severity}, {"state", item.state},
			{"generation", item.generation}, {"title", item.title}, {"message", item.message},
			{"guidance", item.guidance}, {"recommended_workspace", item.recommended_workspace},
			{"acknowledged_by", item.acknowledged_by}, {"acknowledgement_reason", item.acknowledgement_reason},
			{"escalation_level", item.escalation_level}, {"acknowledgement_required", item.acknowledgement_required},
			{"notification_candidate", item.notification_candidate}, {"active", item.active},
			{"attention", item.attention}, {"suppressed", item.suppressed}, {"flapping", item.flapping},
			{"attention_reason", item.attention_reason}, {"execution_authorized", false}
		});
	}

	nlohmann::json escalation_items = nlohmann::json::array();
	for (const auto& item : escalation.items) {
		escalation_items.push_back({
			{"alert_id", item.alert_id}, {"severity", item.severity}, {"lifecycle_state", item.lifecycle_state},
			{"aging_state", item.aging_state}, {"first_seen_utc", item.first_seen_utc}, {"last_seen_utc", item.last_seen_utc},
			{"next_escalation_at_utc", item.next_escalation_at_utc}, {"observed_at_utc", item.observed_at_utc},
			{"latest_escalation", item.latest_escalation}, {"latest_actor", item.latest_actor},
			{"latest_reason", item.latest_reason}, {"overdue", item.overdue},
			{"acknowledgement_required", item.acknowledgement_required}, {"execution_authorized", false}
		});
	}

	return {
		{"schema_version", 1}, {"contract", "sentum.operations.v1"}, {"authority", "READ_ONLY_PRESENTATION"},
		{"runtime", operator_status_json(surface.status)},
		{"governance", {
			{"state", surface.governance_state}, {"evidence_state", evidence.evidence_status},
			{"maintenance_state", surface.maintenance_state}, {"incident_state", surface.incident_state},
			{"recovery_state", surface.recovery_state}, {"pending_approvals", surface.pending_approvals},
			{"approval_summary", surface.approval_summary}, {"audit_summary", surface.audit_summary}
		}},
		{"notification_operations", sentum::operations::notification_operations_json(notification_operations)},
		{"notification_incident_workflow", sentum::operations::notification_incident_workflow_integration_json(notification_incident)},
		{"alerts", {
			{"total", alerts.total}, {"active", alerts.active}, {"acknowledged", alerts.acknowledged},
			{"cleared", alerts.cleared}, {"critical", alerts.critical}, {"warning", alerts.warning},
			{"attention", alerts.attention}, {"suppressed", alerts.suppressed}, {"flapping", alerts.flapping},
			{"storm_limited", alerts.storm_limited}, {"truncated", alerts.truncated},
			{"execution_authorized", false}, {"items", std::move(alert_items)}
		}},
		{"alert_escalation", {
			{"total", escalation.total}, {"due", escalation.due}, {"overdue", escalation.overdue},
			{"unavailable", escalation.unavailable}, {"truncated", escalation.truncated},
			{"execution_authorized", false}, {"items", std::move(escalation_items)}
		}},
		{"workflows", {
			{"maintenance", operator_workflow_json(maintenance)}, {"incident", operator_workflow_json(incident)},
			{"recovery", operator_workflow_json(recovery)}
		}},
		{"approval_queue", {
			{"total", evidence.approval_total}, {"truncated", evidence.approval_total > evidence.approvals.size()},
			{"items", std::move(approvals)}
		}},
		{"audit_timeline", {
			{"total", evidence.audit_total}, {"truncated", evidence.audit_total > evidence.audit.size()},
			{"items", std::move(audit)}
		}}
	};
}


inline nlohmann::json derive_cross_surface_operations_view(
	const nlohmann::json& snapshot,
	const sentum::operations::NotificationDeliveryEvidenceRepository& notification_evidence,
	std::size_t approval_limit = 8,
	std::size_t audit_limit = 12,
	std::size_t alert_limit = 12,
	std::size_t notification_evidence_limit = 1024) {
	const auto durable_snapshot = sentum::operations::notification_delivery_snapshot_from_repository(
		snapshot, notification_evidence, notification_evidence_limit);
	return derive_cross_surface_operations_view(
		durable_snapshot, approval_limit, audit_limit, alert_limit);
}

} // namespace sentum::ui
