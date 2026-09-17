#include <sentum/operations/NotificationIncidentWorkflowIntegration.hpp>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

nlohmann::json incident_candidate_snapshot() {
	return {
		{"operations_control_plane", {
			{"notification_delivery_evidence", nlohmann::json::array({
				{{"dedup_key", "incident-key"}, {"alert_id", "alert-1"}, {"generation", 1},
				 {"channel", "PAGER"}, {"audience", "OPERATIONS_ON_CALL"}, {"state", "FAILED"},
				 {"attempt", 3}, {"terminal", true}, {"delivery_authorized", true}}
			})}
		}}
	};
}

void test_candidate_does_not_invent_control_plane_evidence() {
	const auto view = sentum::operations::derive_notification_incident_workflow_integration(incident_candidate_snapshot());
	require(view.candidate.status == "PROPOSAL_READY", "incident proposal missing");
	require(view.request_id.empty(), "proposal invented request id");
	require(view.approval_status == "NOT_REQUESTED", "proposal invented approval state");
	require(!view.approval_evidence_available, "proposal invented approval evidence");
	require(!view.audit_evidence_available, "proposal invented audit evidence");
	require(!view.recovery_evidence_available, "proposal invented recovery evidence");
	require(!view.incident_authorized && !view.execution_authorized, "integration gained authority");
}

void test_existing_control_plane_evidence_is_correlated() {
	auto snapshot = incident_candidate_snapshot();
	auto& cp = snapshot["operations_control_plane"];
	cp["incident_workflow"] = {
		{"state", "OPENING"}, {"action", "OPEN_INCIDENT"}, {"request_id", "req-42"},
		{"classification", "APPROVAL_REQUIRED"}
	};
	cp["approval_queue"] = nlohmann::json::array({
		{{"request_id", "req-42"}, {"action", "OPEN_INCIDENT"}, {"status", "PENDING"},
		 {"classification", "APPROVAL_REQUIRED"}}
	});
	cp["audit_timeline"] = nlohmann::json::array({
		{{"request_id", "req-42"}, {"action", "OPEN_INCIDENT"}, {"outcome", "REQUESTED"}}
	});
	cp["recovery_workflow"] = {{"state", "PENDING_RECONCILIATION"}};

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
	cp["approval_queue"] = nlohmann::json::array({
		{{"request_id", "other"}, {"action", "RESUME_ENTRIES"}, {"status", "PENDING"}}
	});
	cp["audit_timeline"] = nlohmann::json::array({
		{{"request_id", "other"}, {"action", "CLEAR_KILL_SWITCH"}, {"outcome", "APPROVED"}}
	});

	const auto view = sentum::operations::derive_notification_incident_workflow_integration(snapshot);
	require(!view.approval_evidence_available, "unrelated approval was attached");
	require(!view.audit_evidence_available, "unrelated audit was attached");
}

void test_json_contract_remains_read_only() {
	const auto view = sentum::operations::derive_notification_incident_workflow_integration(incident_candidate_snapshot());
	const auto json = sentum::operations::notification_incident_workflow_integration_json(view);
	require(json.at("incident_authorized") == false, "json incident authority leak");
	require(json.at("execution_authorized") == false, "json execution authority leak");
	require(json.at("candidate").at("classification") == "APPROVAL_REQUIRED", "candidate classification missing");
}

} // namespace

int main() {
	try {
		test_candidate_does_not_invent_control_plane_evidence();
		test_existing_control_plane_evidence_is_correlated();
		test_unrelated_evidence_is_not_attached();
		test_json_contract_remains_read_only();
		std::cout << "notification incident workflow integration tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "notification incident workflow integration test failure: " << error.what() << '\n';
		return 1;
	}
}
