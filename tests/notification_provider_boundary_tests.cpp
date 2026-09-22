#include <sentum/operations/NotificationDeliveryEvidence.hpp>

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

sentum::operations::NotificationRouteIntent eligible_intent() {
	sentum::operations::NotificationRouteIntent intent;
	intent.alert_id = "runtime.health";
	intent.generation = 2;
	intent.severity = "CRITICAL";
	intent.escalation_level = 3;
	intent.channel = "PAGER";
	intent.audience = "OPERATIONS_ON_CALL";
	intent.dedup_key = sentum::operations::notification_dedup_key(
		intent.alert_id, intent.generation, intent.escalation_level, intent.channel, intent.audience);
	intent.status = "ELIGIBLE";
	intent.delivery_authorized = true;
	return intent;
}

void test_provider_boundary_carries_notification_authority_only() {
	using namespace sentum::operations;
	auto attempt = mark_notification_dispatched(begin_notification_delivery(eligible_intent()));
	const auto request = notification_dispatch_request(attempt);
	require(request.delivery_authorized, "dispatch request lost delivery authority");
	require(!request.execution_authorized, "dispatch request granted execution authority");
}

void test_provider_success_maps_to_terminal_delivery() {
	using namespace sentum::operations;
	auto attempt = mark_notification_dispatched(begin_notification_delivery(eligible_intent()));
	NotificationDispatchResult result;
	result.accepted = true;
	result.delivered = true;
	result.provider_reference = "provider-123";
	const auto next = apply_notification_dispatch_result(attempt, result);
	require(next.state == NotificationDeliveryState::Delivered, "provider success did not deliver");
	require(next.terminal, "delivered state not terminal");
	require(next.provider_reference == "provider-123", "provider reference missing");
	require(!next.execution_authorized, "provider success granted execution authority");
}

void test_provider_failure_preserves_retry_semantics() {
	using namespace sentum::operations;
	auto attempt = mark_notification_dispatched(begin_notification_delivery(eligible_intent(), 3));
	NotificationDispatchResult result;
	result.retryable = true;
	result.failure_code = "TIMEOUT";
	result.failure_reason = "provider timeout";
	const auto next = apply_notification_dispatch_result(attempt, result);
	require(next.state == NotificationDeliveryState::Failed, "provider failure state mismatch");
	require(notification_delivery_can_retry(next), "retryable provider failure lost retry path");
	require(next.failure_code == "TIMEOUT", "failure code missing");
}

void test_delivery_evidence_is_append_only_and_deduplicated() {
	using namespace sentum::operations;
	auto attempt = mark_notification_dispatched(begin_notification_delivery(eligible_intent()));
	std::vector<NotificationDeliveryEvidenceRecord> log;
	const auto dispatched = notification_delivery_evidence_record(attempt, "2026-09-17T08:00:00Z");
	require(append_notification_delivery_evidence(log, dispatched), "initial evidence not appended");
	require(!append_notification_delivery_evidence(log, dispatched), "duplicate evidence appended");
	NotificationDispatchResult result;
	result.delivered = true;
	result.provider_reference = "provider-123";
	attempt = apply_notification_dispatch_result(attempt, result);
	const auto delivered = notification_delivery_evidence_record(attempt, "2026-09-17T08:00:02Z");
	require(append_notification_delivery_evidence(log, delivered), "terminal evidence not appended");
	require(log.size() == 2, "append-only evidence count mismatch");
	const auto json = notification_delivery_evidence_json(log.back());
	require(json.at("execution_authorized") == false, "evidence granted execution authority");
}

} // namespace

int main() {
	try {
		test_provider_boundary_carries_notification_authority_only();
		test_provider_success_maps_to_terminal_delivery();
		test_provider_failure_preserves_retry_semantics();
		test_delivery_evidence_is_append_only_and_deduplicated();
		std::cout << "notification provider boundary tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "notification provider boundary test failure: " << error.what() << '\n';
		return 1;
	}
}
