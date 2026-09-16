#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <sentum/ui/OperatorAlertLifecycle.hpp>

namespace sentum::ui {

enum class OperatorAlertAttentionClass {
	Prominent,
	Visible,
	Suppressed
};

struct OperatorAlertAttentionDecision {
	OperatorAlertAttentionClass attention = OperatorAlertAttentionClass::Visible;
	bool suppressed = false;
	bool flapping = false;
	std::string reason;
};

struct OperatorAlertAttentionView {
	std::vector<OperatorAlertAttentionDecision> decisions;
	std::size_t prominent = 0;
	std::size_t visible = 0;
	std::size_t suppressed = 0;
	std::size_t flapping = 0;
	bool storm_limited = false;
	bool execution_authorized = false;
};

inline const char* operator_alert_attention_name(OperatorAlertAttentionClass attention) noexcept {
	switch (attention) {
		case OperatorAlertAttentionClass::Prominent: return "PROMINENT";
		case OperatorAlertAttentionClass::Visible: return "VISIBLE";
		case OperatorAlertAttentionClass::Suppressed: return "SUPPRESSED";
	}
	return "VISIBLE";
}

inline bool operator_alert_is_high_severity(const OperatorAlertLifecycleItem& item) noexcept {
	return item.alert.severity == OperatorAlertSeverity::Critical ||
		item.alert.severity == OperatorAlertSeverity::Warning;
}

inline bool operator_alert_is_flapping(const OperatorAlertLifecycleItem& item) noexcept {
	return item.active && item.generation >= 3;
}

inline OperatorAlertAttentionView derive_operator_alert_attention_view(
	const std::vector<OperatorAlertLifecycleItem>& lifecycle,
	std::size_t low_priority_budget = 4) {
	OperatorAlertAttentionView view;
	view.decisions.reserve(lifecycle.size());
	std::size_t low_priority_visible = 0;

	for (const auto& item : lifecycle) {
		OperatorAlertAttentionDecision decision;
		decision.flapping = operator_alert_is_flapping(item);
		if (decision.flapping) ++view.flapping;

		if (operator_alert_is_high_severity(item)) {
			decision.attention = OperatorAlertAttentionClass::Prominent;
			decision.reason = item.state == OperatorAlertLifecycleState::Acknowledged
				? "high severity remains visible after acknowledgement"
				: "high severity is never suppressed";
			++view.prominent;
			view.decisions.push_back(std::move(decision));
			continue;
		}

		if (decision.flapping) {
			decision.attention = OperatorAlertAttentionClass::Visible;
			decision.reason = "repeated re-activation remains visible";
			++view.visible;
			++low_priority_visible;
			view.decisions.push_back(std::move(decision));
			continue;
		}

		if (item.state == OperatorAlertLifecycleState::Acknowledged) {
			decision.attention = OperatorAlertAttentionClass::Suppressed;
			decision.suppressed = true;
			decision.reason = "acknowledged low-severity alert is deprioritized";
			++view.suppressed;
			view.decisions.push_back(std::move(decision));
			continue;
		}

		if (item.state == OperatorAlertLifecycleState::Cleared || !item.active) {
			decision.attention = OperatorAlertAttentionClass::Suppressed;
			decision.suppressed = true;
			decision.reason = "cleared low-severity alert is retained as evidence only";
			++view.suppressed;
			view.decisions.push_back(std::move(decision));
			continue;
		}

		if (low_priority_visible < low_priority_budget) {
			decision.attention = OperatorAlertAttentionClass::Visible;
			decision.reason = "within low-severity attention budget";
			++view.visible;
			++low_priority_visible;
		} else {
			decision.attention = OperatorAlertAttentionClass::Suppressed;
			decision.suppressed = true;
			decision.reason = "low-severity storm budget exceeded";
			++view.suppressed;
			view.storm_limited = true;
		}
		view.decisions.push_back(std::move(decision));
	}

	view.execution_authorized = false;
	return view;
}

} // namespace sentum::ui
