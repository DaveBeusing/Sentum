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
		test_read_only_and_bounded_history_fail_closed();
		std::cout << "governed incident lifecycle tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "governed incident lifecycle test failure: " << error.what() << '\n';
		return 1;
	}
}
