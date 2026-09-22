#include <sentum/operations/GovernedIncidentLifecycleRuntime.hpp>
#include <sentum/operations/NotificationDeliveryEvidenceRepository.hpp>
#include <sentum/ui/CrossSurfaceOperationsView.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

std::filesystem::path temporary_path(const char* suffix) {
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() /
		("sentum_incident_runtime_" + std::to_string(stamp) + suffix);
}

void cleanup_database(const std::filesystem::path& path) {
	std::error_code error;
	std::filesystem::remove(path, error);
	std::filesystem::remove(path.string() + "-wal", error);
	std::filesystem::remove(path.string() + "-shm", error);
}

void seed_terminal_failure(const std::filesystem::path& path) {
	sentum::operations::NotificationDeliveryEvidenceRepository repository(path.string());
	sentum::operations::NotificationDeliveryEvidenceRecord record;
	record.dedup_key = "notification-alert-7-generation-2-pager";
	record.alert_id = "alert-7";
	record.generation = 2;
	record.channel = "PAGER";
	record.audience = "OPERATIONS_ON_CALL";
	record.state = "FAILED";
	record.attempt = 3;
	record.max_attempts = 3;
	record.terminal = true;
	record.failure_code = "DELIVERY_REJECTED";
	record.failure_reason = "provider rejected notification";
	record.observed_at_utc = "2026-09-23T00:00:00Z";
	record.delivery_authorized = true;
	record.execution_authorized = false;
	require(repository.append(record), "failed to seed terminal notification failure");
}

void test_runtime_creates_only_governed_request() {
	const auto path = temporary_path(".sqlite3");
	try {
		seed_terminal_failure(path);
		sentum::operations::GovernedIncidentLifecycleRuntime runtime(
			path.string(), "test-sha", std::chrono::milliseconds(100));
		runtime.tick_once();

		sentum::operations::GovernedIncidentLifecycleRepository reader(
			path.string(), sentum::operations::GovernedIncidentLifecycleOpenMode::ReadOnly);
		const auto snapshot = reader.control_plane_snapshot();
		require(snapshot.at("incident_state") == "APPROVAL_PENDING",
			"terminal notification failure did not create governed approval request");
		require(snapshot.at("pending_approvals") == 1, "governed request count mismatch");
		require(snapshot.at("approval_queue").at(0).at("action") == "OPEN_INCIDENT", "request action mismatch");
		require(snapshot.at("approval_queue").at(0).at("classification") == "APPROVAL_REQUIRED",
			"runtime bypassed approval classification");
		require(!reader.incident_id_for_request(
			snapshot.at("approval_queue").at(0).at("request_id").get<std::string>()).has_value(),
			"runtime opened incident without explicit approval");

		runtime.tick_once();
		const auto duplicate = reader.control_plane_snapshot();
		require(duplicate.at("pending_approvals") == 1, "repeated runtime tick duplicated request");
		require(duplicate.at("approval_queue").size() == 1, "repeated runtime tick duplicated approval row");
	} catch (...) {
		cleanup_database(path);
		throw;
	}
	cleanup_database(path);
}

void test_cross_surface_uses_authoritative_lifecycle_state() {
	const auto path = temporary_path("_projection.sqlite3");
	try {
		seed_terminal_failure(path);
		sentum::operations::GovernedIncidentLifecycleRuntime runtime(path.string(), "test-sha");
		runtime.tick_once();

		sentum::operations::NotificationDeliveryEvidenceRepository notification(
			path.string(), sentum::operations::NotificationDeliveryEvidenceOpenMode::ReadOnly);
		sentum::operations::GovernedIncidentLifecycleRepository lifecycle(
			path.string(), sentum::operations::GovernedIncidentLifecycleOpenMode::ReadOnly);
		nlohmann::json base = {
			{"health", "healthy"},
			{"kill_switch_active", true},
			{"market_data_connected", true},
			{"entries_paused", true},
			{"performance", {{"queue_pressure", "normal"}}}
		};

		auto view = sentum::ui::derive_cross_surface_operations_view(base, notification, lifecycle);
		const auto request_id = view.at("notification_incident_workflow").at("request_id").get<std::string>();
		require(!request_id.empty(), "cross-surface projection lost durable request identity");
		require(view.at("notification_incident_workflow").at("approval_status") == "PENDING",
			"cross-surface projection lost pending approval evidence");
		require(view.at("notification_incident_workflow").at("incident_state") == "APPROVAL_PENDING",
			"cross-surface projection lost lifecycle state");
		require(view.at("notification_incident_workflow").at("execution_authorized") == false,
			"cross-surface incident projection gained execution authority");
		require(base.at("kill_switch_active") == true,
			"incident projection changed kill-switch evidence");
		require(base.at("entries_paused") == true,
			"incident projection changed entry-pause evidence");

		{
			sentum::operations::GovernedIncidentLifecycleRepository writer(path.string());
			writer.approve_open_incident_request(
				request_id,
				"operator-a",
				"validated notification incident",
				"NOTIFICATION_OPERATIONS:alert-7:2");
		}
		view = sentum::ui::derive_cross_surface_operations_view(base, notification, lifecycle);
		require(view.at("notification_incident_workflow").at("approval_status") == "APPROVED",
			"decided approval evidence disappeared from cross-surface projection");
		require(view.at("notification_incident_workflow").at("incident_state") == "OPEN",
			"approved incident state missing from cross-surface projection");
		require(view.at("notification_incident_workflow").at("execution_authorized") == false,
			"approved incident projection gained execution authority");
	} catch (...) {
		cleanup_database(path);
		throw;
	}
	cleanup_database(path);
}

void test_runtime_restart_preserves_explicit_decision() {
	const auto path = temporary_path("_restart.sqlite3");
	try {
		seed_terminal_failure(path);
		std::string request_id;
		std::string incident_id;
		{
			sentum::operations::GovernedIncidentLifecycleRuntime runtime(path.string(), "test-sha");
			runtime.tick_once();
			sentum::operations::GovernedIncidentLifecycleRepository repository(path.string());
			const auto snapshot = repository.control_plane_snapshot();
			request_id = snapshot.at("approval_queue").at(0).at("request_id").get<std::string>();
			incident_id = repository.approve_open_incident_request(
				request_id,
				"operator-a",
				"validated notification delivery incident",
				"NOTIFICATION_OPERATIONS:alert-7:2").incident_id;
		}
		{
			sentum::operations::GovernedIncidentLifecycleRuntime runtime(path.string(), "test-sha");
			runtime.tick_once();
			sentum::operations::GovernedIncidentLifecycleRepository reader(
				path.string(), sentum::operations::GovernedIncidentLifecycleOpenMode::ReadOnly);
			const auto snapshot = reader.control_plane_snapshot();
			require(snapshot.at("incident_state") == "OPEN", "restart lost approved incident state");
			require(snapshot.at("pending_approvals") == 0, "restart recreated decided approval request");
			require(snapshot.at("incident_workflow").at("incident_id") == incident_id,
				"restart changed incident identity");
		}
	} catch (...) {
		cleanup_database(path);
		throw;
	}
	cleanup_database(path);
}

void test_runtime_thread_and_readers_share_state_safely() {
	const auto path = temporary_path("_concurrency.sqlite3");
	try {
		seed_terminal_failure(path);
		sentum::operations::GovernedIncidentLifecycleRuntime runtime(
			path.string(), "test-sha", std::chrono::milliseconds(10));
		runtime.start();
		{
			sentum::operations::GovernedIncidentLifecycleRepository reader(
				path.string(), sentum::operations::GovernedIncidentLifecycleOpenMode::ReadOnly);
			for (int index = 0; index < 25; ++index) {
				const auto snapshot = reader.control_plane_snapshot();
				require(snapshot.at("execution_authorized") == false,
					"concurrent lifecycle projection gained execution authority");
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}
		runtime.stop();

		sentum::operations::GovernedIncidentLifecycleRepository reader(
			path.string(), sentum::operations::GovernedIncidentLifecycleOpenMode::ReadOnly);
		require(reader.control_plane_snapshot().at("pending_approvals") == 1,
			"runtime thread did not preserve idempotent pending request");
	} catch (...) {
		cleanup_database(path);
		throw;
	}
	cleanup_database(path);
}

void test_missing_notification_evidence_never_invents_incident() {
	const auto path = temporary_path("_missing.sqlite3");
	try {
		{
			sentum::operations::GovernedIncidentLifecycleRepository initialize(path.string());
		}
		sentum::operations::GovernedIncidentLifecycleRuntime runtime(path.string(), "test-sha");
		runtime.tick_once();
		sentum::operations::GovernedIncidentLifecycleRepository reader(
			path.string(), sentum::operations::GovernedIncidentLifecycleOpenMode::ReadOnly);
		const auto snapshot = reader.control_plane_snapshot();
		require(snapshot.at("incident_state") == "NONE", "missing notification evidence invented incident state");
		require(snapshot.at("pending_approvals") == 0, "missing notification evidence invented approval request");
		require(snapshot.at("execution_authorized") == false, "missing evidence granted execution authority");
	} catch (...) {
		cleanup_database(path);
		throw;
	}
	cleanup_database(path);
}

} // namespace

int main() {
	try {
		test_runtime_creates_only_governed_request();
		test_cross_surface_uses_authoritative_lifecycle_state();
		test_runtime_restart_preserves_explicit_decision();
		test_runtime_thread_and_readers_share_state_safely();
		test_missing_notification_evidence_never_invents_incident();
		std::cout << "governed incident lifecycle runtime tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "governed incident lifecycle runtime test failure: " << error.what() << '\n';
		return 1;
	}
}
