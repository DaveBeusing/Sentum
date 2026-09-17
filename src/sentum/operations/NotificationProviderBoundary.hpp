#pragma once

#include <string>

#include <sentum/operations/NotificationDeliveryStateMachine.hpp>

namespace sentum::operations {

struct NotificationDispatchRequest {
	std::string dedup_key;
	std::string alert_id;
	std::size_t generation = 1;
	std::string channel;
	std::string audience;
	std::size_t attempt = 0;
	bool delivery_authorized = false;
	bool execution_authorized = false;
};

struct NotificationDispatchResult {
	bool accepted = false;
	bool delivered = false;
	bool retryable = false;
	std::string provider_reference;
	std::string failure_code;
	std::string failure_reason;
	bool execution_authorized = false;
};

inline NotificationDispatchRequest notification_dispatch_request(
	const NotificationDeliveryAttempt& attempt) {
	NotificationDispatchRequest request;
	request.dedup_key = attempt.dedup_key;
	request.alert_id = attempt.alert_id;
	request.generation = attempt.generation;
	request.channel = attempt.channel;
	request.audience = attempt.audience;
	request.attempt = attempt.attempt;
	request.delivery_authorized = attempt.delivery_authorized &&
		attempt.state == NotificationDeliveryState::Dispatched && !attempt.terminal;
	request.execution_authorized = false;
	return request;
}

inline NotificationDeliveryAttempt apply_notification_dispatch_result(
	NotificationDeliveryAttempt attempt,
	const NotificationDispatchResult& result) {
	if (attempt.state != NotificationDeliveryState::Dispatched || attempt.terminal || !attempt.delivery_authorized) {
		attempt.execution_authorized = false;
		return attempt;
	}
	if (result.delivered) {
		return mark_notification_delivered(std::move(attempt), result.provider_reference);
	}
	const auto code = result.failure_code.empty() ? "PROVIDER_FAILURE" : result.failure_code;
	const auto reason = result.failure_reason.empty() ? "notification provider did not confirm delivery" : result.failure_reason;
	return mark_notification_failed(std::move(attempt), code, reason);
}

} // namespace sentum::operations
