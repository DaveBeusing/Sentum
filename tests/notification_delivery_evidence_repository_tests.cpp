#include <sentum/operations/NotificationDeliveryEvidenceRepository.hpp>
#include <sentum/operations/NotificationIncidentWorkflowBridge.hpp>
#include <sentum/operations/NotificationOperationsObservability.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using sentum::operations::NotificationDeliveryAttempt;
using sentum::operations::NotificationDeliveryEvidenceRecord;
using sentum::operations::NotificationDeliveryEvidenceRepository;
using sentum::operations::NotificationDeliveryState;
using sentum::operations::NotificationIncidentCandidateState;
using sentum::operations::NotificationOperationsHealth;

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

std::filesystem::path temporary_database_path(const char* suffix = "") {
	static std::uint64_t counter = 0;
	const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() /
		("sentum_notification_delivery_" + std::to_string(tick) + "_" + std::to_string(++counter) + suffix + ".sqlite3");
}

void cleanup_database(const std::filesystem::path& path) {
	std::error_code error;
	std::filesystem::remove(path, error);
	std::filesystem::remove(path.string() + "-wal", error);
	std::filesystem::remove(path.string() + "-shm", error);
}

NotificationDeliveryEvidenceRecord record(
	std::string key,
	std::string state,
	std::size_t attempt = 0,
	bool terminal = false) {
	NotificationDeliveryEvidenceRecord value;
	value.dedup_key = std::move(key);
	value.alert_id = "runtime.health";
	value.generation = 2;
	value.channel = "PAGER";
	value.audience = "OPERATIONS_ON_CALL";
	value.state = std::move(state);
	value.attempt = attempt;
	value.max_attempts = 3;
	value.retry_backoff_seconds = 0;
	value.terminal = terminal;
	value.observed_at_utc = "2026-09-23T00:00:00Z";
	value.delivery_authorized = true;
	value.execution_authorized = false;
	return value;
}

void test_append_readback_and_exact_field_preservation() {
	const auto path = temporary_database_path();
	{
		NotificationDeliveryEvidenceRepository repository(path.string());
		auto value = record("alert:g2:l3:PAGER:OPS", "FAILED", 2, false);
		value.max_attempts = 4;
		value.retry_backoff_seconds = 10;
		value.provider_reference = "provider-17";
		value.failure_code = "TIMEOUT";
		value.failure_reason = "provider timeout";
		require(repository.append(value), "initial durable evidence was not inserted");

		const auto batch = repository.load_recent(16);
		require(batch.total == 1 && !batch.truncated && batch.records.size() == 1, "durable read-back count mismatch");
		require(batch.records.front().sequence > 0, "persistent sequence was not assigned");
		const auto& restored = batch.records.front().record;
		require(restored.dedup_key == value.dedup_key, "dedup key changed during persistence");
		require(restored.alert_id == value.alert_id, "alert id changed during persistence");
		require(restored.generation == value.generation, "generation changed during persistence");
		require(restored.channel == value.channel && restored.audience == value.audience, "route identity changed during persistence");
		require(restored.state == value.state && restored.attempt == value.attempt, "delivery state changed during persistence");
		require(restored.max_attempts == 4 && restored.retry_backoff_seconds == 10, "retry context changed during persistence");
		require(restored.provider_reference == "provider-17", "provider reference changed during persistence");
		require(restored.failure_code == "TIMEOUT" && restored.failure_reason == "provider timeout", "failure evidence changed during persistence");
		require(restored.observed_at_utc == value.observed_at_utc, "observation timestamp changed during persistence");
		require(restored.delivery_authorized, "delivery authorization changed during persistence");
		require(!restored.execution_authorized, "durable evidence granted execution authority");
	}
	cleanup_database(path);
}

void test_duplicate_suppression_survives_restart() {
	const auto path = temporary_database_path();
	const auto dispatched = record("alert:g2:l3:PAGER:OPS", "DISPATCHED", 1, false);
	{
		NotificationDeliveryEvidenceRepository repository(path.string());
		require(repository.append(dispatched), "first dispatch evidence was not inserted");
		require(!repository.append(dispatched), "same-process duplicate was inserted");
	}
	{
		NotificationDeliveryEvidenceRepository repository(path.string());
		require(!repository.append(dispatched), "restart replay inserted a duplicate transition");
		const auto batch = repository.load_recent(16);
		require(batch.total == 1 && batch.records.size() == 1, "restart-safe idempotency count mismatch");
	}
	cleanup_database(path);
}

void test_append_only_transitions_and_generations() {
	const auto path = temporary_database_path();
	{
		NotificationDeliveryEvidenceRepository repository(path.string());
		auto pending = record("alert:g2:l3:PAGER:OPS", "PENDING", 0, false);
		auto dispatched = record("alert:g2:l3:PAGER:OPS", "DISPATCHED", 1, false);
		auto delivered = record("alert:g2:l3:PAGER:OPS", "DELIVERED", 1, true);
		delivered.provider_reference = "provider-18";
		auto next_generation = record("alert:g3:l3:PAGER:OPS", "PENDING", 0, false);
		next_generation.generation = 3;

		require(repository.append(pending), "pending transition missing");
		require(repository.append(dispatched), "dispatched transition missing");
		require(repository.append(delivered), "delivered transition missing");
		require(repository.append(next_generation), "next generation was incorrectly deduplicated");

		const auto history = repository.load_recent(16);
		require(history.total == 4 && history.records.size() == 4, "append-only history count mismatch");
		for (std::size_t index = 1; index < history.records.size(); ++index) {
			require(history.records[index - 1].sequence < history.records[index].sequence, "persistent ordering is not monotonic");
		}
		const auto latest = repository.load_latest_per_dedup_key(16);
		require(latest.total == 2 && latest.records.size() == 2, "latest-state-by-dedup-key reduction mismatch");
		require(latest.records[0].record.state == "DELIVERED", "latest terminal state was not retained");
		require(latest.records[1].record.generation == 3, "new alert generation was not independently retained");
	}
	cleanup_database(path);
}

void test_restart_recovery_preserves_terminal_and_retriable_state() {
	const auto path = temporary_database_path();
	{
		NotificationDeliveryEvidenceRepository repository(path.string());
		auto retryable = record("retryable", "FAILED", 1, false);
		retryable.max_attempts = 3;
		retryable.retry_backoff_seconds = 5;
		retryable.failure_code = "TEMP";
		retryable.failure_reason = "temporary provider failure";
		auto terminal = record("terminal", "FAILED", 3, true);
		terminal.max_attempts = 3;
		terminal.failure_code = "TEMP";
		require(repository.append(retryable), "retryable evidence missing");
		require(repository.append(terminal), "terminal evidence missing");
	}
	{
		NotificationDeliveryEvidenceRepository repository(path.string());
		const auto active = repository.restore_active_delivery_state(16);
		require(active.size() == 1, "restart restored terminal delivery state as active");
		const NotificationDeliveryAttempt& retryable = active.front();
		require(retryable.dedup_key == "retryable", "wrong delivery state restored");
		require(retryable.state == NotificationDeliveryState::Failed, "retryable failure state changed after restart");
		require(retryable.attempt == 1 && retryable.max_attempts == 3, "retry attempt context changed after restart");
		require(retryable.retry_backoff_seconds == 5, "retry backoff context changed after restart");
		require(sentum::operations::notification_delivery_can_retry(retryable), "retriable failure lost retry eligibility after restart");
		require(!retryable.execution_authorized, "restored delivery state granted execution authority");

		const auto latest = repository.load_latest_per_dedup_key(16);
		require(latest.records.size() == 2, "terminal evidence disappeared after restart");
		bool terminal_found = false;
		for (const auto& item : latest.records) {
			if (item.record.dedup_key == "terminal") terminal_found = item.record.terminal;
			require(!item.record.execution_authorized, "restored record granted execution authority");
		}
		require(terminal_found, "terminal state did not remain terminal after restart");
	}
	cleanup_database(path);
}

void test_bounded_queries_fail_closed_for_incomplete_current_state() {
	const auto path = temporary_database_path();
	{
		NotificationDeliveryEvidenceRepository repository(path.string());
		require(repository.append(record("a", "PENDING")), "first bounded record missing");
		require(repository.append(record("b", "PENDING")), "second bounded record missing");
		require(repository.append(record("c", "PENDING")), "third bounded record missing");

		const auto latest = repository.load_latest_per_dedup_key(2);
		require(latest.total == 3 && latest.records.size() == 2 && latest.truncated, "bounded latest query did not report truncation");

		const auto operations = sentum::operations::derive_notification_operations_view(repository, {}, 2);
		require(operations.health == NotificationOperationsHealth::Unavailable, "truncated durable evidence did not fail closed");
		require(!operations.evidence_available, "truncated durable evidence was reported available");

		bool recovery_failed_closed = false;
		try {
			(void)repository.restore_active_delivery_state(2);
		} catch (const std::runtime_error&) {
			recovery_failed_closed = true;
		}
		require(recovery_failed_closed, "bounded restart recovery accepted incomplete state");
	}
	cleanup_database(path);
}

void test_observability_and_incident_semantics_match_durable_truth() {
	const auto path = temporary_database_path();
	{
		NotificationDeliveryEvidenceRepository repository(path.string());
		auto terminal = record("critical", "FAILED", 3, true);
		terminal.failure_code = "TIMEOUT";
		require(repository.append(terminal), "terminal incident evidence missing");

		const std::vector<NotificationDeliveryEvidenceRecord> in_memory{terminal};
		const auto expected = sentum::operations::derive_notification_operations_view(in_memory);
		const auto durable = sentum::operations::derive_notification_operations_view(repository);
		require(durable.status == expected.status, "durable AP21 status drifted from equivalent evidence");
		require(durable.terminal_failed == expected.terminal_failed && durable.backlog == expected.backlog, "durable AP21 counts drifted");

		const auto expected_candidate = sentum::operations::derive_notification_incident_candidate(expected);
		const auto durable_candidate = sentum::operations::derive_notification_incident_candidate(repository);
		require(durable_candidate.state == expected_candidate.state, "durable incident candidate state drifted");
		require(durable_candidate.status == expected_candidate.status, "durable incident candidate status drifted");
		require(durable_candidate.state == NotificationIncidentCandidateState::ProposalReady, "terminal durable failure did not produce governed proposal");
		require(!durable_candidate.incident_authorized && !durable_candidate.execution_authorized, "durable incident projection gained authority");
	}
	cleanup_database(path);
}

void test_persistence_failure_and_unsafe_evidence_fail_closed() {
	const auto corrupt = temporary_database_path("_corrupt");
	{
		std::ofstream file(corrupt);
		file << "not a sqlite database";
	}
	bool corrupt_rejected = false;
	try {
		NotificationDeliveryEvidenceRepository repository(corrupt.string());
	} catch (const std::runtime_error&) {
		corrupt_rejected = true;
	}
	require(corrupt_rejected, "corrupt database did not fail closed");
	cleanup_database(corrupt);

	const auto path = temporary_database_path();
	{
		NotificationDeliveryEvidenceRepository repository(path.string());
		auto unsafe = record("unsafe", "PENDING");
		unsafe.execution_authorized = true;
		bool unsafe_rejected = false;
		try {
			(void)repository.append(unsafe);
		} catch (const std::invalid_argument&) {
			unsafe_rejected = true;
		}
		require(unsafe_rejected, "execution-authorized notification evidence was persisted");
	}
	cleanup_database(path);
}

} // namespace

int main() {
	try {
		test_append_readback_and_exact_field_preservation();
		test_duplicate_suppression_survives_restart();
		test_append_only_transitions_and_generations();
		test_restart_recovery_preserves_terminal_and_retriable_state();
		test_bounded_queries_fail_closed_for_incomplete_current_state();
		test_observability_and_incident_semantics_match_durable_truth();
		test_persistence_failure_and_unsafe_evidence_fail_closed();
		std::cout << "notification delivery evidence repository tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "notification delivery evidence repository test failure: " << error.what() << '\n';
		return 1;
	}
}
