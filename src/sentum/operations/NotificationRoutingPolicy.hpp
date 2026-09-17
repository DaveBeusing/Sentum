#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/ui/OperatorAlertCenterView.hpp>

namespace sentum::operations {

struct NotificationRouteIntent {
	std::string alert_id;
	std::size_t generation = 1;
	std::string severity;
	int escalation_level = 0;
	std::string channel;
	std::string audience;
	std::string dedup_key;
	std::string status = "BLOCKED";
	std::string reason;
	bool delivery_authorized = false;
	bool execution_authorized = false;
};

struct NotificationRoutingPlan {
	std::vector<NotificationRouteIntent> intents;
	std::size_t eligible = 0;
	std::size_t blocked = 0;
	bool policy_valid = false;
	std::string policy_status = "BLOCKED";
	bool execution_authorized = false;
};

inline bool notification_policy_valid(const nlohmann::json& policy) noexcept {
	try {
		if (!policy.is_object()) return false;
		if (policy.value("schema_version", 0) != 1) return false;
		if (policy.value("authority", std::string{}) != "NOTIFICATION_ROUTING_ONLY") return false;
		if (policy.value("default_action", std::string{}) != "BLOCK") return false;
		return policy.contains("allowed_channels") && policy.at("allowed_channels").is_array() &&
			policy.contains("routes") && policy.at("routes").is_array();
	} catch (...) {
		return false;
	}
}

inline std::unordered_set<std::string> notification_allowed_channels(const nlohmann::json& policy) {
	std::unordered_set<std::string> channels;
	if (!notification_policy_valid(policy)) return channels;
	for (const auto& value : policy.at("allowed_channels")) {
		if (value.is_string()) channels.insert(value.get<std::string>());
	}
	return channels;
}

inline std::string notification_dedup_key(
	std::string_view alert_id,
	std::size_t generation,
	int escalation_level,
	std::string_view channel,
	std::string_view audience) {
	return std::string(alert_id) + ":g" + std::to_string(generation) + ":l" +
		std::to_string(escalation_level) + ":" + std::string(channel) + ":" + std::string(audience);
}

inline const nlohmann::json* notification_route_for(
	const nlohmann::json& policy,
	const std::string& severity,
	int escalation_level) noexcept {
	if (!notification_policy_valid(policy)) return nullptr;
	for (const auto& route : policy.at("routes")) {
		if (!route.is_object()) continue;
		if (route.value("severity", std::string{}) != severity) continue;
		if (escalation_level < route.value("minimum_escalation_level", 0)) continue;
		return &route;
	}
	return nullptr;
}

inline NotificationRoutingPlan derive_notification_routing_plan(
	const nlohmann::json& snapshot,
	const nlohmann::json& policy) {
	NotificationRoutingPlan plan;
	plan.policy_valid = notification_policy_valid(policy);
	plan.policy_status = plan.policy_valid ? "CONTROLLED" : "BLOCKED";
	if (!plan.policy_valid) return plan;

	const auto allowed_channels = notification_allowed_channels(policy);
	const auto lifecycle = sentum::ui::current_operator_alert_lifecycle_view(snapshot);
	for (const auto& item : lifecycle) {
		if (!item.active || item.state == sentum::ui::OperatorAlertLifecycleState::Cleared) continue;
		if (item.state == sentum::ui::OperatorAlertLifecycleState::Acknowledged) continue;
		if (!item.alert.notification_candidate) continue;

		const auto severity = std::string(sentum::ui::operator_alert_severity_name(item.alert.severity));
		const auto* route = notification_route_for(policy, severity, item.alert.escalation_level);
		if (route == nullptr) {
			NotificationRouteIntent blocked;
			blocked.alert_id = item.alert.id;
			blocked.generation = item.generation;
			blocked.severity = severity;
			blocked.escalation_level = item.alert.escalation_level;
			blocked.reason = "no governed route for alert severity/escalation";
			plan.intents.push_back(std::move(blocked));
			++plan.blocked;
			continue;
		}

		const auto channels = route->value("channels", nlohmann::json::array());
		const auto audiences = route->value("audiences", nlohmann::json::array());
		if (!channels.is_array() || !audiences.is_array() || channels.empty() || audiences.empty()) {
			NotificationRouteIntent blocked;
			blocked.alert_id = item.alert.id;
			blocked.generation = item.generation;
			blocked.severity = severity;
			blocked.escalation_level = item.alert.escalation_level;
			blocked.reason = "governed route is incomplete";
			plan.intents.push_back(std::move(blocked));
			++plan.blocked;
			continue;
		}

		for (const auto& channel_value : channels) {
			if (!channel_value.is_string()) continue;
			const auto channel = channel_value.get<std::string>();
			for (const auto& audience_value : audiences) {
				if (!audience_value.is_string()) continue;
				const auto audience = audience_value.get<std::string>();
				NotificationRouteIntent intent;
				intent.alert_id = item.alert.id;
				intent.generation = item.generation;
				intent.severity = severity;
				intent.escalation_level = item.alert.escalation_level;
				intent.channel = channel;
				intent.audience = audience;
				intent.dedup_key = notification_dedup_key(
					intent.alert_id,
					intent.generation,
					intent.escalation_level,
					intent.channel,
					intent.audience);
				if (allowed_channels.find(channel) == allowed_channels.end() || audience.empty()) {
					intent.status = "BLOCKED";
					intent.reason = "channel or audience is not governed";
					++plan.blocked;
				} else {
					intent.status = "ELIGIBLE";
					intent.reason = "governed notification intent";
					intent.delivery_authorized = true;
					++plan.eligible;
				}
				intent.execution_authorized = false;
				plan.intents.push_back(std::move(intent));
			}
		}
	}

	std::stable_sort(plan.intents.begin(), plan.intents.end(), [](const auto& left, const auto& right) {
		if (left.status != right.status) return left.status < right.status;
		if (left.alert_id != right.alert_id) return left.alert_id < right.alert_id;
		return left.dedup_key < right.dedup_key;
	});
	plan.execution_authorized = false;
	return plan;
}

} // namespace sentum::operations
