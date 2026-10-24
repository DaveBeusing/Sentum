#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/ui/OperatorAlertPolicy.hpp>

namespace sentum::ui {

inline constexpr std::size_t kMaximumOperatorAlertGeneration = static_cast<std::size_t>(
	std::min<std::uint64_t>(std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::int64_t>::max()));

enum class OperatorAlertLifecycleState {
	New,
	Active,
	Acknowledged,
	Cleared
};

struct OperatorAlertLifecycleItem {
	OperatorAlert alert;
	OperatorAlertLifecycleState state = OperatorAlertLifecycleState::New;
	std::size_t generation = 1;
	std::size_t observations = 1;
	std::string acknowledged_by;
	std::string acknowledgement_reason;
	bool active = true;
	bool execution_authorized = false;
};

inline const char* operator_alert_lifecycle_name(OperatorAlertLifecycleState state) noexcept {
	switch (state) {
		case OperatorAlertLifecycleState::New: return "NEW";
		case OperatorAlertLifecycleState::Active: return "ACTIVE";
		case OperatorAlertLifecycleState::Acknowledged: return "ACKNOWLEDGED";
		case OperatorAlertLifecycleState::Cleared: return "CLEARED";
	}
	return "CLEARED";
}

struct OperatorAlertAcknowledgementEvidence {
	std::string alert_id;
	std::size_t generation = 0;
	std::string actor;
	std::string reason;
};

inline nlohmann::json operator_alert_lifecycle_json(const OperatorAlertLifecycleItem& item) {
	return {{"id", item.alert.id}, {"source", item.alert.source},
		{"severity", operator_alert_severity_name(item.alert.severity)}, {"title", item.alert.title},
		{"message", item.alert.message}, {"guidance", item.alert.guidance},
		{"recommended_workspace", item.alert.recommended_workspace},
		{"acknowledgement_required", item.alert.acknowledgement_required},
		{"notification_candidate", item.alert.notification_candidate}, {"escalation_level", item.alert.escalation_level},
		{"state", operator_alert_lifecycle_name(item.state)}, {"generation", item.generation},
		{"observations", item.observations}, {"acknowledged_by", item.acknowledged_by},
		{"acknowledgement_reason", item.acknowledgement_reason}, {"active", item.active},
		{"execution_authorized", false}};
}

inline nlohmann::json operator_alert_lifecycle_json(const std::vector<OperatorAlertLifecycleItem>& lifecycle) {
	auto result = nlohmann::json::array();
	for (const auto& item : lifecycle) result.push_back(operator_alert_lifecycle_json(item));
	return result;
}

inline std::vector<OperatorAlertAcknowledgementEvidence> operator_alert_acknowledgements(
	const nlohmann::json& snapshot) {
	std::vector<OperatorAlertAcknowledgementEvidence> acknowledgements;
	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	const auto evidence = control_plane.value("alert_acknowledgements", nlohmann::json::array());
	if (!evidence.is_array()) return acknowledgements;

	for (const auto& item : evidence) {
		if (!item.is_object()) continue;
		const auto alert_id = json_text(item, "alert_id");
		if (alert_id.empty()) continue;
		std::size_t generation = 0;
		try {
			const auto& value = item.at("generation");
			if (value.is_number_integer() && value.get<std::int64_t>() > 0 &&
				value.get<std::uint64_t>() <= kMaximumOperatorAlertGeneration) generation = value.get<std::size_t>();
		} catch (...) { generation = 0; }
		if (generation == 0) continue;
		acknowledgements.push_back({
			alert_id,
			generation,
			json_text(item, "actor"),
			json_text(item, "reason")
		});
	}
	return acknowledgements;
}

inline const OperatorAlertAcknowledgementEvidence* find_alert_acknowledgement(
	const std::vector<OperatorAlertAcknowledgementEvidence>& acknowledgements,
	std::string_view alert_id,
	std::size_t generation) noexcept {
	for (const auto& acknowledgement : acknowledgements) {
		if (acknowledgement.alert_id == alert_id && acknowledgement.generation == generation) {
			return &acknowledgement;
		}
	}
	return nullptr;
}

inline std::vector<OperatorAlertLifecycleItem> reconcile_operator_alert_lifecycle(
	const std::vector<OperatorAlertLifecycleItem>& previous,
	const std::vector<OperatorAlert>& current,
	const std::vector<OperatorAlertAcknowledgementEvidence>& acknowledgements = {}) {
	std::unordered_map<std::string, const OperatorAlertLifecycleItem*> previous_by_id;
	for (const auto& item : previous) {
		if (item.generation == 0 || item.generation > kMaximumOperatorAlertGeneration)
			throw std::invalid_argument("Invalid operator alert generation");
		previous_by_id[item.alert.id] = &item;
	}

	std::vector<OperatorAlertLifecycleItem> next;
	next.reserve(current.size() + previous.size());

	for (const auto& alert : current) {
		OperatorAlertLifecycleItem item;
		item.alert = alert;
		item.execution_authorized = false;
		const auto previous_it = previous_by_id.find(alert.id);
		if (previous_it == previous_by_id.end()) {
			item.state = OperatorAlertLifecycleState::New;
			item.generation = 1;
			item.observations = 1;
		} else {
			const auto& prior = *previous_it->second;
			if (prior.state == OperatorAlertLifecycleState::Cleared || !prior.active) {
				if (prior.generation == kMaximumOperatorAlertGeneration)
					throw std::overflow_error("Operator alert generation exhausted");
				item.state = OperatorAlertLifecycleState::New;
				item.generation = prior.generation + 1;
				item.observations = 1;
			} else {
				item.generation = prior.generation;
				item.observations = prior.observations < kMaximumOperatorAlertGeneration ? prior.observations + 1 : prior.observations;
				item.state = prior.state == OperatorAlertLifecycleState::Acknowledged
					? OperatorAlertLifecycleState::Acknowledged
					: OperatorAlertLifecycleState::Active;
				item.acknowledged_by = prior.acknowledged_by;
				item.acknowledgement_reason = prior.acknowledgement_reason;
			}
		}

		if (alert.acknowledgement_required) {
			if (const auto* acknowledgement = find_alert_acknowledgement(acknowledgements, alert.id, item.generation)) {
				item.state = OperatorAlertLifecycleState::Acknowledged;
				item.acknowledged_by = acknowledgement->actor;
				item.acknowledgement_reason = acknowledgement->reason;
			}
		}
		item.active = true;
		next.push_back(std::move(item));
	}

	for (const auto& prior : previous) {
		const auto still_active = std::find_if(current.begin(), current.end(), [&](const OperatorAlert& alert) {
			return alert.id == prior.alert.id;
		}) != current.end();
		if (still_active) continue;
		auto cleared = prior;
		cleared.state = OperatorAlertLifecycleState::Cleared;
		cleared.active = false;
		cleared.execution_authorized = false;
		next.push_back(std::move(cleared));
	}

	std::stable_sort(next.begin(), next.end(), [](const OperatorAlertLifecycleItem& left, const OperatorAlertLifecycleItem& right) {
		if (left.active != right.active) return left.active > right.active;
		if (left.alert.severity != right.alert.severity) {
			return static_cast<int>(left.alert.severity) > static_cast<int>(right.alert.severity);
		}
		return left.alert.id < right.alert.id;
	});
	return next;
}

inline std::vector<OperatorAlertLifecycleItem> derive_operator_alert_lifecycle(
	const nlohmann::json& snapshot,
	const std::vector<OperatorAlertLifecycleItem>& previous = {}) {
	return reconcile_operator_alert_lifecycle(
		previous,
		derive_operator_alerts(snapshot),
		operator_alert_acknowledgements(snapshot));
}

} // namespace sentum::ui
