#include <sentum/ui/CrossSurfaceSemanticParity.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

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
			{"evidence_status", "AVAILABLE"},
			{"maintenance_state", "NORMAL"},
			{"incident_state", "NONE"},
			{"recovery_state", "IDLE"},
			{"pending_approvals", 1},
			{"approval_queue", nlohmann::json::array({
				{{"request_id", "req-1"}, {"action", "enter_maintenance"}, {"classification", "APPROVAL_REQUIRED"}, {"actor", "operator-a"}, {"reason", "planned"}}
			})},
			{"maintenance_workflow", {
				{"state", "REQUESTED"}, {"action", "enter_maintenance"}, {"classification", "APPROVAL_REQUIRED"}, {"request_id", "maint-1"}
			}}
		}}
	};
}

void require_no_drift(const nlohmann::json& snapshot, const char* message) {
	const auto web = sentum::ui::derive_cross_surface_operations_view(snapshot);
	const auto drift = sentum::ui::detect_cross_surface_semantic_drift(snapshot, web);
	require(drift.empty(), message);
}

void test_healthy_semantics_match() {
	require_no_drift(healthy_snapshot(), "healthy terminal/web semantics drifted");
}

void test_critical_semantics_match() {
	auto snapshot = healthy_snapshot();
	snapshot["kill_switch_active"] = true;
	require_no_drift(snapshot, "critical terminal/web semantics drifted");
}

void test_missing_governance_semantics_match() {
	auto snapshot = healthy_snapshot();
	snapshot.erase("operations_control_plane");
	require_no_drift(snapshot, "missing-governance terminal/web semantics drifted");
	const auto web = sentum::ui::derive_cross_surface_operations_view(snapshot);
	require(web["governance"]["state"] == "UNAVAILABLE", "missing governance did not remain unavailable");
	require(web["governance"]["evidence_state"] == "MISSING", "missing evidence did not remain explicit");
}

void test_stale_semantics_match_and_fail_closed() {
	auto snapshot = healthy_snapshot();
	snapshot["operations_control_plane"]["evidence_status"] = "STALE";
	require_no_drift(snapshot, "stale terminal/web semantics drifted");
	const auto web = sentum::ui::derive_cross_surface_operations_view(snapshot);
	const auto& row = web["approval_queue"]["items"][0];
	require(row["classification"] == "FORBIDDEN", "stale approval classification did not fail closed");
	require(row["status"] == "BLOCKED - STALE EVIDENCE", "stale approval status did not fail closed");
}

void test_schema_or_authority_drift_is_detected() {
	const auto snapshot = healthy_snapshot();
	auto web = sentum::ui::derive_cross_surface_operations_view(snapshot);
	web["schema_version"] = 2;
	web["authority"] = "WRITE_ENABLED";
	const auto drift = sentum::ui::detect_cross_surface_semantic_drift(snapshot, web);
	require(!drift.empty(), "schema/authority drift was not detected");
	require(drift.front() == "schema/authority contract invalid", "schema/authority drift did not fail closed first");
}

void test_semantic_value_drift_is_detected() {
	const auto snapshot = healthy_snapshot();
	auto web = sentum::ui::derive_cross_surface_operations_view(snapshot);
	web["runtime"]["label"] = "NORMAL";
	web["governance"]["state"] = "UNCONTROLLED";
	web["approval_queue"]["items"][0]["classification"] = "AUTOMATED";
	const auto drift = sentum::ui::detect_cross_surface_semantic_drift(snapshot, web);
	require(drift.size() >= 2, "semantic drift was not detected");
	bool governance = false;
	bool approvals = false;
	for (const auto& item : drift) {
		governance = governance || item == "governance.state";
		approvals = approvals || item == "approval_queue.semantic_projection";
	}
	require(governance, "governance drift missing from report");
	require(approvals, "approval semantic drift missing from report");
}

void test_contract_identifier_is_stable() {
	const auto web = sentum::ui::derive_cross_surface_operations_view(healthy_snapshot());
	require(web["schema_version"] == sentum::ui::kCrossSurfaceOperationsSchemaVersion, "schema version mismatch");
	require(web["contract"] == sentum::ui::kCrossSurfaceOperationsContract, "contract identifier mismatch");
	require(web["authority"] == "READ_ONLY_PRESENTATION", "authority contract mismatch");
}

} // namespace

int main() {
	try {
		test_healthy_semantics_match();
		test_critical_semantics_match();
		test_missing_governance_semantics_match();
		test_stale_semantics_match_and_fail_closed();
		test_schema_or_authority_drift_is_detected();
		test_semantic_value_drift_is_detected();
		test_contract_identifier_is_stable();
		std::cout << "cross-surface semantic parity tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "cross-surface semantic parity test failure: " << error.what() << '\n';
		return 1;
	}
}
