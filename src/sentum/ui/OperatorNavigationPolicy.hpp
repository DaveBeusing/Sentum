#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include <sentum/ui/OperatorAuditQueueView.hpp>
#include <sentum/ui/OperatorActionFlow.hpp>

namespace sentum::ui {

enum class OperatorFocusRegion {
	ApprovalQueue,
	AuditTimeline,
	WorkflowDetail
};

enum class OperatorNavigationCommand {
	None,
	MovePrevious,
	MoveNext,
	FocusPrevious,
	FocusNext,
	Open,
	Cancel
};

struct OperatorNavigationState {
	OperatorFocusRegion focus = OperatorFocusRegion::ApprovalQueue;
	std::size_t approval_index = 0;
	std::size_t audit_index = 0;
	std::string selected_request_id;
	std::string selected_action;
	std::string status = "BROWSE";
	bool confirmation_open = false;
	bool execution_authorized = false;
};

inline OperatorNavigationCommand operator_navigation_command(char key) noexcept {
	switch (key) {
		case 'k':
		case 'K':
			return OperatorNavigationCommand::MovePrevious;
		case 'j':
		case 'J':
			return OperatorNavigationCommand::MoveNext;
		case '[':
			return OperatorNavigationCommand::FocusPrevious;
		case ']':
		case '\t':
			return OperatorNavigationCommand::FocusNext;
		case '\r':
		case '\n':
			return OperatorNavigationCommand::Open;
		case 27:
			return OperatorNavigationCommand::Cancel;
		default:
			return OperatorNavigationCommand::None;
	}
}

inline std::size_t bounded_previous(std::size_t index, std::size_t count) noexcept {
	if (count == 0) return 0;
	return index == 0 ? count - 1 : index - 1;
}

inline std::size_t bounded_next(std::size_t index, std::size_t count) noexcept {
	if (count == 0) return 0;
	return (index + 1) % count;
}

inline OperatorFocusRegion previous_focus(OperatorFocusRegion focus) noexcept {
	if (focus == OperatorFocusRegion::ApprovalQueue) return OperatorFocusRegion::WorkflowDetail;
	if (focus == OperatorFocusRegion::AuditTimeline) return OperatorFocusRegion::ApprovalQueue;
	return OperatorFocusRegion::AuditTimeline;
}

inline OperatorFocusRegion next_focus(OperatorFocusRegion focus) noexcept {
	if (focus == OperatorFocusRegion::ApprovalQueue) return OperatorFocusRegion::AuditTimeline;
	if (focus == OperatorFocusRegion::AuditTimeline) return OperatorFocusRegion::WorkflowDetail;
	return OperatorFocusRegion::ApprovalQueue;
}

inline OperatorNavigationState reconcile_operator_navigation(
	OperatorNavigationState state,
	const OperatorAuditQueueView& evidence) {
	state.execution_authorized = false;
	state.approval_index = evidence.approvals.empty()
		? 0
		: std::min(state.approval_index, evidence.approvals.size() - 1);
	state.audit_index = evidence.audit.empty()
		? 0
		: std::min(state.audit_index, evidence.audit.size() - 1);

	if (evidence.evidence_state == OperatorEvidenceState::Missing) {
		state.confirmation_open = false;
		state.selected_request_id.clear();
		state.selected_action.clear();
		state.status = "EVIDENCE MISSING - READ ONLY";
		return state;
	}

	if (evidence.evidence_state == OperatorEvidenceState::Stale) {
		state.confirmation_open = false;
		state.selected_request_id.clear();
		state.selected_action.clear();
		state.status = "EVIDENCE STALE - BLOCKED";
		return state;
	}

	if (state.confirmation_open) {
		const auto selected = std::find_if(
			evidence.approvals.begin(),
			evidence.approvals.end(),
			[&state](const OperatorApprovalItem& item) {
				return item.request_id == state.selected_request_id &&
					item.action == state.selected_action &&
					item.classification == "APPROVAL_REQUIRED";
			});
		if (selected == evidence.approvals.end()) {
			state.confirmation_open = false;
			state.selected_request_id.clear();
			state.selected_action.clear();
			state.status = "SELECTION CHANGED - CONFIRMATION CANCELLED";
			return state;
		}
	}

	if (state.focus == OperatorFocusRegion::ApprovalQueue && evidence.approvals.empty()) {
		state.selected_request_id.clear();
		state.selected_action.clear();
		state.status = "APPROVAL QUEUE EMPTY";
	} else if (state.focus == OperatorFocusRegion::AuditTimeline && evidence.audit.empty()) {
		state.selected_request_id.clear();
		state.selected_action.clear();
		state.status = "AUDIT TIMELINE EMPTY";
	}
	return state;
}

inline OperatorNavigationState apply_operator_navigation(
	OperatorNavigationState state,
	OperatorNavigationCommand command,
	const OperatorAuditQueueView& evidence) {
	state = reconcile_operator_navigation(std::move(state), evidence);
	state.execution_authorized = false;

	if (command == OperatorNavigationCommand::Cancel) {
		state.confirmation_open = false;
		state.selected_request_id.clear();
		state.selected_action.clear();
		state.status = "CANCELLED";
		return state;
	}

	if (evidence.evidence_state != OperatorEvidenceState::Available) return state;

	if (state.confirmation_open) {
		state.status = "CONFIRMATION OPEN - ENTER CANNOT EXECUTE";
		return state;
	}

	if (command == OperatorNavigationCommand::FocusPrevious) {
		state.focus = previous_focus(state.focus);
		state.status = "BROWSE";
		return reconcile_operator_navigation(std::move(state), evidence);
	}
	if (command == OperatorNavigationCommand::FocusNext) {
		state.focus = next_focus(state.focus);
		state.status = "BROWSE";
		return reconcile_operator_navigation(std::move(state), evidence);
	}

	if (state.focus == OperatorFocusRegion::ApprovalQueue) {
		if (command == OperatorNavigationCommand::MovePrevious) {
			state.approval_index = bounded_previous(state.approval_index, evidence.approvals.size());
			state.status = evidence.approvals.empty() ? "APPROVAL QUEUE EMPTY" : "BROWSE";
		} else if (command == OperatorNavigationCommand::MoveNext) {
			state.approval_index = bounded_next(state.approval_index, evidence.approvals.size());
			state.status = evidence.approvals.empty() ? "APPROVAL QUEUE EMPTY" : "BROWSE";
		} else if (command == OperatorNavigationCommand::Open && !evidence.approvals.empty()) {
			const auto& item = evidence.approvals[state.approval_index];
			state.selected_request_id = item.request_id;
			state.selected_action = item.action;
			const auto flow = begin_operator_action_flow(item.action, item.classification);
			if (flow.state == OperatorActionFlowState::ConfirmationRequired) {
				state.confirmation_open = true;
				state.status = "CONFIRM APPROVAL REQUEST";
			} else if (flow.state == OperatorActionFlowState::Blocked) {
				state.status = "BLOCKED";
			} else {
				state.status = "READ ONLY";
			}
		}
		return state;
	}

	if (state.focus == OperatorFocusRegion::AuditTimeline) {
		if (command == OperatorNavigationCommand::MovePrevious) {
			state.audit_index = bounded_previous(state.audit_index, evidence.audit.size());
			state.status = evidence.audit.empty() ? "AUDIT TIMELINE EMPTY" : "BROWSE";
		} else if (command == OperatorNavigationCommand::MoveNext) {
			state.audit_index = bounded_next(state.audit_index, evidence.audit.size());
			state.status = evidence.audit.empty() ? "AUDIT TIMELINE EMPTY" : "BROWSE";
		} else if (command == OperatorNavigationCommand::Open && !evidence.audit.empty()) {
			const auto& item = evidence.audit[state.audit_index];
			state.selected_request_id = item.request_id;
			state.selected_action = item.action;
			state.status = "AUDIT DETAIL - READ ONLY";
		}
		return state;
	}

	state.status = command == OperatorNavigationCommand::Open ? "WORKFLOW DETAIL - READ ONLY" : "BROWSE";
	return state;
}

inline std::string operator_focus_name(OperatorFocusRegion focus) {
	if (focus == OperatorFocusRegion::ApprovalQueue) return "APPROVALS";
	if (focus == OperatorFocusRegion::AuditTimeline) return "AUDIT";
	return "WORKFLOWS";
}

inline std::string operator_navigation_text(const OperatorNavigationState& state) {
	std::string text = "Focus " + operator_focus_name(state.focus) + " | j/k select | Tab/] next | [ previous | Enter open | Esc cancel";
	if (!state.status.empty()) text += " | " + state.status;
	if (!state.selected_request_id.empty()) text += " | request " + state.selected_request_id;
	return text;
}

} // namespace sentum::ui
