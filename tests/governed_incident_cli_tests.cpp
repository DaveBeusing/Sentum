#include <sentum/cli/CommandLine.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

sentum::cli::Options parse(std::vector<std::string> arguments) {
	std::vector<char*> argv;
	argv.reserve(arguments.size());
	for (auto& item : arguments) argv.push_back(item.data());
	return sentum::cli::parse(static_cast<int>(argv.size()), argv.data());
}

template <typename Function>
void require_throws(Function&& function, const char* message) {
	try {
		function();
	} catch (const std::exception&) {
		return;
	}
	throw std::runtime_error(message);
}

void test_incident_status() {
	const auto options = parse({"sentum", "incident", "status"});
	require(options.mode == sentum::cli::Mode::Incident, "incident status mode mismatch");
	require(options.incident_command == "status", "incident status command mismatch");
	require(!options.tui, "incident command unexpectedly enabled TUI");
}

void test_incident_decision_commands() {
	const auto approve = parse({"sentum", "incident", "approve", "req-1", "operator-a", "validated failure"});
	require(approve.incident_command == "approve", "approve command mismatch");
	require(approve.target_id == "req-1", "approve request id mismatch");
	require(approve.actor == "operator-a", "approve actor mismatch");
	require(approve.reason == "validated failure", "approve reason mismatch");

	const auto deny = parse({"sentum", "incident", "deny", "req-2", "operator-b", "not actionable"});
	require(deny.incident_command == "deny", "deny command mismatch");
	require(deny.target_id == "req-2", "deny request id mismatch");
}

void test_incident_transition_commands() {
	const auto acknowledge = parse({"sentum", "incident", "acknowledge", "inc-1", "operator-a", "triage started"});
	require(acknowledge.incident_command == "acknowledge", "acknowledge command mismatch");
	require(acknowledge.target_id == "inc-1", "acknowledge incident id mismatch");

	const auto recover = parse({
		"sentum", "incident", "recover", "inc-1", "operator-a", "reconciliation-42", "recovery validated"
	});
	require(recover.incident_command == "recover", "recover command mismatch");
	require(recover.evidence_id == "reconciliation-42", "recover evidence id mismatch");
	require(recover.reason == "recovery validated", "recover reason mismatch");

	const auto resolve = parse({"sentum", "incident", "resolve", "inc-1", "operator-a", "service restored"});
	require(resolve.incident_command == "resolve", "resolve command mismatch");

	const auto close = parse({"sentum", "incident", "close", "inc-1", "operator-a", "review recorded"});
	require(close.incident_command == "close", "close command mismatch");
}

void test_malformed_incident_commands_fail_closed() {
	require_throws([] { (void)parse({"sentum", "incident"}); }, "incident command without action was accepted");
	require_throws([] { (void)parse({"sentum", "incident", "approve", "req-1", "operator-a"}); },
		"approval without reason was accepted");
	require_throws([] { (void)parse({"sentum", "incident", "recover", "inc-1", "operator-a", "evidence"}); },
		"recovery without reason was accepted");
	require_throws([] { (void)parse({"sentum", "incident", "unknown", "inc-1", "operator-a", "reason"}); },
		"unknown incident command was accepted");
}

} // namespace

int main() {
	try {
		test_incident_status();
		test_incident_decision_commands();
		test_incident_transition_commands();
		test_malformed_incident_commands_fail_closed();
		std::cout << "governed incident CLI tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "governed incident CLI test failure: " << error.what() << '\n';
		return 1;
	}
}
