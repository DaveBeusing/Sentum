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
		{"terminal_failures", candidate.terminal_failures},
		{"backlog", candidate.backlog},
		{"approval_required", candidate.approval_required},
		{"incident_authorized", false},
		{"execution_authorized", false}
	};
}

inline NotificationIncidentCandidate derive_notification_incident_candidate_from_snapshot(
	const nlohmann::json& snapshot,
	NotificationOperationsThresholds thresholds = {}) {
	return derive_notification_incident_candidate(
		derive_notification_operations_view_from_snapshot(snapshot, thresholds));
}

} // namespace sentum::operations
