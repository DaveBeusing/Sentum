#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/operations/NotificationDeliveryEvidenceRepository.hpp>

namespace sentum::operations {

enum class NotificationOperationsHealth {
	NoActivity,
	Healthy,
	Attention,
	IncidentCandidate,
	Unavailable
};

struct NotificationOperationsThresholds {
	std::size_t backlog_attention = 8;
	std::size_t terminal_failure_incident = 1;
};

struct NotificationChannelOperations {
	std::string channel;
	std::size_t active = 0;
	std::size_t delivered = 0;
	std::size_t failed = 0;
};

struct NotificationOperationsView {
	NotificationOperationsHealth health = NotificationOperationsHealth::Unavailable;
	std::string status = "UNAVAILABLE";
	std::string incident_signal = "NONE";
	std::size_t current = 0;
	std::size_t pending = 0;
	std::size_t dispatched = 0;
	std::size_t delivered = 0;
	std::size_t failed = 0;
	std::size_t retriable_failed = 0;
	std::size_t terminal_failed = 0;
	std::size_t backlog = 0;
	std::vector<NotificationChannelOperations> channels;
	bool evidence_available = false;
	bool incident_authorized = false;
	bool execution_authorized = false;
};

inline const char* notification_operations_health_name(NotificationOperationsHealth health) noexcept {
	switch (health) {
		case NotificationOperationsHealth::NoActivity: return "NO_ACTIVITY";
		case NotificationOperationsHealth::Healthy: return "HEALTHY";
		case NotificationOperationsHealth::Attention: return "ATTENTION";
		case NotificationOperationsHealth::IncidentCandidate: return "INCIDENT_CANDIDATE";
		case NotificationOperationsHealth::Unavailable: return "UNAVAILABLE";
	}
	return "UNAVAILABLE";
}

inline std::vector<const NotificationDeliveryEvidenceRecord*> latest_notification_delivery_evidence(
	const std::vector<NotificationDeliveryEvidenceRecord>& evidence) {
	std::unordered_map<std::string, std::size_t> index_by_key;
	std::vector<const NotificationDeliveryEvidenceRecord*> latest;
	latest.reserve(evidence.size());
	for (const auto& record : evidence) {
		if (record.dedup_key.empty()) continue;
		const auto found = index_by_key.find(record.dedup_key);
		if (found == index_by_key.end()) {
			index_by_key.emplace(record.dedup_key, latest.size());
			latest.push_back(&record);
		} else {
			latest[found->second] = &record;
		}
	}
	return latest;
}

inline NotificationOperationsView derive_notification_operations_view(
	const std::vector<NotificationDeliveryEvidenceRecord>& evidence,
	NotificationOperationsThresholds thresholds = {}) {
	NotificationOperationsView view;
	view.evidence_available = true;
	const auto latest = latest_notification_delivery_evidence(evidence);
	view.current = latest.size();
	if (latest.empty()) {
		view.health = NotificationOperationsHealth::NoActivity;
		view.status = notification_operations_health_name(view.health);
		return view;
	}

	std::unordered_map<std::string, NotificationChannelOperations> channel_map;
	for (const auto* record : latest) {
		if (record == nullptr) continue;
		const auto channel_name = record->channel.empty() ? std::string("UNKNOWN") : record->channel;
		auto& channel = channel_map[channel_name];
		channel.channel = channel_name;
		if (record->state == "PENDING") {
			++view.pending;
			++channel.active;
		} else if (record->state == "DISPATCHED") {
			++view.dispatched;
			++channel.active;
		} else if (record->state == "DELIVERED") {
			++view.delivered;
			++channel.delivered;
		} else if (record->state == "FAILED") {
			++view.failed;
			++channel.failed;
			if (record->terminal) ++view.terminal_failed;
			else {
				++view.retriable_failed;
				++channel.active;
			}
		}
	}

	view.backlog = view.pending + view.dispatched + view.retriable_failed;
	view.channels.reserve(channel_map.size());
	for (auto& [_, channel] : channel_map) view.channels.push_back(std::move(channel));
	std::sort(view.channels.begin(), view.channels.end(), [](const auto& left, const auto& right) {
		return left.channel < right.channel;
	});

	if (view.terminal_failed >= thresholds.terminal_failure_incident && thresholds.terminal_failure_incident > 0) {
		view.health = NotificationOperationsHealth::IncidentCandidate;
		view.incident_signal = "NOTIFICATION_DELIVERY_FAILURE";
	} else if (view.retriable_failed > 0 ||
		(thresholds.backlog_attention > 0 && view.backlog >= thresholds.backlog_attention)) {
		view.health = NotificationOperationsHealth::Attention;
		view.incident_signal = "ATTENTION";
	} else {
		view.health = NotificationOperationsHealth::Healthy;
	}
	view.status = notification_operations_health_name(view.health);
	view.incident_authorized = false;
	view.execution_authorized = false;
	return view;
}

inline NotificationOperationsView unavailable_notification_operations_view() {
	NotificationOperationsView view;
	view.health = NotificationOperationsHealth::Unavailable;
	view.status = notification_operations_health_name(view.health);
	view.evidence_available = false;
	view.incident_authorized = false;
	view.execution_authorized = false;
	return view;
}

inline NotificationOperationsView derive_notification_operations_view(
	const NotificationDeliveryEvidenceRepository& repository,
	NotificationOperationsThresholds thresholds = {},
	std::size_t evidence_limit = 1024) {
	try {
		const auto batch = repository.load_latest_per_dedup_key(evidence_limit);
		if (batch.truncated) return unavailable_notification_operations_view();
		std::vector<NotificationDeliveryEvidenceRecord> evidence;
		evidence.reserve(batch.records.size());
		for (const auto& persisted : batch.records) evidence.push_back(persisted.record);
		return derive_notification_operations_view(evidence, thresholds);
	} catch (...) {
		return unavailable_notification_operations_view();
	}
}

inline nlohmann::json notification_delivery_snapshot_from_repository(
	const nlohmann::json& snapshot,
	const NotificationDeliveryEvidenceRepository& repository,
	std::size_t evidence_limit = 1024) {
	auto durable_snapshot = snapshot;
	if (!durable_snapshot.contains("operations_control_plane") ||
		!durable_snapshot.at("operations_control_plane").is_object()) {
		return durable_snapshot;
	}

	auto& control_plane = durable_snapshot["operations_control_plane"];
	control_plane.erase("notification_delivery_evidence");
	try {
		const auto batch = repository.load_latest_per_dedup_key(evidence_limit);
		if (batch.truncated) return durable_snapshot;

		nlohmann::json evidence = nlohmann::json::array();
		for (const auto& persisted : batch.records) {
			evidence.push_back(notification_delivery_evidence_json(persisted.record));
		}
		control_plane["notification_delivery_evidence"] = std::move(evidence);
	} catch (...) {
		// Missing or unreadable durable evidence remains absent so the existing
		// observability projection reports UNAVAILABLE and fails closed.
	}
	return durable_snapshot;
}

inline std::vector<NotificationDeliveryEvidenceRecord> notification_delivery_evidence_from_snapshot(
	const nlohmann::json& snapshot) {
	std::vector<NotificationDeliveryEvidenceRecord> evidence;
	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	if (!control_plane.contains("notification_delivery_evidence") ||
		!control_plane.at("notification_delivery_evidence").is_array()) return evidence;
	for (const auto& item : control_plane.at("notification_delivery_evidence")) {
		if (!item.is_object()) continue;
		NotificationDeliveryEvidenceRecord record;
		record.dedup_key = item.value("dedup_key", std::string{});
		record.alert_id = item.value("alert_id", std::string{});
		record.generation = item.value("generation", static_cast<std::size_t>(1));
		record.channel = item.value("channel", std::string{});
		record.audience = item.value("audience", std::string{});
		record.state = item.value("state", std::string{});
		record.attempt = item.value("attempt", static_cast<std::size_t>(0));
		record.max_attempts = item.value("max_attempts", static_cast<std::size_t>(3));
		record.retry_backoff_seconds = item.value("retry_backoff_seconds", static_cast<std::size_t>(0));
		record.terminal = item.value("terminal", false);
		record.provider_reference = item.value("provider_reference", std::string{});
		record.failure_code = item.value("failure_code", std::string{});
		record.failure_reason = item.value("failure_reason", std::string{});
		record.observed_at_utc = item.value("observed_at_utc", std::string{});
		record.delivery_authorized = item.value("delivery_authorized", false);
		record.execution_authorized = false;
		if (!record.dedup_key.empty() && !record.state.empty()) evidence.push_back(std::move(record));
	}
	return evidence;
}

inline NotificationOperationsView derive_notification_operations_view_from_snapshot(
	const nlohmann::json& snapshot,
	NotificationOperationsThresholds thresholds = {}) {
	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	if (!control_plane.contains("notification_delivery_evidence") ||
		!control_plane.at("notification_delivery_evidence").is_array()) {
		return unavailable_notification_operations_view();
	}
	return derive_notification_operations_view(notification_delivery_evidence_from_snapshot(snapshot), thresholds);
}

inline nlohmann::json notification_operations_json(const NotificationOperationsView& view) {
	nlohmann::json channels = nlohmann::json::array();
	for (const auto& channel : view.channels) {
		channels.push_back({
			{"channel", channel.channel},
			{"active", channel.active},
			{"delivered", channel.delivered},
			{"failed", channel.failed}
		});
	}
	return {
		{"status", view.status},
		{"incident_signal", view.incident_signal},
		{"current", view.current},
		{"pending", view.pending},
		{"dispatched", view.dispatched},
		{"delivered", view.delivered},
		{"failed", view.failed},
		{"retriable_failed", view.retriable_failed},
		{"terminal_failed", view.terminal_failed},
		{"backlog", view.backlog},
		{"evidence_available", view.evidence_available},
		{"incident_authorized", false},
		{"execution_authorized", false},
		{"channels", std::move(channels)}
	};
}

} // namespace sentum::operations
