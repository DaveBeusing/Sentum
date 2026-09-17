#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sentum/operations/NotificationDeliveryEvidence.hpp>

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
		auto& channel = channel_map[record->channel];
		channel.channel = record->channel.empty() ? "UNKNOWN" : record->channel;
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

} // namespace sentum::operations
