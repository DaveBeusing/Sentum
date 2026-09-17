#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include <sentum/operations/NotificationIncidentWorkflowBridge.hpp>

namespace sentum::operations {

struct NotificationIncidentWorkflowIntegration {
	NotificationIncidentCandidate candidate;
	std::string request_id;
	std::string approval_status = "NOT_REQUESTED";
	std::string audit_outcome;
	std::string incident_state = "IDLE";
	std::string recovery_state = "IDLE";
	bool approval_evidence_available = false;
	bool audit_evidence_available = false;
	bool recovery_evidence_available = false;
	bool incident_authorized = false;
	bool execution_authorized = false;
};

inline NotificationIncidentWorkflowIntegration derive_notification_incident_workflow_integration(
	const nlohmann::json& snapshot,
	NotificationOperationsThresholds thresholds = {}) {
	NotificationIncidentWorkflowIntegration view;
	view.candidate = derive_notification_incident_candidate_from_snapshot(snapshot, thresholds);
	view.incident_authorized = false;
	view.execution_authorized = false;

	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	if (!control_plane.is_object()) return view;

	const auto incident = control_plane.value("incident_workflow", nlohmann::json::object());
	if (incident.is_object()) {
		view.incident_state = incident.value("state", std::string("IDLE"));
		if (incident.value("action", std::string{}) == "OPEN_INCIDENT") {
			view.request_id = incident.value("request_id", std::string{});
		}
	}

	const auto recovery = control_plane.value("recovery_workflow", nlohmann::json::object());
	if (recovery.is_object() && !recovery.empty()) {
		view.recovery_state = recovery.value("state", std::string("UNAVAILABLE"));
		view.recovery_evidence_available = true;
	}

	const auto approvals = control_plane.value("approval_queue", nlohmann::json::array());
	if (approvals.is_array()) {
		for (const auto& item : approvals) {
			if (!item.is_object() || item.value("action", std::string{}) != "OPEN_INCIDENT") continue;
			if (!view.request_id.empty() && item.value("request_id", std::string{}) != view.request_id) continue;
			view.request_id = item.value("request_id", view.request_id);
			view.approval_status = item.value("status", std::string("PENDING"));
			view.approval_evidence_available = true;
			break;
		}
	}

	const auto audit = control_plane.value("audit_timeline", nlohmann::json::array());
	if (audit.is_array()) {
		for (const auto& item : audit) {
			if (!item.is_object() || item.value("action", std::string{}) != "OPEN_INCIDENT") continue;
			if (!view.request_id.empty() && item.value("request_id", std::string{}) != view.request_id) continue;
			view.request_id = item.value("request_id", view.request_id);
			view.audit_outcome = item.value("outcome", std::string{});
			view.audit_evidence_available = true;
			break;
		}
	}

	return view;
}

inline nlohmann::json notification_incident_workflow_integration_json(
	const NotificationIncidentWorkflowIntegration& view) {
	return {
		{"candidate", notification_incident_candidate_json(view.candidate)},
		{"request_id", view.request_id},
		{"approval_status", view.approval_status},
		{"audit_outcome", view.audit_outcome},
		{"incident_state", view.incident_state},
		{"recovery_state", view.recovery_state},
		{"approval_evidence_available", view.approval_evidence_available},
		{"audit_evidence_available", view.audit_evidence_available},
		{"recovery_evidence_available", view.recovery_evidence_available},
		{"incident_authorized", false},
		{"execution_authorized", false}
	};
}

} // namespace sentum::operations
