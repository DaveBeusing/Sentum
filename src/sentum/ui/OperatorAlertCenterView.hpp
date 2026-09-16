#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/ui/OperatorAlertAttentionPolicy.hpp>
#include <sentum/ui/OperatorAlertLifecycle.hpp>

namespace sentum::ui {

struct OperatorAlertCenterItem {
	std::string id;
	std::string source;
	std::string severity;
	std::string state;
	std::size_t generation = 1;
	std::string title;
	std::string message;
	std::string guidance;
	std::string recommended_workspace;
	std::string acknowledged_by;
	std::string acknowledgement_reason;
	int escalation_level = 0;
	bool acknowledgement_required = false;
	bool notification_candidate = false;
	bool active = true;
	std::string attention = "VISIBLE";
	bool suppressed = false;
	bool flapping = false;
	std::string attention_reason;
	bool execution_authorized = false;
};

struct OperatorAlertCenterView {
	std::vector<OperatorAlertCenterItem> items;
	std::size_t total = 0;
	std::size_t active = 0;
	std::size_t acknowledged = 0;
	std::size_t cleared = 0;
	std::size_t critical = 0;
	std::size_t warning = 0;
	std::size_t attention = 0;
	std::size_t suppressed = 0;
	std::size_t flapping = 0;
	bool storm_limited = false;
	bool truncated = false;
	bool execution_authorized = false;
};

inline OperatorAlertLifecycleState operator_alert_lifecycle_state_from_text(
	const std::string& value,
	OperatorAlertLifecycleState fallback = OperatorAlertLifecycleState::Active) noexcept {
	if (value == "NEW") return OperatorAlertLifecycleState::New;
	if (value == "ACTIVE") return OperatorAlertLifecycleState::Active;
	if (value == "ACKNOWLEDGED") return OperatorAlertLifecycleState::Acknowledged;
	if (value == "CLEARED") return OperatorAlertLifecycleState::Cleared;
	return fallback;
}

inline OperatorAlertSeverity operator_alert_severity_from_text(
	const std::string& value,
	OperatorAlertSeverity fallback = OperatorAlertSeverity::Info) noexcept {
	if (value == "CRITICAL") return OperatorAlertSeverity::Critical;
	if (value == "WARNING") return OperatorAlertSeverity::Warning;
	if (value == "ATTENTION") return OperatorAlertSeverity::Attention;
	if (value == "INFO") return OperatorAlertSeverity::Info;
	return fallback;
}

inline std::vector<OperatorAlertLifecycleItem> operator_alert_lifecycle_evidence(
	const nlohmann::json& snapshot) {
	std::vector<OperatorAlertLifecycleItem> lifecycle;
	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	const auto evidence = control_plane.value("alert_lifecycle", nlohmann::json::array());
	if (!evidence.is_array()) return lifecycle;

	for (const auto& value : evidence) {
		if (!value.is_object()) continue;
		const auto id = json_text(value, "id");
		if (id.empty()) continue;

		OperatorAlertLifecycleItem item;
		item.alert.id = id;
		item.alert.source = json_text(value, "source", "UNKNOWN");
		item.alert.severity = operator_alert_severity_from_text(json_text(value, "severity", "INFO"));
		item.alert.title = json_text(value, "title");
		item.alert.message = json_text(value, "message");
		item.alert.guidance = json_text(value, "guidance");
		item.alert.recommended_workspace = json_text(value, "recommended_workspace", "SYSTEM");
		item.alert.acknowledgement_required = json_bool(value, "acknowledgement_required");
		item.alert.notification_candidate = json_bool(value, "notification_candidate");
		item.alert.execution_authorized = false;
		try { item.alert.escalation_level = value.value("escalation_level", 0); } catch (...) { item.alert.escalation_level = 0; }
		item.state = operator_alert_lifecycle_state_from_text(json_text(value, "state", "ACTIVE"));
		try { item.generation = value.value("generation", static_cast<std::size_t>(1)); } catch (...) { item.generation = 1; }
		if (item.generation == 0) item.generation = 1;
		item.acknowledged_by = json_text(value, "acknowledged_by");
		item.acknowledgement_reason = json_text(value, "acknowledgement_reason");
		item.active = item.state != OperatorAlertLifecycleState::Cleared;
		item.execution_authorized = false;
		lifecycle.push_back(std::move(item));
	}
	return lifecycle;
}

inline std::vector<OperatorAlertLifecycleItem> current_operator_alert_lifecycle_view(
	const nlohmann::json& snapshot) {
	const auto persisted = operator_alert_lifecycle_evidence(snapshot);
	if (!persisted.empty()) return persisted;

	const auto current = derive_operator_alerts(snapshot);
	const auto acknowledgements = operator_alert_acknowledgements(snapshot);
	std::vector<OperatorAlertLifecycleItem> lifecycle;
	lifecycle.reserve(current.size());
	for (const auto& alert : current) {
		OperatorAlertLifecycleItem item;
		item.alert = alert;
		item.state = OperatorAlertLifecycleState::Active;
		item.generation = 1;
		item.active = true;
		if (alert.acknowledgement_required) {
			if (const auto* acknowledgement = find_alert_acknowledgement(acknowledgements, alert.id, 1)) {
				item.state = OperatorAlertLifecycleState::Acknowledged;
				item.acknowledged_by = acknowledgement->actor;
				item.acknowledgement_reason = acknowledgement->reason;
			}
		}
		item.execution_authorized = false;
		lifecycle.push_back(std::move(item));
	}
	return lifecycle;
}

inline OperatorAlertCenterView derive_operator_alert_center_view(
	const nlohmann::json& snapshot,
	std::size_t limit = 12) {
	OperatorAlertCenterView view;
	auto lifecycle = current_operator_alert_lifecycle_view(snapshot);
	std::stable_sort(lifecycle.begin(), lifecycle.end(), [](const auto& left, const auto& right) {
		if (left.active != right.active) return left.active > right.active;
		if (left.alert.severity != right.alert.severity) {
			return static_cast<int>(left.alert.severity) > static_cast<int>(right.alert.severity);
		}
		return left.alert.id < right.alert.id;
	});

	view.total = lifecycle.size();
	for (const auto& item : lifecycle) {
		if (item.active) ++view.active;
		if (item.state == OperatorAlertLifecycleState::Acknowledged) ++view.acknowledged;
		if (item.state == OperatorAlertLifecycleState::Cleared) ++view.cleared;
		if (item.alert.severity == OperatorAlertSeverity::Critical) ++view.critical;
		else if (item.alert.severity == OperatorAlertSeverity::Warning) ++view.warning;
		else if (item.alert.severity == OperatorAlertSeverity::Attention) ++view.attention;
	}

	const auto attention_view = derive_operator_alert_attention_view(lifecycle, limit);
	view.suppressed = attention_view.suppressed;
	view.flapping = attention_view.flapping;
	view.storm_limited = attention_view.storm_limited;
	view.items.reserve(lifecycle.size() - std::min(lifecycle.size(), view.suppressed));
	for (std::size_t index = 0; index < lifecycle.size(); ++index) {
		const auto& item = lifecycle[index];
		const auto& decision = attention_view.decisions[index];
		if (decision.suppressed) continue;
		view.items.push_back({
			item.alert.id,
			item.alert.source,
			operator_alert_severity_name(item.alert.severity),
			operator_alert_lifecycle_name(item.state),
			item.generation,
			item.alert.title,
			item.alert.message,
			item.alert.guidance,
			item.alert.recommended_workspace,
			item.acknowledged_by,
			item.acknowledgement_reason,
			item.alert.escalation_level,
			item.alert.acknowledgement_required,
			item.alert.notification_candidate,
			item.active,
			operator_alert_attention_name(decision.attention),
			decision.suppressed,
			decision.flapping,
			decision.reason,
			false
		});
	}
	view.truncated = view.suppressed > 0;
	view.execution_authorized = false;
	return view;
}

inline std::string operator_alert_center_item_text(const OperatorAlertCenterItem& item) {
	std::string text = item.severity + " | " + item.state + " | " + item.attention + " | " + item.id + " | " + item.title;
	if (item.flapping) text += " | FLAPPING";
	if (!item.acknowledged_by.empty()) text += " | ack " + item.acknowledged_by;
	return text;
}

} // namespace sentum::ui
