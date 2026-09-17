#include <sentum/operations/NotificationOperationsObservability.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using sentum::operations::NotificationDeliveryEvidenceRecord;
using sentum::operations::NotificationOperationsHealth;
using sentum::operations::NotificationOperationsThresholds;

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

NotificationDeliveryEvidenceRecord record(
	std::string key,
	std::string channel,
	std::string state,
	bool terminal = false,
	std::size_t attempt = 1) {
	NotificationDeliveryEvidenceRecord value;
	value.dedup_key = std::move(key);
	value.alert_id = value.dedup_key;
	value.channel = std::move(channel);
	value.audience = "OPERATIONS";
	value.state = std::move(state);
	value.terminal = terminal;
	value.attempt = attempt;
	value.execution_authorized = false;
	return value;
}

void test_empty_evidence_is_no_activity() {
	const auto view = sentum::operations::derive_notification_operations_view({});
	require(view.health == NotificationOperationsHealth::NoActivity, "empty evidence should be no activity");
	require(view.status == "NO_ACTIVITY", "empty evidence status mismatch");
	require(view.current == 0 && view.backlog == 0, "empty evidence counts mismatch");
	require(view.evidence_available, "empty but available evidence was marked unavailable");
	require(!view.incident_authorized && !view.execution_authorized, "observability view gained authority");
}

void test_latest_record_per_dedup_key_drives_current_state() {
	std::vector<NotificationDeliveryEvidenceRecord> evidence;
	evidence.push_back(record("a", "EMAIL", "PENDING", false, 0));
	evidence.push_back(record("a", "EMAIL", "DISPATCHED", false, 1));
	evidence.push_back(record("a", "EMAIL", "DELIVERED", true, 1));
	evidence.push_back(record("b", "PAGER", "FAILED", false, 1));
	const auto view = sentum::operations::derive_notification_operations_view(evidence);
	require(view.current == 2, "append-only history was double counted");
	require(view.delivered == 1, "latest delivered state missing");
	require(view.failed == 1 && view.retriable_failed == 1, "retriable failure count mismatch");
	require(view.pending == 0 && view.dispatched == 0, "historical states leaked into current counts");
	require(view.backlog == 1, "retry backlog mismatch");
	require(view.health == NotificationOperationsHealth::Attention, "retriable failure should require attention");
}

void test_terminal_failure_is_incident_candidate_only() {
	std::vector<NotificationDeliveryEvidenceRecord> evidence;
	evidence.push_back(record("critical", "PAGER", "FAILED", true, 3));
	const auto view = sentum::operations::derive_notification_operations_view(evidence);
	require(view.health == NotificationOperationsHealth::IncidentCandidate, "terminal failure did not produce incident candidate");
	require(view.incident_signal == "NOTIFICATION_DELIVERY_FAILURE", "incident signal mismatch");
	require(view.terminal_failed == 1, "terminal failure count mismatch");
	require(!view.incident_authorized, "observability projection was allowed to create incident state");
	require(!view.execution_authorized, "incident candidate gained execution authority");
}

void test_backlog_threshold_requires_attention() {
	std::vector<NotificationDeliveryEvidenceRecord> evidence;
	for (int index = 0; index < 4; ++index) {
		evidence.push_back(record("pending-" + std::to_string(index), "EMAIL", "PENDING", false, 0));
	}
	NotificationOperationsThresholds thresholds;
	thresholds.backlog_attention = 4;
	thresholds.terminal_failure_incident = 1;
	const auto view = sentum::operations::derive_notification_operations_view(evidence, thresholds);
	require(view.health == NotificationOperationsHealth::Attention, "backlog threshold did not require attention");
	require(view.backlog == 4, "backlog count mismatch");
	require(view.incident_signal == "ATTENTION", "attention signal mismatch");
}

void test_channel_projection_is_deterministic() {
	std::vector<NotificationDeliveryEvidenceRecord> evidence;
	evidence.push_back(record("z", "PAGER", "DELIVERED", true));
	evidence.push_back(record("a", "EMAIL", "PENDING"));
	evidence.push_back(record("b", "IN_APP", "FAILED", false));
	const auto view = sentum::operations::derive_notification_operations_view(evidence);
	require(view.channels.size() == 3, "channel projection count mismatch");
	require(view.channels[0].channel == "EMAIL", "channels are not deterministically ordered");
	require(view.channels[1].channel == "IN_APP", "channels are not deterministically ordered");
	require(view.channels[2].channel == "PAGER", "channels are not deterministically ordered");
}

void test_unavailable_view_fails_closed() {
	const auto view = sentum::operations::unavailable_notification_operations_view();
	require(view.health == NotificationOperationsHealth::Unavailable, "unavailable health mismatch");
	require(view.status == "UNAVAILABLE", "unavailable status mismatch");
	require(!view.evidence_available, "unavailable evidence was marked available");
	require(!view.incident_authorized && !view.execution_authorized, "unavailable view gained authority");
}

} // namespace

int main() {
	try {
		test_empty_evidence_is_no_activity();
		test_latest_record_per_dedup_key_drives_current_state();
		test_terminal_failure_is_incident_candidate_only();
		test_backlog_threshold_requires_attention();
		test_channel_projection_is_deterministic();
		test_unavailable_view_fails_closed();
		std::cout << "notification operations observability tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "notification operations observability test failure: " << error.what() << '\n';
		return 1;
	}
}
