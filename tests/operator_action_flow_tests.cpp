#include <sentum/ui/OperatorActionFlow.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

void test_approval_required_only_requests_approval() {
	const auto flow = sentum::ui::begin_operator_action_flow("resume_entries", "APPROVAL_REQUIRED");
	require(flow.state == sentum::ui::OperatorActionFlowState::ConfirmationRequired, "approval-required action did not enter confirmation state");
	require(flow.confirm_available, "approval-required action did not expose confirmation");
	require(!flow.execution_authorized, "terminal must never authorize execution locally");

	const auto confirmed = sentum::ui::confirm_operator_action_flow(flow);
	require(confirmed.state == sentum::ui::OperatorActionFlowState::ApprovalRequested, "confirmation did not become approval request");
	require(!confirmed.execution_authorized, "approval request incorrectly authorized execution");
	require(confirmed.detail.find("no action executed") != std::string::npos, "approval request did not state execution boundary");
}

void test_forbidden_action_is_blocked() {
	const auto flow = sentum::ui::begin_operator_action_flow("clear_kill_switch", "FORBIDDEN");
	require(flow.state == sentum::ui::OperatorActionFlowState::Blocked, "forbidden action was not blocked");
	require(!flow.confirm_available, "forbidden action exposed confirmation");
	require(!flow.execution_authorized, "forbidden action authorized execution");

	const auto unchanged = sentum::ui::confirm_operator_action_flow(flow);
	require(unchanged.state == sentum::ui::OperatorActionFlowState::Blocked, "confirmation changed blocked action state");
}

void test_unknown_classification_fails_closed() {
	const auto flow = sentum::ui::begin_operator_action_flow("maintenance_exit", "UNKNOWN");
	require(flow.state == sentum::ui::OperatorActionFlowState::Blocked, "unknown action classification did not fail closed");
	require(!flow.execution_authorized, "unknown action classification authorized execution");
}

void test_automated_action_remains_delegated() {
	const auto flow = sentum::ui::begin_operator_action_flow("refresh_observation", "AUTOMATED");
	require(flow.state == sentum::ui::OperatorActionFlowState::Delegated, "automated action not delegated to control plane");
	require(!flow.confirm_available, "delegated action exposed local confirmation");
	require(!flow.execution_authorized, "delegated action incorrectly authorized local execution");
}

void test_cancel_never_executes() {
	const auto flow = sentum::ui::begin_operator_action_flow("enter_maintenance", "APPROVAL_REQUIRED");
	const auto cancelled = sentum::ui::cancel_operator_action_flow(flow);
	require(cancelled.state == sentum::ui::OperatorActionFlowState::Cancelled, "cancellation state not recorded");
	require(!cancelled.execution_authorized, "cancelled flow authorized execution");
}

} // namespace

int main() {
	try {
		test_approval_required_only_requests_approval();
		test_forbidden_action_is_blocked();
		test_unknown_classification_fails_closed();
		test_automated_action_remains_delegated();
		test_cancel_never_executes();
		std::cout << "operator action flow tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "operator action flow test failure: " << error.what() << '\n';
		return 1;
	}
}
