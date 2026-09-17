#include <sentum/operations/NotificationIncidentWorkflowBridge.hpp>

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

void test_healthy_delivery_has_no_incident_proposal() {
	const auto candidate = sentum::operations::derive_notification_incident_candidate(
		operations(NotificationOperationsHealth::Healthy));
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
	require(candidate.status == "ATTENTION", "attention status mismatch");
	require(candidate.action.empty(), "attention state exposed incident action");
	require(candidate.backlog == 9, "attention backlog evidence missing");
	require(!candidate.approval_required, "attention state requested approval");
}

void test_terminal_failure_creates_approval_required_proposal_only() {
	auto view = operations(NotificationOperationsHealth::IncidentCandidate);
	view.terminal_failed = 2;
	view.backlog = 1;
	const auto candidate = sentum::operations::derive_notification_incident_candidate(view);
	require(candidate.state == NotificationIncidentCandidateState::ProposalReady, "terminal failure did not create proposal");
	require(candidate.status == "PROPOSAL_READY", "proposal status mismatch");
	require(candidate.action == "OPEN_INCIDENT", "proposal action mismatch");
	require(candidate.classification == "APPROVAL_REQUIRED", "incident proposal bypassed approval governance");
	require(candidate.approval_required, "incident proposal did not require approval");
	require(candidate.terminal_failures == 2, "terminal failure evidence missing");
	require(!candidate.incident_authorized, "proposal was allowed to open incident authoritatively");
	require(!candidate.execution_authorized, "proposal gained execution authority");
}

void test_incident_health_without_terminal_failure_does_not_open_proposal() {
	auto view = operations(NotificationOperationsHealth::IncidentCandidate);
	view.terminal_failed = 0;
	const auto candidate = sentum::operations::derive_notification_incident_candidate(view);
	require(candidate.state == NotificationIncidentCandidateState::None, "incident health without terminal evidence created proposal");
	require(candidate.action.empty(), "missing terminal evidence exposed incident action");
}

void test_unavailable_evidence_blocks_fail_closed() {
	const auto candidate = sentum::operations::derive_notification_incident_candidate(
		sentum::operations::unavailable_notification_operations_view());
	require(candidate.state == NotificationIncidentCandidateState::Blocked, "unavailable evidence did not block proposal");
	require(candidate.status == "BLOCKED", "blocked status mismatch");
	require(candidate.action.empty(), "blocked candidate exposed action");
	require(candidate.classification == "FORBIDDEN", "blocked candidate classification mismatch");
	require(!candidate.approval_required, "blocked candidate requested approval");
	require(!candidate.incident_authorized && !candidate.execution_authorized, "blocked candidate gained authority");
}

void test_json_contract_is_read_only() {
	auto view = operations(NotificationOperationsHealth::IncidentCandidate);
	view.terminal_failed = 1;
	const auto candidate = sentum::operations::derive_notification_incident_candidate(view);
	const auto json = sentum::operations::notification_incident_candidate_json(candidate);
	require(json.value("status", std::string{}) == "PROPOSAL_READY", "json status mismatch");
	require(json.value("classification", std::string{}) == "APPROVAL_REQUIRED", "json governance mismatch");
	require(!json.value("incident_authorized", true), "json granted incident authority");
	require(!json.value("execution_authorized", true), "json granted execution authority");
}

} // namespace

int main() {
	try {
		test_healthy_delivery_has_no_incident_proposal();
		test_attention_remains_advisory();
		test_terminal_failure_creates_approval_required_proposal_only();
		test_incident_health_without_terminal_failure_does_not_open_proposal();
		test_unavailable_evidence_blocks_fail_closed();
		test_json_contract_is_read_only();
		std::cout << "notification incident workflow bridge tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "notification incident workflow bridge test failure: " << error.what() << '\n';
		return 1;
	}
}
