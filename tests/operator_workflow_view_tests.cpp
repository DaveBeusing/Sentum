#include <sentum/ui/OperatorWorkflowView.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

void test_maintenance_requires_approval_without_execution_authority() {
	const nlohmann::json control_plane = {
		{"maintenance_workflow", {
			{"state", "REQUESTED"},
			{"action", "enter_maintenance"},
			{"classification", "APPROVAL_REQUIRED"},
			{"request_id", "maint-17"},
			{"reason", "planned database maintenance"},
			{"actor", "operator-a"}
		}}
	};
	const auto view = sentum::ui::maintenance_operator_workflow(control_plane);
	require(view.approval_required, "maintenance approval requirement was lost");
	require(!view.blocked, "valid maintenance approval request was blocked");
	require(!view.execution_authorized, "maintenance view authorized execution");
	require(view.summary.find("request maint-17") != std::string::npos, "maintenance request id missing");
}

void test_incident_acknowledgement_remains_governed() {
	const nlohmann::json control_plane = {
		{"incident_workflow", {
			{"state", "OPEN"},
			{"action", "acknowledge_incident"},
			{"classification", "APPROVAL_REQUIRED"},
			{"request_id", "inc-9"},
			{"reason", "market data disconnect"}
		}}
	};
	const auto view = sentum::ui::incident_operator_workflow(control_plane);
	require(view.approval_required, "incident acknowledgement did not require approval");
	require(!view.execution_authorized, "incident view authorized execution");
	require(view.summary.find("INCIDENT | OPEN") != std::string::npos, "incident state missing");
}

void test_recovery_requires_explicit_approval() {
	const nlohmann::json control_plane = {
		{"recovery_workflow", {
			{"state", "CANDIDATE"},
			{"action", "promote_recovery_candidate"},
			{"classification", "APPROVAL_REQUIRED"},
			{"request_id", "rec-4"}
		}}
	};
	const auto view = sentum::ui::recovery_operator_workflow(control_plane);
	require(view.approval_required, "recovery promotion approval requirement missing");
	require(!view.execution_authorized, "recovery view authorized execution");
}

void test_missing_classification_fails_closed() {
	const nlohmann::json control_plane = {
		{"recovery_workflow", {
			{"state", "CANDIDATE"},
			{"action", "promote_recovery_candidate"}
		}}
	};
	const auto view = sentum::ui::recovery_operator_workflow(control_plane);
	require(view.blocked, "workflow without classification did not fail closed");
	require(view.classification == "FORBIDDEN", "workflow without classification was not forbidden");
	require(!view.execution_authorized, "blocked workflow authorized execution");
}

void test_idle_workflows_are_visible_without_authority() {
	const nlohmann::json control_plane = nlohmann::json::object();
	const auto text = sentum::ui::operator_workflows_text(control_plane);
	require(text.find("MAINTENANCE | IDLE") != std::string::npos, "maintenance idle state missing");
	require(text.find("INCIDENT | IDLE") != std::string::npos, "incident idle state missing");
	require(text.find("RECOVERY | IDLE") != std::string::npos, "recovery idle state missing");
}

} // namespace

int main() {
	try {
		test_maintenance_requires_approval_without_execution_authority();
		test_incident_acknowledgement_remains_governed();
		test_recovery_requires_explicit_approval();
		test_missing_classification_fails_closed();
		test_idle_workflows_are_visible_without_authority();
		std::cout << "operator workflow view tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "operator workflow view test failure: " << error.what() << '\n';
		return 1;
	}
}
