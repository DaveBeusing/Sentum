#include <sentum/ui/CrossSurfaceOperationsView.hpp>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

nlohmann::json healthy_snapshot() {
	return {
		{"health", "healthy"},
		{"kill_switch_active", false},
		{"market_data_connected", true},
		{"entries_paused", false},
		{"performance", {{"queue_pressure", "normal"}}},
		{"operations_control_plane", {
			{"governance_state", "CONTROLLED"},
			{"maintenance_state", "NORMAL"},
			{"incident_state", "NONE"},
			{"recovery_state", "IDLE"},
			{"pending_approvals", 1},
			{"last_audit_action", "enter_maintenance"},
			{"last_audit_actor", "operator-a"},
			{"notification_delivery_evidence", nlohmann::json::array()},
			{"approval_queue", nlohmann::json::array({
				{{"request_id", "req-1"}, {"action", "enter_maintenance"}, {"classification", "APPROVAL_REQUIRED"}, {"actor", "operator-a"}, {"reason", "planned"}}
			})},
			{"audit_timeline", nlohmann::json::array({
				{{"timestamp_utc", "2026-09-16T17:00:00Z"}, {"request_id", "req-0"}, {"action", "acknowledge_incident"}, {"actor", "operator-b"}, {"reason", "triage"}, {"outcome", "RECORDED"}}
			})}
		}}
	};
}

void test_healthy_view_matches_terminal_semantics() {
	const auto view = sentum::ui::derive_cross_surface_operations_view(healthy_snapshot());
	require(view["schema_version"] == 1, "schema version mismatch");
	require(view["authority"] == "READ_ONLY_PRESENTATION", "authority boundary missing");
	require(view["runtime"]["severity"] == "NORMAL", "runtime severity mismatch");
	require(view["governance"]["state"] == "CONTROLLED", "governance state mismatch");
	require(view["governance"]["evidence_state"] == "AVAILABLE", "evidence state mismatch");
	require(view["approval_queue"]["total"] == 1, "approval total mismatch");
	require(view["approval_queue"]["items"][0]["classification"] == "APPROVAL_REQUIRED", "approval classification mismatch");
	require(!view["approval_queue"]["items"][0]["execution_authorized"].get<bool>(), "presentation authorized execution");
	require(view["notification_incident_workflow"]["candidate"]["status"] == "NONE", "healthy notification state created incident proposal");
}

void test_notification_incident_proposal_is_cross_surface_read_only() {
	auto snapshot = healthy_snapshot();
	snapshot["operations_control_plane"]["notification_delivery_evidence"] = nlohmann::json::array({
		{{"dedup_key", "incident-key"}, {"alert_id", "alert-1"}, {"generation", 1}, {"channel", "PAGER"},
		 {"audience", "OPERATIONS_ON_CALL"}, {"state", "FAILED"}, {"attempt", 3}, {"terminal", true}}
	});
	const auto view = sentum::ui::derive_cross_surface_operations_view(snapshot);
	const auto& workflow = view["notification_incident_workflow"];
	require(workflow["candidate"]["status"] == "PROPOSAL_READY", "incident proposal missing from cross-surface contract");
	require(workflow["candidate"]["action"] == "OPEN_INCIDENT", "incident proposal action mismatch");
	require(workflow["candidate"]["classification"] == "APPROVAL_REQUIRED", "incident proposal governance missing");
	require(workflow["incident_authorized"] == false, "cross-surface contract granted incident authority");
	require(workflow["execution_authorized"] == false, "cross-surface contract granted execution authority");
}

void test_notification_incident_evidence_is_correlated_not_invented() {
	auto snapshot = healthy_snapshot();
	auto& cp = snapshot["operations_control_plane"];
	cp["notification_delivery_evidence"] = nlohmann::json::array({
		{{"dedup_key", "incident-key"}, {"alert_id", "alert-1"}, {"generation", 1}, {"channel", "PAGER"},
		 {"audience", "OPERATIONS_ON_CALL"}, {"state", "FAILED"}, {"attempt", 3}, {"terminal", true}}
	});
	cp["incident_workflow"] = {{"state", "OPENING"}, {"action", "OPEN_INCIDENT"}, {"request_id", "req-42"}, {"classification", "APPROVAL_REQUIRED"}};
	cp["approval_queue"].push_back({{"request_id", "req-42"}, {"action", "OPEN_INCIDENT"}, {"classification", "APPROVAL_REQUIRED"}, {"status", "PENDING"}});
	cp["audit_timeline"].push_back({{"request_id", "req-42"}, {"action", "OPEN_INCIDENT"}, {"outcome", "REQUESTED"}});
	cp["recovery_workflow"] = {{"state", "PENDING_RECONCILIATION"}};
	const auto view = sentum::ui::derive_cross_surface_operations_view(snapshot);
	const auto& workflow = view["notification_incident_workflow"];
	require(workflow["request_id"] == "req-42", "request id not correlated");
	require(workflow["approval_evidence_available"] == true, "approval evidence unavailable");
	require(workflow["audit_evidence_available"] == true, "audit evidence unavailable");
	require(workflow["recovery_evidence_available"] == true, "recovery evidence unavailable");
	require(workflow["recovery_state"] == "PENDING_RECONCILIATION", "recovery evidence mismatch");
}

void test_stale_evidence_fails_closed_across_surfaces() {
	auto snapshot = healthy_snapshot();
	snapshot["operations_control_plane"]["evidence_status"] = "STALE";
	const auto view = sentum::ui::derive_cross_surface_operations_view(snapshot);
	require(view["governance"]["evidence_state"] == "STALE", "stale evidence state lost");
	require(view["approval_queue"]["items"][0]["classification"] == "FORBIDDEN", "stale approval not fail-closed");
	require(view["approval_queue"]["items"][0]["status"] == "BLOCKED - STALE EVIDENCE", "stale approval status mismatch");
}

void test_missing_evidence_remains_visible() {
	auto snapshot = healthy_snapshot();
	snapshot.erase("operations_control_plane");
	const auto view = sentum::ui::derive_cross_surface_operations_view(snapshot);
	require(view["governance"]["state"] == "UNAVAILABLE", "missing governance became healthy");
	require(view["governance"]["evidence_state"] == "MISSING", "missing evidence state lost");
	require(view["approval_queue"]["total"] == 0, "missing evidence invented approvals");
	require(view["notification_incident_workflow"]["candidate"]["status"] == "BLOCKED", "missing notification evidence did not fail closed");
}

} // namespace

int main() {
	try {
		test_healthy_view_matches_terminal_semantics();
		test_notification_incident_proposal_is_cross_surface_read_only();
		test_notification_incident_evidence_is_correlated_not_invented();
		test_stale_evidence_fails_closed_across_surfaces();
		test_missing_evidence_remains_visible();
		std::cout << "cross-surface operations view tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "cross-surface operations view test failure: " << error.what() << '\n';
		return 1;
	}
}
