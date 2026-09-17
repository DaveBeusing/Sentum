#include <sentum/operations/NotificationDeliveryStateMachine.hpp>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

sentum::operations::NotificationRouteIntent eligible_intent(std::string key = "alert:g1:l3:PAGER:OPS") {
	sentum::operations::NotificationRouteIntent intent;
	intent.alert_id = "alert";
	intent.generation = 1;
	intent.severity = "CRITICAL";
	intent.escalation_level = 3;
	intent.channel = "PAGER";
	intent.audience = "OPS";
	intent.dedup_key = std::move(key);
	intent.status = "ELIGIBLE";
	intent.delivery_authorized = true;
	intent.execution_authorized = false;
	return intent;
}

void test_happy_path_delivery_is_terminal() {
	using namespace sentum::operations;
	auto attempt = begin_notification_delivery(eligible_intent(), 3);
	require(attempt.state == NotificationDeliveryState::Pending, "eligible intent did not start pending");
	attempt = mark_notification_dispatched(std::move(attempt));
	require(attempt.state == NotificationDeliveryState::Dispatched && attempt.attempt == 1, "dispatch transition failed");
	attempt = mark_notification_delivered(std::move(attempt), "provider-123");
	require(attempt.state == NotificationDeliveryState::Delivered && attempt.terminal, "delivered state not terminal");
	require(attempt.provider_reference == "provider-123", "provider reference missing");
	require(!attempt.execution_authorized, "notification delivery granted execution authority");
}

void test_failed_delivery_has_bounded_retry() {
	using namespace sentum::operations;
	auto attempt = begin_notification_delivery(eligible_intent(), 2);
	attempt = mark_notification_dispatched(std::move(attempt));
	attempt = mark_notification_failed(std::move(attempt), "TEMP", "temporary failure");
	require(notification_delivery_can_retry(attempt), "retryable failure was not retryable");
	require(attempt.retry_backoff_seconds == 5, "first retry backoff mismatch");
	attempt = mark_notification_dispatched(std::move(attempt));
	attempt = mark_notification_failed(std::move(attempt), "TEMP", "temporary failure");
	require(attempt.terminal, "retry budget exhaustion was not terminal");
	require(!notification_delivery_can_retry(attempt), "terminal failure still retryable");
	require(attempt.retry_backoff_seconds == 0, "terminal failure retained retry delay");
}

void test_backoff_is_bounded() {
	using namespace sentum::operations;
	require(notification_retry_backoff_seconds(1) == 5, "attempt 1 backoff mismatch");
	require(notification_retry_backoff_seconds(2) == 10, "attempt 2 backoff mismatch");
	require(notification_retry_backoff_seconds(6) <= 120, "backoff exceeded bound");
	require(notification_retry_backoff_seconds(100) == 120, "backoff cap mismatch");
}

void test_ineligible_intent_fails_closed() {
	using namespace sentum::operations;
	auto intent = eligible_intent();
	intent.status = "BLOCKED";
	intent.delivery_authorized = false;
	const auto attempt = begin_notification_delivery(intent);
	require(attempt.state == NotificationDeliveryState::Failed && attempt.terminal, "blocked intent did not fail closed");
	require(attempt.failure_code == "NOT_ELIGIBLE", "blocked intent failure code mismatch");
}

void test_generation_safe_dedup_prevents_duplicate_attempt() {
	using namespace sentum::operations;
	NotificationRoutingPlan plan;
	plan.intents.push_back(eligible_intent("alert:g1:l3:PAGER:OPS"));
	plan.intents.push_back(eligible_intent("alert:g1:l3:PAGER:OPS"));
	const auto additions = derive_notification_delivery_attempts(plan, {});
	require(additions.size() == 1, "duplicate routing intent created duplicate delivery attempt");

	auto delivered = additions.front();
	delivered = mark_notification_dispatched(std::move(delivered));
	delivered = mark_notification_delivered(std::move(delivered));
	const auto duplicate = derive_notification_delivery_attempts(plan, {delivered});
	require(duplicate.empty(), "delivered dedup key was scheduled again");

	NotificationRoutingPlan next_generation;
	next_generation.intents.push_back(eligible_intent("alert:g2:l3:PAGER:OPS"));
	const auto next = derive_notification_delivery_attempts(next_generation, {delivered});
	require(next.size() == 1, "new generation was incorrectly deduplicated");
}

void test_pending_and_dispatched_evidence_are_idempotent() {
	using namespace sentum::operations;
	NotificationRoutingPlan plan;
	plan.intents.push_back(eligible_intent());
	auto pending = begin_notification_delivery(plan.intents.front());
	require(derive_notification_delivery_attempts(plan, {pending}).empty(), "pending evidence did not deduplicate");
	auto dispatched = mark_notification_dispatched(std::move(pending));
	require(derive_notification_delivery_attempts(plan, {dispatched}).empty(), "dispatched evidence did not deduplicate");
}

} // namespace

int main() {
	try {
		test_happy_path_delivery_is_terminal();
		test_failed_delivery_has_bounded_retry();
		test_backoff_is_bounded();
		test_ineligible_intent_fails_closed();
		test_generation_safe_dedup_prevents_duplicate_attempt();
		test_pending_and_dispatched_evidence_are_idempotent();
		std::cout << "notification delivery state machine tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "notification delivery state machine test failure: " << error.what() << '\n';
		return 1;
	}
}
