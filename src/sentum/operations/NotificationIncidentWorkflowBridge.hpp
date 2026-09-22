#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>
#include <sentum/operations/NotificationOperationsObservability.hpp>

namespace sentum::operations {

enum class NotificationIncidentCandidateState {
	None,
	Attention,
	ProposalReady,
	Blocked
};

struct NotificationIncidentCandidate {
	NotificationIncidentCandidateState state = NotificationIncidentCandidateState::None;
	std::string status = "NONE";
	std::string action;
	std::string classification = "FORBIDDEN";
	std::string reason;
	std::string source = "NOTIFICATION_OPERATIONS";
	std::string source_correlation_id;
	std::string source_evidence_id;
	std::size_t terminal_failures = 0;
	std::size_t backlog = 0;
	bool approval_required = false;
	bool incident_authorized = false;
	bool execution_authorized = false;
};

inline const char* notification_incident_candidate_state_name(NotificationIncidentCandidateState state) noexcept {
	switch (state) {
		case NotificationIncidentCandidateState::None: return "NONE";
		case NotificationIncidentCandidateState::Attention: return "ATTENTION";
		case NotificationIncidentCandidateState::ProposalReady: return "PROPOSAL_READY";
		case NotificationIncidentCandidateState::Blocked: return "BLOCKED";
	}
	return "BLOCKED";
}

inline NotificationIncidentCandidate derive_notification_incident_candidate(
	const NotificationOperationsView& operations) {
	NotificationIncidentCandidate candidate;
	candidate.terminal_failures = operations.terminal_failed;
	candidate.backlog = operations.backlog;
	candidate.incident_authorized = false;
	candidate.execution_authorized = false;

	if (!operations.evidence_available || operations.health == NotificationOperationsHealth::Unavailable) {
		candidate.state = NotificationIncidentCandidateState::Blocked;
		candidate.status = notification_incident_candidate_state_name(candidate.state);
		candidate.reason = "notification delivery evidence unavailable; incident proposal blocked fail closed";
		return candidate;
	}

	if (operations.health == NotificationOperationsHealth::IncidentCandidate && operations.terminal_failed > 0) {
		candidate.state = NotificationIncidentCandidateState::ProposalReady;
		candidate.status = notification_incident_candidate_state_name(candidate.state);
		candidate.action = "OPEN_INCIDENT";
		candidate.classification = "APPROVAL_REQUIRED";
		candidate.reason = "terminal notification delivery failure requires governed incident review";
		candidate.approval_required = true;
		return candidate;
	}

	if (operations.health == NotificationOperationsHealth::Attention) {
		candidate.state = NotificationIncidentCandidateState::Attention;
		candidate.status = notification_incident_candidate_state_name(candidate.state);
		candidate.reason = "notification delivery requires operator attention but does not meet incident proposal threshold";
		return candidate;
	}

	candidate.state = NotificationIncidentCandidateState::None;
	candidate.status = notification_incident_candidate_state_name(candidate.state);
	candidate.reason = "notification delivery does not require incident workflow escalation";
	return candidate;
}

inline nlohmann::json notification_incident_candidate_json(const NotificationIncidentCandidate& candidate) {
	return {
		{"status", candidate.status},
		{"action", candidate.action},
		{"classification", candidate.classification},
		{"reason", candidate.reason},
		{"source", candidate.source},
		{"source_correlation_id", candidate.source_correlation_id},
		{"source_evidence_id", candidate.source_evidence_id},
		{"terminal_failures", candidate.terminal_failures},
		{"backlog", candidate.backlog},
		{"approval_required", candidate.approval_required},
		{"incident_authorized", false},
		{"execution_authorized", false}
	};
}

inline void attach_notification_incident_source_identity(
	NotificationIncidentCandidate& candidate,
	const NotificationDeliveryEvidenceRecord& record) {
	if (candidate.state != NotificationIncidentCandidateState::ProposalReady ||
		record.state != "FAILED" || !record.terminal) {
		return;
	}
	const auto identity = record.alert_id.empty() ? record.dedup_key : record.alert_id;
	if (identity.empty()) return;
	candidate.source_correlation_id =
		"NOTIFICATION_OPERATIONS:" + identity + ":" + std::to_string(record.generation);
	candidate.source_evidence_id =
		record.dedup_key + ":" + std::to_string(record.attempt) + ":" + record.observed_at_utc;
}

inline NotificationIncidentCandidate derive_notification_incident_candidate_from_snapshot(
	const nlohmann::json& snapshot,
	NotificationOperationsThresholds thresholds = {}) {
	auto candidate = derive_notification_incident_candidate(
		derive_notification_operations_view_from_snapshot(snapshot, thresholds));
	if (candidate.state != NotificationIncidentCandidateState::ProposalReady) return candidate;

	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	const auto evidence = control_plane.value("notification_delivery_evidence", nlohmann::json::array());
	if (!evidence.is_array()) return candidate;
	for (auto it = evidence.rbegin(); it != evidence.rend(); ++it) {
		if (!it->is_object() || it->value("state", std::string{}) != "FAILED" ||
			!it->value("terminal", false)) {
			continue;
		}
		NotificationDeliveryEvidenceRecord record;
		record.dedup_key = it->value("dedup_key", std::string{});
		record.alert_id = it->value("alert_id", std::string{});
		record.generation = it->value("generation", std::size_t{0});
		record.state = "FAILED";
		record.attempt = it->value("attempt", std::size_t{0});
		record.terminal = true;
		record.observed_at_utc = it->value("observed_at_utc", std::string{});
		attach_notification_incident_source_identity(candidate, record);
		break;
	}
	return candidate;
}

inline NotificationIncidentCandidate derive_notification_incident_candidate(
	const NotificationDeliveryEvidenceRepository& repository,
	NotificationOperationsThresholds thresholds = {},
	std::size_t evidence_limit = 1024) {
	auto candidate = derive_notification_incident_candidate(
		derive_notification_operations_view(repository, thresholds, evidence_limit));
	if (candidate.state != NotificationIncidentCandidateState::ProposalReady) return candidate;

	const auto evidence = repository.load_latest_per_dedup_key(evidence_limit);
	if (evidence.truncated) {
		candidate.state = NotificationIncidentCandidateState::Blocked;
		candidate.status = notification_incident_candidate_state_name(candidate.state);
		candidate.action.clear();
		candidate.classification = "FORBIDDEN";
		candidate.approval_required = false;
		candidate.reason = "notification delivery evidence exceeds bounded incident correlation query";
		return candidate;
	}
	for (auto it = evidence.records.rbegin(); it != evidence.records.rend(); ++it) {
		if (it->record.state != "FAILED" || !it->record.terminal) continue;
		attach_notification_incident_source_identity(candidate, it->record);
		break;
	}
	if (candidate.source_correlation_id.empty()) {
		candidate.state = NotificationIncidentCandidateState::Blocked;
		candidate.status = notification_incident_candidate_state_name(candidate.state);
		candidate.action.clear();
		candidate.classification = "FORBIDDEN";
		candidate.approval_required = false;
		candidate.reason = "terminal notification failure lacks stable incident correlation evidence";
	}
	return candidate;
}

} // namespace sentum::operations
