#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <sentum/operations/NotificationRoutingPolicy.hpp>

namespace sentum::operations {

enum class NotificationDeliveryState {
	Pending,
	Dispatched,
	Delivered,
	Failed
};

struct NotificationDeliveryAttempt {
	std::string dedup_key;
	std::string alert_id;
	std::size_t generation = 1;
	std::string channel;
	std::string audience;
	NotificationDeliveryState state = NotificationDeliveryState::Pending;
	std::size_t attempt = 0;
	std::size_t max_attempts = 3;
	std::size_t retry_backoff_seconds = 0;
	bool terminal = false;
	std::string provider_reference;
	std::string failure_code;
	std::string failure_reason;
	bool delivery_authorized = false;
	bool execution_authorized = false;
};

inline const char* notification_delivery_state_name(NotificationDeliveryState state) noexcept {
	switch (state) {
		case NotificationDeliveryState::Pending: return "PENDING";
		case NotificationDeliveryState::Dispatched: return "DISPATCHED";
		case NotificationDeliveryState::Delivered: return "DELIVERED";
		case NotificationDeliveryState::Failed: return "FAILED";
	}
	return "FAILED";
}

inline std::size_t notification_retry_backoff_seconds(std::size_t attempt) noexcept {
	if (attempt == 0) return 0;
	const auto shift = std::min<std::size_t>(attempt - 1, 5);
	return std::min<std::size_t>(5ULL << shift, 120ULL);
}

inline NotificationDeliveryAttempt begin_notification_delivery(
	const NotificationRouteIntent& intent,
	std::size_t max_attempts = 3) {
	NotificationDeliveryAttempt attempt;
	attempt.dedup_key = intent.dedup_key;
	attempt.alert_id = intent.alert_id;
	attempt.generation = intent.generation;
	attempt.channel = intent.channel;
	attempt.audience = intent.audience;
	attempt.max_attempts = std::max<std::size_t>(1, max_attempts);
	attempt.delivery_authorized = intent.delivery_authorized && intent.status == "ELIGIBLE" && !intent.dedup_key.empty();
	attempt.execution_authorized = false;
	if (!attempt.delivery_authorized) {
		attempt.state = NotificationDeliveryState::Failed;
		attempt.terminal = true;
		attempt.failure_code = "NOT_ELIGIBLE";
		attempt.failure_reason = "routing intent is not eligible for notification delivery";
	}
	return attempt;
}

inline NotificationDeliveryAttempt mark_notification_dispatched(NotificationDeliveryAttempt current) {
	if (!current.delivery_authorized || current.terminal ||
		(current.state != NotificationDeliveryState::Pending && current.state != NotificationDeliveryState::Failed)) {
		current.execution_authorized = false;
		return current;
	}
	if (current.attempt >= current.max_attempts) {
		current.state = NotificationDeliveryState::Failed;
		current.terminal = true;
		current.failure_code = "ATTEMPTS_EXHAUSTED";
		current.failure_reason = "notification retry budget exhausted";
		current.execution_authorized = false;
		return current;
	}
	++current.attempt;
	current.state = NotificationDeliveryState::Dispatched;
	current.retry_backoff_seconds = 0;
	current.failure_code.clear();
	current.failure_reason.clear();
	current.execution_authorized = false;
	return current;
}

inline NotificationDeliveryAttempt mark_notification_delivered(
	NotificationDeliveryAttempt current,
	std::string provider_reference = {}) {
	if (current.state != NotificationDeliveryState::Dispatched || current.terminal) {
		current.execution_authorized = false;
		return current;
	}
	current.state = NotificationDeliveryState::Delivered;
	current.terminal = true;
	current.provider_reference = std::move(provider_reference);
	current.retry_backoff_seconds = 0;
	current.execution_authorized = false;
	return current;
}

inline NotificationDeliveryAttempt mark_notification_failed(
	NotificationDeliveryAttempt current,
	std::string failure_code,
	std::string failure_reason) {
	if (current.state != NotificationDeliveryState::Dispatched || current.terminal) {
		current.execution_authorized = false;
		return current;
	}
	current.state = NotificationDeliveryState::Failed;
	current.failure_code = std::move(failure_code);
	current.failure_reason = std::move(failure_reason);
	current.terminal = current.attempt >= current.max_attempts;
	current.retry_backoff_seconds = current.terminal ? 0 : notification_retry_backoff_seconds(current.attempt);
	current.execution_authorized = false;
	return current;
}

inline bool notification_delivery_can_retry(const NotificationDeliveryAttempt& attempt) noexcept {
	return attempt.delivery_authorized && !attempt.terminal &&
		attempt.state == NotificationDeliveryState::Failed &&
		attempt.attempt < attempt.max_attempts;
}

inline bool notification_delivery_is_duplicate(
	std::string_view dedup_key,
	const std::vector<NotificationDeliveryAttempt>& evidence) noexcept {
	if (dedup_key.empty()) return true;
	for (const auto& attempt : evidence) {
		if (attempt.dedup_key != dedup_key) continue;
		if (attempt.state == NotificationDeliveryState::Pending ||
			attempt.state == NotificationDeliveryState::Dispatched ||
			attempt.state == NotificationDeliveryState::Delivered ||
			attempt.terminal) {
			return true;
		}
	}
	return false;
}

inline std::vector<NotificationDeliveryAttempt> derive_notification_delivery_attempts(
	const NotificationRoutingPlan& plan,
	const std::vector<NotificationDeliveryAttempt>& existing,
	std::size_t max_attempts = 3) {
	std::vector<NotificationDeliveryAttempt> additions;
	for (const auto& intent : plan.intents) {
		if (!intent.delivery_authorized || intent.status != "ELIGIBLE") continue;
		if (notification_delivery_is_duplicate(intent.dedup_key, existing)) continue;
		if (notification_delivery_is_duplicate(intent.dedup_key, additions)) continue;
		additions.push_back(begin_notification_delivery(intent, max_attempts));
	}
	std::stable_sort(additions.begin(), additions.end(), [](const auto& left, const auto& right) {
		return left.dedup_key < right.dedup_key;
	});
	return additions;
}

} // namespace sentum::operations
