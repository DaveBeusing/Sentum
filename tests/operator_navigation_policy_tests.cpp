#include <sentum/ui/OperatorNavigationPolicy.hpp>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

sentum::ui::OperatorAuditQueueView evidence() {
	sentum::ui::OperatorAuditQueueView view;
	view.approvals = {
		{"req-1", "resume_entries", "APPROVAL_REQUIRED", "operator-a", "review", "APPROVAL REQUIRED", false},
		{"req-2", "override_exchange_truth", "FORBIDDEN", "operator-b", "unsafe", "BLOCKED", false}
	};
	view.audit = {
		{"req-a", "enter_maintenance", "operator-a", "planned", "RECORDED", "2026-09-16T16:00:00Z"},
		{"req-b", "acknowledge_incident", "operator-b", "triage", "RECORDED", "2026-09-16T16:01:00Z"}
	};
	view.approval_total = view.approvals.size();
	view.audit_total = view.audit.size();
	view.evidence_state = sentum::ui::OperatorEvidenceState::Available;
	view.evidence_status = "AVAILABLE";
	return view;
}

void test_navigation_keys_never_authorize_execution() {
	auto state = sentum::ui::OperatorNavigationState{};
	const auto view = evidence();
	state = sentum::ui::apply_operator_navigation(state, sentum::ui::operator_navigation_command('j'), view);
	require(state.approval_index == 1, "j did not move approval selection");
	require(!state.execution_authorized, "navigation authorized execution");
	state = sentum::ui::apply_operator_navigation(state, sentum::ui::operator_navigation_command('k'), view);
	require(state.approval_index == 0, "k did not move approval selection");
	require(!state.execution_authorized, "reverse navigation authorized execution");
}

void test_tab_changes_focus_only() {
	auto state = sentum::ui::OperatorNavigationState{};
	const auto view = evidence();
	state = sentum::ui::apply_operator_navigation(state, sentum::ui::operator_navigation_command('\t'), view);
	require(state.focus == sentum::ui::OperatorFocusRegion::AuditTimeline, "Tab did not move focus to audit");
	require(!state.confirmation_open, "focus navigation opened confirmation");
	require(!state.execution_authorized, "focus navigation authorized execution");
}

void test_enter_on_approval_only_opens_confirmation() {
	auto state = sentum::ui::OperatorNavigationState{};
	const auto view = evidence();
	state = sentum::ui::apply_operator_navigation(state, sentum::ui::OperatorNavigationCommand::Open, view);
	require(state.confirmation_open, "approval-required item did not open confirmation");
	require(state.status == "CONFIRM APPROVAL REQUEST", "approval confirmation status incorrect");
	require(!state.execution_authorized, "opening approval confirmation authorized execution");
	const auto second_enter = sentum::ui::apply_operator_navigation(state, sentum::ui::OperatorNavigationCommand::Open, view);
	require(second_enter.confirmation_open, "second Enter unexpectedly closed confirmation");
	require(!second_enter.execution_authorized, "second Enter authorized execution");
}

void test_forbidden_item_stays_blocked() {
	auto state = sentum::ui::OperatorNavigationState{};
	state.approval_index = 1;
	const auto view = evidence();
	state = sentum::ui::apply_operator_navigation(state, sentum::ui::OperatorNavigationCommand::Open, view);
	require(!state.confirmation_open, "forbidden item opened confirmation");
	require(state.status == "BLOCKED", "forbidden item not blocked");
	require(!state.execution_authorized, "forbidden item authorized execution");
}

void test_audit_and_workflow_focus_are_read_only() {
	auto state = sentum::ui::OperatorNavigationState{};
	const auto view = evidence();
	state.focus = sentum::ui::OperatorFocusRegion::AuditTimeline;
	state = sentum::ui::apply_operator_navigation(state, sentum::ui::OperatorNavigationCommand::Open, view);
	require(state.status == "AUDIT DETAIL - READ ONLY", "audit open was not read only");
	require(!state.execution_authorized, "audit view authorized execution");
	state.focus = sentum::ui::OperatorFocusRegion::WorkflowDetail;
	state = sentum::ui::apply_operator_navigation(state, sentum::ui::OperatorNavigationCommand::Open, view);
	require(state.status == "WORKFLOW DETAIL - READ ONLY", "workflow open was not read only");
	require(!state.execution_authorized, "workflow view authorized execution");
}

void test_escape_cancels_without_side_effects() {
	auto state = sentum::ui::OperatorNavigationState{};
	const auto view = evidence();
	state = sentum::ui::apply_operator_navigation(state, sentum::ui::OperatorNavigationCommand::Open, view);
	require(state.confirmation_open, "precondition confirmation missing");
	state = sentum::ui::apply_operator_navigation(state, sentum::ui::OperatorNavigationCommand::Cancel, view);
	require(!state.confirmation_open, "Esc did not close confirmation");
	require(state.status == "CANCELLED", "Esc did not set cancelled status");
	require(!state.execution_authorized, "Esc path authorized execution");
}

void test_queue_shrink_clamps_selection() {
	auto state = sentum::ui::OperatorNavigationState{};
	state.approval_index = 1;
	auto view = evidence();
	view.approvals.resize(1);
	view.approval_total = 1;
	state = sentum::ui::reconcile_operator_navigation(state, view);
	require(state.approval_index == 0, "approval selection was not clamped after queue shrink");
	require(!state.execution_authorized, "queue shrink reconciliation authorized execution");
}

void test_disappearing_confirmation_is_cancelled() {
	auto state = sentum::ui::OperatorNavigationState{};
	auto view = evidence();
	state = sentum::ui::apply_operator_navigation(state, sentum::ui::OperatorNavigationCommand::Open, view);
	require(state.confirmation_open, "precondition confirmation missing");
	view.approvals.erase(view.approvals.begin());
	view.approval_total = view.approvals.size();
	state = sentum::ui::reconcile_operator_navigation(state, view);
	require(!state.confirmation_open, "disappearing request left confirmation open");
	require(state.status == "SELECTION CHANGED - CONFIRMATION CANCELLED", "disappearing request did not invalidate confirmation");
	require(!state.execution_authorized, "confirmation invalidation authorized execution");
}

void test_classification_change_invalidates_confirmation() {
	auto state = sentum::ui::OperatorNavigationState{};
	auto view = evidence();
	state = sentum::ui::apply_operator_navigation(state, sentum::ui::OperatorNavigationCommand::Open, view);
	view.approvals[0].classification = "FORBIDDEN";
	view.approvals[0].status = "BLOCKED";
	state = sentum::ui::reconcile_operator_navigation(state, view);
	require(!state.confirmation_open, "classification change left confirmation open");
	require(state.status == "SELECTION CHANGED - CONFIRMATION CANCELLED", "classification change did not cancel confirmation");
}

void test_missing_and_stale_evidence_fail_closed() {
	auto state = sentum::ui::OperatorNavigationState{};
	auto view = evidence();
	view.evidence_state = sentum::ui::OperatorEvidenceState::Missing;
	view.evidence_status = "MISSING";
	state = sentum::ui::apply_operator_navigation(state, sentum::ui::OperatorNavigationCommand::Open, view);
	require(state.status == "EVIDENCE MISSING - READ ONLY", "missing evidence did not force read-only state");
	require(!state.confirmation_open, "missing evidence opened confirmation");

	view = evidence();
	view.evidence_state = sentum::ui::OperatorEvidenceState::Stale;
	view.evidence_status = "STALE";
	state = sentum::ui::apply_operator_navigation({}, sentum::ui::OperatorNavigationCommand::Open, view);
	require(state.status == "EVIDENCE STALE - BLOCKED", "stale evidence did not block navigation action");
	require(!state.confirmation_open, "stale evidence opened confirmation");
	require(!state.execution_authorized, "stale evidence authorized execution");
}

} // namespace

int main() {
	try {
		test_navigation_keys_never_authorize_execution();
		test_tab_changes_focus_only();
		test_enter_on_approval_only_opens_confirmation();
		test_forbidden_item_stays_blocked();
		test_audit_and_workflow_focus_are_read_only();
		test_escape_cancels_without_side_effects();
		test_queue_shrink_clamps_selection();
		test_disappearing_confirmation_is_cancelled();
		test_classification_change_invalidates_confirmation();
		test_missing_and_stale_evidence_fail_closed();
		std::cout << "operator navigation policy tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "operator navigation policy test failure: " << error.what() << '\n';
		return 1;
	}
}
