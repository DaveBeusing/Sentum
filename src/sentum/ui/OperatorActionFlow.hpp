#pragma once

#include <string>
#include <string_view>

#include <sentum/ui/TerminalWorkspacePolicy.hpp>

namespace sentum::ui {

enum class OperatorActionFlowState {
	Idle,
	ConfirmationRequired,
	ApprovalRequested,
	Delegated,
	Blocked,
	Cancelled
};

struct OperatorActionFlow {
	std::string action;
	OperatorActionPresentation presentation;
	OperatorActionFlowState state = OperatorActionFlowState::Idle;
	std::string headline;
	std::string detail;
	bool confirm_available = false;
	bool cancel_available = false;
	bool execution_authorized = false;
};

inline OperatorActionFlow begin_operator_action_flow(
	std::string_view action,
	std::string_view classification) {
	OperatorActionFlow flow;
	flow.action = std::string(action);
	flow.presentation = operator_action_presentation(classification);

	if (flow.presentation.blocked) {
		flow.state = OperatorActionFlowState::Blocked;
		flow.headline = "BLOCKED";
		flow.detail = flow.presentation.guidance;
		flow.cancel_available = true;
		return flow;
	}

	if (flow.presentation.classification == "APPROVAL_REQUIRED") {
		flow.state = OperatorActionFlowState::ConfirmationRequired;
		flow.headline = "CONFIRM APPROVAL REQUEST";
		flow.detail = flow.presentation.guidance;
		flow.confirm_available = true;
		flow.cancel_available = true;
		return flow;
	}

	if (flow.presentation.classification == "AUTOMATED") {
		flow.state = OperatorActionFlowState::Delegated;
		flow.headline = "CONTROL PLANE";
		flow.detail = "Action remains delegated to operational automation within existing guardrails";
		return flow;
	}

	flow.state = OperatorActionFlowState::Blocked;
	flow.headline = "BLOCKED";
	flow.detail = "Unknown governance classification; fail closed";
	flow.cancel_available = true;
	return flow;
}

inline OperatorActionFlow confirm_operator_action_flow(const OperatorActionFlow& current) {
	OperatorActionFlow next = current;
	if (current.state != OperatorActionFlowState::ConfirmationRequired ||
		current.presentation.classification != "APPROVAL_REQUIRED") {
		return next;
	}

	next.state = OperatorActionFlowState::ApprovalRequested;
	next.headline = "APPROVAL REQUESTED";
	next.detail = "Request recorded for upstream control-plane approval; no action executed by the terminal";
	next.confirm_available = false;
	next.cancel_available = false;
	next.execution_authorized = false;
	return next;
}

inline OperatorActionFlow cancel_operator_action_flow(const OperatorActionFlow& current) {
	OperatorActionFlow next = current;
	if (!current.cancel_available) return next;
	next.state = OperatorActionFlowState::Cancelled;
	next.headline = "CANCELLED";
	next.detail = "Operator action flow cancelled without execution";
	next.confirm_available = false;
	next.cancel_available = false;
	next.execution_authorized = false;
	return next;
}

inline std::string operator_action_flow_text(const OperatorActionFlow& flow) {
	std::string text = flow.headline;
	if (!flow.action.empty()) text += " | " + flow.action;
	if (!flow.detail.empty()) text += " | " + flow.detail;
	return text;
}

} // namespace sentum::ui
