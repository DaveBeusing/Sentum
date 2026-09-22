#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>
#include <sentum/operations/NotificationIncidentWorkflowBridge.hpp>

namespace sentum::operations {

struct NotificationIncidentWorkflowIntegration {
	NotificationIncidentCandidate candidate;
	std::string request_id;
	std::string source_correlation_id;
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
	view.source_correlation_id = view.candidate.source_correlation_id;

	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	if (!control_plane.is_object()) return view;

	const auto incident = control_plane.value("incident_workflow", nlohmann::json::object());
	if (incident.is_object()) {
		const auto incident_correlation = incident.value("source_correlation_id", std::string{});
		const bool correlation_matches = view.source_correlation_id.empty()
			? true
			: incident_correlation == view.source_correlation_id;
		if (correlation_matches) {
			view.incident_state = incident.value("state", std::string("IDLE"));
			view.request_id = incident.value("request_id", std::string{});
			if (view.source_correlation_id.empty()) view.source_correlation_id = incident_correlation;
		}
	}

	const auto recovery = control_plane.contains("incident_recovery_workflow")
		? control_plane.value("incident_recovery_workflow", nlohmann::json::object())
		: control_plane.value("recovery_workflow", nlohmann::json::object());
	if (recovery.is_object() && !recovery.empty()) {
		const auto recovery_request_id = recovery.value("request_id", std::string{});
		const auto recovery_correlation = recovery.value("source_correlation_id", std::string{});
		const bool request_matches = !view.request_id.empty() && recovery_request_id == view.request_id;
		const bool correlation_matches =
			view.request_id.empty() && !view.source_correlation_id.empty() &&
			recovery_correlation == view.source_correlation_id;
		if (request_matches || correlation_matches ||
			(view.request_id.empty() && view.source_correlation_id.empty())) {
			view.recovery_state = recovery.value("state", std::string("UNAVAILABLE"));
			view.recovery_evidence_available = true;
		}
	}

	const auto approvals = control_plane.value("approval_queue", nlohmann::json::array());
	if (approvals.is_array()) {
		for (const auto& item : approvals) {
			if (!item.is_object() || item.value("action", std::string{}) != "OPEN_INCIDENT") continue;
			const auto item_request_id = item.value("request_id", std::string{});
			const auto item_correlation = item.value("source_correlation_id", std::string{});
			if (!view.request_id.empty() && item_request_id != view.request_id) continue;
			if (view.request_id.empty() && !view.source_correlation_id.empty() &&
				item_correlation != view.source_correlation_id) {
				continue;
			}
			view.request_id = item_request_id;
			if (view.source_correlation_id.empty()) view.source_correlation_id = item_correlation;
			view.approval_status = item.value("status", std::string("PENDING"));
			view.approval_evidence_available = true;
			break;
		}
	}

	const auto audit = control_plane.value("audit_timeline", nlohmann::json::array());
	if (audit.is_array()) {
		for (const auto& item : audit) {
			if (!item.is_object() || item.value("action", std::string{}) != "OPEN_INCIDENT") continue;
			const auto item_request_id = item.value("request_id", std::string{});
			const auto item_correlation = item.value("source_correlation_id", std::string{});
			if (!view.request_id.empty() && item_request_id != view.request_id) continue;
			if (view.request_id.empty() && !view.source_correlation_id.empty() &&
				item_correlation != view.source_correlation_id) {
				continue;
			}
			view.request_id = item_request_id;
			if (view.source_correlation_id.empty()) view.source_correlation_id = item_correlation;
			if (!view.audit_evidence_available) {
				view.audit_outcome = item.value("outcome", std::string{});
				view.audit_evidence_available = true;
			}
			if (!view.approval_evidence_available &&
				item.value("event_type", std::string{}) == "APPROVAL_DECIDED") {
				const auto decision = item.value("outcome", std::string{});
				if (decision == "APPROVED" || decision == "DENIED") {
					view.approval_status = decision;
					view.approval_evidence_available = true;
				}
			}
			if (view.audit_evidence_available && view.approval_evidence_available) break;
		}
	}

	return view;
}

inline NotificationIncidentWorkflowIntegration derive_notification_incident_workflow_integration(
	const nlohmann::json& snapshot,
	const NotificationDeliveryEvidenceRepository& repository,
	NotificationOperationsThresholds thresholds = {},
	std::size_t evidence_limit = 1024) {
	const auto durable_snapshot = notification_delivery_snapshot_from_repository(
		snapshot, repository, evidence_limit);
	return derive_notification_incident_workflow_integration(durable_snapshot, thresholds);
}

inline nlohmann::json notification_incident_workflow_integration_json(
	const NotificationIncidentWorkflowIntegration& view) {
	return {
		{"candidate", notification_incident_candidate_json(view.candidate)},
		{"request_id", view.request_id},
		{"source_correlation_id", view.source_correlation_id},
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
