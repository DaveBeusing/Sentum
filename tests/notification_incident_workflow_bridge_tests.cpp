#include <sentum/operations/NotificationIncidentWorkflowIntegration.hpp>

#include <iostream>
#include <stdexcept>

namespace {

using sentum::operations::NotificationIncidentCandidateState;
using sentum::operations::NotificationOperationsHealth;
using sentum::operations::NotificationOperationsView;

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

NotificationOperationsView operations(NotificationOperationsHealth health) {
	NotificationOperationsView view;
	view.health = health;
	view.status = sentum::operations::notification_operations_health_name(health);
	view.evidence_available = health != NotificationOperationsHealth::Unavailable;
	view.incident_authorized = false;
	view.execution_authorized = false;
	return view;
}

nlohmann::json incident_candidate_snapshot() {
	return {{"operations_control_plane", {{"notification_delivery_evidence", nlohmann::json::array({
		{{"dedup_key", "incident-key"}, {"alert_id", "alert-1"}, {"generation", 1}, {"channel", "PAGER"},
		 {"audience", "OPERATIONS_ON_CALL"}, {"state", "FAILED"}, {"attempt", 3}, {"terminal", true},
		 {"delivery_authorized", true}}
	})}}}};
}

void test_healthy_delivery_has_no_incident_proposal() {
	const auto candidate = sentum::operations::derive_notification_incident_candidate(operations(NotificationOperationsHealth::Healthy));
	require(candidate.state == NotificationIncidentCandidateState::None, "healthy delivery created incident proposal");
	require(candidate.action.empty(), "healthy delivery exposed incident action");
	require(!candidate.approval_required, "healthy delivery requested approval");
	require(!candidate.incident_authorized && !candidate.execution_authorized, "healthy candidate gained authority");
}

void test_attention_remains_advisory() {
	auto view = operations(NotificationOperationsHealth::Attention);
	view.backlog = 9;
	const auto candidate = sentum::operations::derive_notification_incident_candidate(view);
	require(candidate.state == NotificationIncidentCandidateState::Attention, "attention state escalated incorrectly");
	require(candidate.action.empty(), "attention state exposed incident action");
	require(candidate.backlog == 9, "attention backlog evidence missing");
}

void test_terminal_failure_creates_approval_required_proposal_only() {
	auto view = operations(NotificationOperationsHealth::IncidentCandidate);
	view.terminal_failed = 2;
	view.backlog = 1;
	const auto candidate = sentum::operations::derive_notification_incident_candidate(view);
	require(candidate.state == NotificationIncidentCandidateState::ProposalReady, "terminal failure did not create proposal");
	require(candidate.action == "OPEN_INCIDENT", "proposal action mismatch");
	require(candidate.classification == "APPROVAL_REQUIRED", "incident proposal bypassed approval governance");
	require(candidate.approval_required, "incident proposal did not require approval");
	require(!candidate.incident_authorized && !candidate.execution_authorized, "proposal gained authority");
}

void test_incident_health_without_terminal_failure_does_not_open_proposal() {
	auto view = operations(NotificationOperationsHealth::IncidentCandidate);
	const auto candidate = sentum::operations::derive_notification_incident_candidate(view);
	require(candidate.state == NotificationIncidentCandidateState::None, "incident health without terminal evidence created proposal");
	require(candidate.action.empty(), "missing terminal evidence exposed incident action");
}

void test_unavailable_evidence_blocks_fail_closed() {
	const auto candidate = sentum::operations::derive_notification_incident_candidate(sentum::operations::unavailable_notification_operations_view());
	require(candidate.state == NotificationIncidentCandidateState::Blocked, "unavailable evidence did not block proposal");
	require(candidate.action.empty(), "blocked candidate exposed action");
	require(candidate.classification == "FORBIDDEN", "blocked candidate classification mismatch");
}

void test_candidate_does_not_invent_control_plane_evidence() {
	const auto view = sentum::operations::derive_notification_incident_workflow_integration(incident_candidate_snapshot());
	require(view.candidate.status == "PROPOSAL_READY", "incident proposal missing");
	require(view.request_id.empty(), "proposal invented request id");
	require(view.approval_status == "NOT_REQUESTED", "proposal invented approval state");
	require(!view.approval_evidence_available && !view.audit_evidence_available && !view.recovery_evidence_available,
		"proposal invented control-plane evidence");
}

void test_existing_control_plane_evidence_is_correlated() {
	auto snapshot = incident_candidate_snapshot();
	auto& cp = snapshot["operations_control_plane"];
	cp["incident_workflow"] = {
		{"state", "OPENING"}, {"action", "OPEN_INCIDENT"}, {"request_id", "req-42"},
		{"source_correlation_id", "NOTIFICATION_OPERATIONS:alert-1:1"},
		{"classification", "APPROVAL_REQUIRED"}
	};
	cp["approval_queue"] = nlohmann::json::array({{
		{"request_id", "req-42"}, {"source_correlation_id", "NOTIFICATION_OPERATIONS:alert-1:1"},
		{"action", "OPEN_INCIDENT"}, {"status", "PENDING"}, {"classification", "APPROVAL_REQUIRED"}
	}});
	cp["audit_timeline"] = nlohmann::json::array({{
		{"request_id", "req-42"}, {"source_correlation_id", "NOTIFICATION_OPERATIONS:alert-1:1"},
		{"action", "OPEN_INCIDENT"}, {"outcome", "REQUESTED"}
	}});
	cp["recovery_workflow"] = {
		{"state", "PENDING_RECONCILIATION"}, {"request_id", "req-42"},
		{"source_correlation_id", "NOTIFICATION_OPERATIONS:alert-1:1"}
	};
	const auto view = sentum::operations::derive_notification_incident_workflow_integration(snapshot);
	require(view.request_id == "req-42", "request correlation failed");
	require(view.approval_status == "PENDING", "approval evidence missing");
	require(view.audit_outcome == "REQUESTED", "audit evidence missing");
	require(view.incident_state == "OPENING", "incident state mismatch");
	require(view.recovery_state == "PENDING_RECONCILIATION", "recovery state mismatch");
	require(view.approval_evidence_available && view.audit_evidence_available && view.recovery_evidence_available,
		"control-plane evidence availability mismatch");
	require(!view.incident_authorized && !view.execution_authorized, "evidence projection gained authority");
}

void test_unrelated_evidence_is_not_attached() {
	auto snapshot = incident_candidate_snapshot();
	auto& cp = snapshot["operations_control_plane"];
	cp["approval_queue"] = nlohmann::json::array({{{"request_id", "other"}, {"action", "RESUME_ENTRIES"}, {"status", "PENDING"}}});
	cp["audit_timeline"] = nlohmann::json::array({{{"request_id", "other"}, {"action", "CLEAR_KILL_SWITCH"}, {"outcome", "APPROVED"}}});
	const auto view = sentum::operations::derive_notification_incident_workflow_integration(snapshot);
	require(!view.approval_evidence_available, "unrelated approval was attached");
	require(!view.audit_evidence_available, "unrelated audit was attached");
}


void test_mismatched_open_incident_evidence_fails_closed() {
	auto snapshot = incident_candidate_snapshot();
	auto& cp = snapshot["operations_control_plane"];
	cp["incident_workflow"] = {
		{"state", "OPEN"}, {"action", "OPEN_INCIDENT"}, {"request_id", "req-other"},
		{"source_correlation_id", "NOTIFICATION_OPERATIONS:other-alert:9"}
	};
	cp["approval_queue"] = nlohmann::json::array({{
		{"request_id", "req-other"}, {"source_correlation_id", "NOTIFICATION_OPERATIONS:other-alert:9"},
		{"action", "OPEN_INCIDENT"}, {"status", "PENDING"}
	}});
	cp["audit_timeline"] = nlohmann::json::array({{
		{"request_id", "req-other"}, {"source_correlation_id", "NOTIFICATION_OPERATIONS:other-alert:9"},
		{"action", "OPEN_INCIDENT"}, {"event_type", "APPROVAL_DECIDED"}, {"outcome", "APPROVED"}
	}});
	cp["recovery_workflow"] = {
		{"state", "IN_PROGRESS"}, {"request_id", "req-other"},
		{"source_correlation_id", "NOTIFICATION_OPERATIONS:other-alert:9"}
	};

	const auto view = sentum::operations::derive_notification_incident_workflow_integration(snapshot);
	require(view.request_id.empty(), "mismatched request was attached");
	require(view.incident_state == "IDLE", "mismatched incident state was attached");
	require(view.approval_status == "NOT_REQUESTED", "mismatched approval was attached");
	require(view.recovery_state == "IDLE", "mismatched recovery state was attached");
	require(!view.approval_evidence_available && !view.audit_evidence_available && !view.recovery_evidence_available,
		"mismatched governed evidence was treated as authoritative");
}

void test_json_contract_is_read_only() {
	const auto view = sentum::operations::derive_notification_incident_workflow_integration(incident_candidate_snapshot());
	const auto json = sentum::operations::notification_incident_workflow_integration_json(view);
	require(json.at("candidate").at("status") == "PROPOSAL_READY", "json status mismatch");
	require(json.at("candidate").at("classification") == "APPROVAL_REQUIRED", "json governance mismatch");
	require(json.at("incident_authorized") == false, "json granted incident authority");
	require(json.at("execution_authorized") == false, "json granted execution authority");
}

} // namespace

int main() {
	try {
		test_healthy_delivery_has_no_incident_proposal();
		test_attention_remains_advisory();
		test_terminal_failure_creates_approval_required_proposal_only();
		test_incident_health_without_terminal_failure_does_not_open_proposal();
		test_unavailable_evidence_blocks_fail_closed();
		test_candidate_does_not_invent_control_plane_evidence();
		test_existing_control_plane_evidence_is_correlated();
		test_unrelated_evidence_is_not_attached();
		test_mismatched_open_incident_evidence_fails_closed();
		test_json_contract_is_read_only();
		std::cout << "notification incident workflow bridge tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "notification incident workflow bridge test failure: " << error.what() << '\n';
		return 1;
	}
}
