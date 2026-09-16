#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/ui/OperatorAlertCenterView.hpp>
#include <sentum/ui/TerminalWorkspacePolicy.hpp>

namespace sentum::ui {

enum class OperatorAlertAgingState {
	Unavailable,
	Fresh,
	Due,
	Overdue,
	Acknowledged,
	Cleared
};

struct OperatorAlertEscalationItem {
	std::string alert_id;
	std::string severity;
	std::string lifecycle_state;
	std::string aging_state;
	std::string first_seen_utc;
	std::string last_seen_utc;
	std::string next_escalation_at_utc;
	std::string observed_at_utc;
	std::string latest_escalation;
	std::string latest_actor;
	std::string latest_reason;
	bool overdue = false;
	bool acknowledgement_required = false;
	bool execution_authorized = false;
};

struct OperatorAlertEscalationTimelineView {
	std::vector<OperatorAlertEscalationItem> items;
	std::size_t total = 0;
	std::size_t due = 0;
	std::size_t overdue = 0;
	std::size_t unavailable = 0;
	bool truncated = false;
	bool execution_authorized = false;
};

inline const char* operator_alert_aging_name(OperatorAlertAgingState state) noexcept {
	switch (state) {
		case OperatorAlertAgingState::Unavailable: return "AGING UNAVAILABLE";
		case OperatorAlertAgingState::Fresh: return "FRESH";
		case OperatorAlertAgingState::Due: return "DUE";
		case OperatorAlertAgingState::Overdue: return "OVERDUE";
		case OperatorAlertAgingState::Acknowledged: return "ACKNOWLEDGED";
		case OperatorAlertAgingState::Cleared: return "CLEARED";
	}
	return "AGING UNAVAILABLE";
}

inline bool canonical_utc_evidence(const std::string& value) noexcept {
	if (value.size() < 20 || value.back() != 'Z') return false;
	return value[4] == '-' && value[7] == '-' && value[10] == 'T' && value[13] == ':' && value[16] == ':';
}

inline OperatorAlertAgingState derive_alert_aging_state(
	const OperatorAlertCenterItem& item,
	const std::string& observed_at_utc,
	const std::string& next_escalation_at_utc) noexcept {
	if (item.state == "CLEARED") return OperatorAlertAgingState::Cleared;
	if (item.state == "ACKNOWLEDGED") return OperatorAlertAgingState::Acknowledged;
	if (!canonical_utc_evidence(observed_at_utc) || !canonical_utc_evidence(next_escalation_at_utc)) {
		return OperatorAlertAgingState::Unavailable;
	}
	if (observed_at_utc > next_escalation_at_utc) return OperatorAlertAgingState::Overdue;
	if (observed_at_utc == next_escalation_at_utc) return OperatorAlertAgingState::Due;
	return OperatorAlertAgingState::Fresh;
}

inline OperatorAlertEscalationTimelineView derive_operator_alert_escalation_timeline(
	const nlohmann::json& snapshot,
	std::size_t limit = 12) {
	OperatorAlertEscalationTimelineView view;
	const auto center = derive_operator_alert_center_view(snapshot, 64);
	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	const auto observed_at_utc = json_text(control_plane, "observed_at_utc");
	const auto evidence = control_plane.value("alert_escalation", nlohmann::json::array());

	for (const auto& item : center.items) {
		OperatorAlertEscalationItem row;
		row.alert_id = item.id;
		row.severity = item.severity;
		row.lifecycle_state = item.state;
		row.observed_at_utc = observed_at_utc;
		row.acknowledgement_required = item.acknowledgement_required;
		row.execution_authorized = false;

		if (evidence.is_array()) {
			for (const auto& entry : evidence) {
				if (!entry.is_object() || json_text(entry, "alert_id") != item.id) continue;
				row.first_seen_utc = json_text(entry, "first_seen_utc");
				row.last_seen_utc = json_text(entry, "last_seen_utc");
				row.next_escalation_at_utc = json_text(entry, "next_escalation_at_utc");
				const auto timeline = entry.value("timeline", nlohmann::json::array());
				if (timeline.is_array() && !timeline.empty() && timeline.back().is_object()) {
					row.latest_escalation = json_text(timeline.back(), "state");
					row.latest_actor = json_text(timeline.back(), "actor");
					row.latest_reason = json_text(timeline.back(), "reason");
				}
				break;
			}
		}

		const auto aging = derive_alert_aging_state(item, observed_at_utc, row.next_escalation_at_utc);
		row.aging_state = operator_alert_aging_name(aging);
		row.overdue = aging == OperatorAlertAgingState::Overdue;
		if (aging == OperatorAlertAgingState::Due) ++view.due;
		if (aging == OperatorAlertAgingState::Overdue) ++view.overdue;
		if (aging == OperatorAlertAgingState::Unavailable) ++view.unavailable;
		++view.total;
		if (view.items.size() < limit) view.items.push_back(std::move(row));
	}

	view.truncated = view.total > view.items.size();
	view.execution_authorized = false;
	return view;
}

inline std::string operator_alert_escalation_item_text(const OperatorAlertEscalationItem& item) {
	std::string text = item.severity + " | " + item.aging_state + " | " + item.alert_id;
	if (!item.next_escalation_at_utc.empty()) text += " | next " + item.next_escalation_at_utc;
	if (!item.latest_escalation.empty()) text += " | last " + item.latest_escalation;
	return text;
}

inline std::string operator_alert_escalation_timeline_text(
	const nlohmann::json& snapshot,
	std::size_t limit = 6) {
	const auto view = derive_operator_alert_escalation_timeline(snapshot, limit);
	std::string text = "ESCALATION TIMELINE total=" + std::to_string(view.total) +
		" due=" + std::to_string(view.due) +
		" overdue=" + std::to_string(view.overdue) +
		" unavailable=" + std::to_string(view.unavailable) + "\n";
	for (const auto& item : view.items) {
		text += "   Escalation ";
		text += operator_alert_escalation_item_text(item);
		text += '\n';
	}
	if (view.truncated) text += "   Additional escalation evidence omitted from terminal view\n";
	return text;
}

} // namespace sentum::ui
