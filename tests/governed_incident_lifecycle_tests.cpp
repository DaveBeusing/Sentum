#include <sentum/operations/GovernedIncidentLifecycleRepository.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using sentum::operations::GovernedIncidentLifecycleOpenMode;
using sentum::operations::GovernedIncidentLifecycleRepository;

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

std::filesystem::path temporary_path(const char* suffix) {
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() /
		("sentum_incident_lifecycle_" + std::to_string(stamp) + suffix);
}

void cleanup_database(const std::filesystem::path& path) {
	std::error_code error;
	std::filesystem::remove(path, error);
	std::filesystem::remove(path.string() + "-wal", error);
	std::filesystem::remove(path.string() + "-shm", error);
}

template <typename Function>
void require_throws(Function&& function, const char* message) {
	try {
		function();
	} catch (const std::exception&) {
		return;
	}
	throw std::runtime_error(message);
}

void require_authority_isolated(const nlohmann::json& snapshot) {
	require(snapshot.at("execution_authorized") == false, "control plane gained execution authority");
	for (const auto& item : snapshot.at("approval_queue")) {
		require(item.at("execution_authorized") == false, "approval item gained execution authority");
	}
	for (const auto& item : snapshot.at("audit_timeline")) {
		require(item.at("execution_authorized") == false, "audit item gained execution authority");
	}
	if (!snapshot.at("incident_workflow").empty()) {
		require(snapshot.at("incident_workflow").at("execution_authorized") == false,
			"incident workflow gained execution authority");
	}
}

void test_request_approval_and_idempotency() {
	const auto path = temporary_path(".sqlite3");
	try {
		GovernedIncidentLifecycleRepository repository(path.string());
		const auto first = repository.submit_open_incident_request(
			"NOTIFICATION_OPERATIONS:alert-1:1", "notification-runtime", "terminal delivery failure", "sha-1", "evidence-1");
		const auto duplicate = repository.submit_open_incident_request(
			"NOTIFICATION_OPERATIONS:alert-1:1", "notification-runtime", "terminal delivery failure", "sha-1", "evidence-1");
		require(first.created, "first incident request was not created");
		require(!duplicate.created && duplicate.request_id == first.request_id, "duplicate proposal was not idempotent");

		const auto pending = repository.control_plane_snapshot();
		require(pending.at("incident_state") == "APPROVAL_PENDING", "request did not enter approval pending");
		require(pending.at("pending_approvals") == 1, "pending approval count mismatch");
		require(pending.at("approval_queue").size() == 1, "duplicate proposal created a second approval");
		require(pending.at("audit_timeline").size() == 1, "duplicate proposal created duplicate audit history");
		require_authority_isolated(pending);

		require_throws([&] {
			repository.approve_open_incident_request(
				first.request_id, "operator-a", "approve incident", "NOTIFICATION_OPERATIONS:other:1");
		}, "mismatched approval correlation was accepted");

		const auto approval = repository.approve_open_incident_request(
			first.request_id, "operator-a", "approve incident", "NOTIFICATION_OPERATIONS:alert-1:1");
		require(approval.opened && !approval.incident_id.empty(), "approved request did not open incident");

		const auto open = repository.control_plane_snapshot();
		require(open.at("incident_state") == "OPEN", "approved incident was not open");
		require(open.at("pending_approvals") == 0, "approved request remained pending");
		require(open.at("incident_workflow").at("incident_id") == approval.incident_id, "incident id projection mismatch");
		require_authority_isolated(open);

		require_throws([&] {
			repository.approve_open_incident_request(first.request_id, "operator-b", "stale approval");
		}, "stale duplicate approval advanced incident state");
	} catch (...) {
		cleanup_database(path);
		throw;
	}
	cleanup_database(path);
}

void test_denial_creates_no_incident() {
	const auto path = temporary_path("_deny.sqlite3");
	try {
		GovernedIncidentLifecycleRepository repository(path.string());
		const auto request = repository.submit_open_incident_request(
			"NOTIFICATION_OPERATIONS:alert-deny:1", "notification-runtime", "terminal delivery failure");
		repository.deny_open_incident_request(request.request_id, "operator-a", "not actionable");

		const auto snapshot = repository.control_plane_snapshot();
		require(snapshot.at("incident_state") == "DENIED", "denial state was not preserved");
		require(!repository.incident_id_for_request(request.request_id).has_value(), "denied request created an incident");
		require(snapshot.at("pending_approvals") == 0, "denied request remained pending");
		require_authority_isolated(snapshot);
	} catch (...) {
		cleanup_database(path);
		throw;
	}
	cleanup_database(path);
}

void test_lifecycle_transitions_and_recovery_prerequisites() {
	const auto path = temporary_path("_transitions.sqlite3");
	try {
		GovernedIncidentLifecycleRepository repository(path.string());
		const auto request = repository.submit_open_incident_request(
			"NOTIFICATION_OPERATIONS:alert-2:3", "notification-runtime", "terminal delivery failure");
		const auto approval = repository.approve_open_incident_request(
			request.request_id, "operator-a", "validated failure");

		require_throws([&] {
			repository.begin_recovery(approval.incident_id, "operator-a", "recover", "reconciliation-1");
		}, "recovery started before acknowledgement");
		require_throws([&] {
			repository.resolve_incident(approval.incident_id, "operator-a", "resolve before acknowledgement");
		}, "open incident resolved without acknowledgement");

		repository.acknowledge_incident(approval.incident_id, "operator-b", "investigating");
		auto acknowledged = repository.control_plane_snapshot();
		require(acknowledged.at("incident_state") == "ACKNOWLEDGED", "acknowledgement did not preserve distinct state");
		require(acknowledged.at("recovery_state") == "IDLE", "acknowledgement implied recovery");

		require_throws([&] {
			repository.begin_recovery(approval.incident_id, "operator-b", "recover", "");
		}, "recovery started without reconciliation evidence");

		repository.begin_recovery(
			approval.incident_id, "operator-b", "reconciliation accepted for recovery", "reconciliation-42");
		auto recovery = repository.control_plane_snapshot();
		require(recovery.at("incident_state") == "RECOVERY_IN_PROGRESS", "recovery state mismatch");
		require(recovery.at("recovery_state") == "IN_PROGRESS", "recovery projection mismatch");
		require(recovery.at("recovery_workflow").at("reconciliation_evidence_id") == "reconciliation-42",
			"reconciliation evidence missing from recovery projection");

		repository.resolve_incident(approval.incident_id, "operator-c", "service restored");
		auto resolved = repository.control_plane_snapshot();
		require(resolved.at("incident_state") == "RESOLVED", "incident did not resolve");
		require(resolved.at("recovery_state") == "RESOLVED", "resolved recovery state mismatch");

		repository.close_incident(approval.incident_id, "operator-c", "post-incident review recorded");
		auto closed = repository.control_plane_snapshot();
		require(closed.at("incident_state") == "NONE", "closed incident remained active");
		require(closed.at("incident_workflow").at("state") == "CLOSED", "closed state not auditable");
		require(closed.at("recovery_state") == "CLOSED", "closed recovery projection mismatch");
		require_authority_isolated(closed);

		require_throws([&] {
			repository.acknowledge_incident(approval.incident_id, "operator-c", "invalid repeat");
		}, "closed incident accepted invalid transition");
	} catch (...) {
		cleanup_database(path);
		throw;
	}
	cleanup_database(path);
}

void test_restart_reconstructs_every_non_terminal_state() {
	const auto path = temporary_path("_restart.sqlite3");
	std::string request_id;
	std::string incident_id;
	try {
		{
			GovernedIncidentLifecycleRepository repository(path.string());
			request_id = repository.submit_open_incident_request(
				"NOTIFICATION_OPERATIONS:restart:1", "notification-runtime", "terminal delivery failure").request_id;
		}
		{
			GovernedIncidentLifecycleRepository repository(path.string());
			require(repository.control_plane_snapshot().at("incident_state") == "APPROVAL_PENDING",
				"restart lost approval-pending state");
			incident_id = repository.approve_open_incident_request(
				request_id, "operator-a", "approved after restart").incident_id;
		}
		{
			GovernedIncidentLifecycleRepository repository(path.string());
			require(repository.control_plane_snapshot().at("incident_state") == "OPEN", "restart lost open state");
			repository.acknowledge_incident(incident_id, "operator-a", "acknowledged after restart");
		}
		{
			GovernedIncidentLifecycleRepository repository(path.string());
			require(repository.control_plane_snapshot().at("incident_state") == "ACKNOWLEDGED",
				"restart lost acknowledged state");
			repository.begin_recovery(incident_id, "operator-a", "begin recovery", "reconciliation-restart");
		}
		{
			GovernedIncidentLifecycleRepository repository(path.string());
			const auto snapshot = repository.control_plane_snapshot();
			require(snapshot.at("incident_state") == "RECOVERY_IN_PROGRESS", "restart lost recovery state");
			require(snapshot.at("recovery_workflow").at("reconciliation_evidence_id") == "reconciliation-restart",
				"restart lost reconciliation evidence");
			repository.resolve_incident(incident_id, "operator-a", "resolved after restart");
		}
		{
			GovernedIncidentLifecycleRepository repository(path.string());
			require(repository.control_plane_snapshot().at("incident_state") == "RESOLVED",
				"restart lost resolved state");
		}
	} catch (...) {
		cleanup_database(path);
		throw;
	}
	cleanup_database(path);
}

void test_merge_preserves_other_control_plane_evidence() {
	const auto path = temporary_path("_merge.sqlite3");
	try {
		GovernedIncidentLifecycleRepository repository(path.string());
		const auto request = repository.submit_open_incident_request(
			"NOTIFICATION_OPERATIONS:merge:1", "notification-runtime", "terminal delivery failure");

		nlohmann::json base = {
			{"operations_control_plane", {
				{"governance_state", "CONTROLLED"},
				{"evidence_status", "AVAILABLE"},
				{"pending_approvals", 3},
				{"approval_queue", nlohmann::json::array({
					{{"request_id", "maintenance-1"}, {"action", "enter_maintenance"},
					 {"classification", "APPROVAL_REQUIRED"}, {"actor", "operator-z"},
					 {"reason", "maintenance window"}, {"status", "PENDING"}}
				})},
				{"audit_timeline", nlohmann::json::array({
					{{"request_id", "maintenance-0"}, {"action", "enter_maintenance"},
					 {"actor", "operator-z"}, {"reason", "scheduled"},
					 {"outcome", "APPROVED"}, {"timestamp_utc", "2026-09-22T20:00:00Z"}}
				})},
				{"audit_timeline_total", 5},
				{"audit_timeline_truncated", true},
				{"recovery_state", "WAITING_RECONCILIATION"},
				{"recovery_workflow", {
					{"state", "WAITING_RECONCILIATION"},
					{"request_id", "recovery-general"},
					{"execution_authorized", false}
				}}
			}}
		};

		auto merged = sentum::operations::merge_governed_incident_lifecycle_snapshot(base, repository);
		const auto& control = merged.at("operations_control_plane");
		require(control.at("incident_state") == "APPROVAL_PENDING", "incident state was not projected");
		require(control.at("pending_approvals") == 4, "non-incident pending approval total was replaced");
		require(control.at("approval_queue").size() == 2, "non-incident approval row was replaced");
		require(control.at("approval_queue_truncated") == true, "combined bounded approval queue lost truncation");
		require(control.at("audit_timeline").size() == 2, "non-incident audit row was replaced");
		require(control.at("audit_timeline_total") == 6, "bounded non-incident audit total was lost");
		require(control.at("audit_timeline_truncated") == true, "combined audit truncation was lost");
		require(control.at("recovery_state") == "WAITING_RECONCILIATION",
			"incident lifecycle replaced general recovery state");
		require(control.at("incident_recovery_state") == "IDLE", "idle incident recovery projection mismatch");
		require(control.at("recovery_workflow").at("request_id") == "recovery-general",
			"incident lifecycle replaced general recovery workflow");

		const auto incident = repository.approve_open_incident_request(
			request.request_id, "operator-a", "approve merge test").incident_id;
		repository.acknowledge_incident(incident, "operator-a", "investigating");
		repository.begin_recovery(incident, "operator-a", "reconciled", "reconciliation-merge");

		merged = sentum::operations::merge_governed_incident_lifecycle_snapshot(base, repository);
		const auto& recovering = merged.at("operations_control_plane");
		require(recovering.at("recovery_state") == "WAITING_RECONCILIATION",
			"incident recovery replaced active general recovery state");
		require(recovering.at("incident_recovery_state") == "IN_PROGRESS",
			"incident recovery state was not projected separately");
		require(recovering.at("incident_recovery_workflow").at("reconciliation_evidence_id") == "reconciliation-merge",
			"incident recovery evidence was not projected");
		require(recovering.at("recovery_workflow").at("request_id") == "recovery-general",
			"incident recovery replaced general recovery workflow");
	} catch (...) {
		cleanup_database(path);
		throw;
	}
	cleanup_database(path);
}

void test_resolution_without_recovery_keeps_recovery_idle() {
	const auto path = temporary_path("_resolve_without_recovery.sqlite3");
	try {
		GovernedIncidentLifecycleRepository repository(path.string());
		const auto request = repository.submit_open_incident_request(
			"NOTIFICATION_OPERATIONS:no-recovery:1", "notification-runtime", "terminal delivery failure");
		const auto incident = repository.approve_open_incident_request(
			request.request_id, "operator-a", "approve incident").incident_id;
		repository.acknowledge_incident(incident, "operator-a", "acknowledged");
		repository.resolve_incident(incident, "operator-a", "resolved without recovery");

		const auto snapshot = repository.control_plane_snapshot();
		require(snapshot.at("incident_state") == "RESOLVED", "incident did not resolve");
		require(snapshot.at("recovery_state") == "IDLE", "resolution without recovery invented recovery state");
		require(snapshot.at("recovery_workflow").empty(), "resolution without recovery invented recovery evidence");
	} catch (...) {
		cleanup_database(path);
		throw;
	}
	cleanup_database(path);
}

void test_read_only_and_bounded_history_fail_closed() {
	const auto path = temporary_path("_readonly.sqlite3");
	try {
		std::string request_id;
		{
			GovernedIncidentLifecycleRepository writer(path.string());
			request_id = writer.submit_open_incident_request(
				"NOTIFICATION_OPERATIONS:read-only:1", "notification-runtime", "terminal delivery failure").request_id;
		}
		{
			GovernedIncidentLifecycleRepository reader(path.string(), GovernedIncidentLifecycleOpenMode::ReadOnly);
			require(reader.control_plane_snapshot(1, 1).at("incident_state") == "APPROVAL_PENDING",
				"read-only restart did not reconstruct state");
			require_throws([&] {
				reader.approve_open_incident_request(request_id, "operator-a", "must fail");
			}, "read-only repository accepted mutation");
		}
	} catch (...) {
		cleanup_database(path);
		throw;
	}
	cleanup_database(path);
}

} // namespace

int main() {
	try {
		test_request_approval_and_idempotency();
		test_denial_creates_no_incident();
		test_lifecycle_transitions_and_recovery_prerequisites();
		test_restart_reconstructs_every_non_terminal_state();
		test_merge_preserves_other_control_plane_evidence();
		test_resolution_without_recovery_keeps_recovery_idle();
		test_read_only_and_bounded_history_fail_closed();
		std::cout << "governed incident lifecycle tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "governed incident lifecycle test failure: " << error.what() << '\n';
		return 1;
	}
}
