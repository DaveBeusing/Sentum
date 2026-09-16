#include <sentum/ui/TerminalWorkspacePolicy.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using sentum::ui::OperatorSeverity;
using sentum::ui::derive_operator_status;
using sentum::ui::workspace_for_key;

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

nlohmann::json healthy_snapshot() {
	return {
		{"health", "healthy"},
		{"kill_switch_active", false},
		{"market_data_connected", true},
		{"entries_paused", false},
		{"performance", {{"queue_pressure", "normal"}}}
	};
}

void test_operator_priority_order() {
	auto snapshot = healthy_snapshot();
	auto status = derive_operator_status(snapshot);
	require(status.severity == OperatorSeverity::Normal, "healthy snapshot must be normal");

	snapshot["entries_paused"] = true;
	status = derive_operator_status(snapshot);
	require(status.severity == OperatorSeverity::Attention, "paused entries must require attention");
	require(status.recommended_workspace == "MARKET", "paused entries should direct operator to market workspace");

	snapshot["performance"]["queue_pressure"] = "critical";
	status = derive_operator_status(snapshot);
	require(status.severity == OperatorSeverity::Warning, "critical persistence pressure must outrank paused entries");
	require(status.recommended_workspace == "SYSTEM", "persistence pressure should direct operator to system workspace");

	snapshot["market_data_connected"] = false;
	status = derive_operator_status(snapshot);
	require(status.message == "Market data disconnected", "market disconnect must outrank persistence pressure");

	snapshot["kill_switch_active"] = true;
	status = derive_operator_status(snapshot);
	require(status.severity == OperatorSeverity::Critical, "kill switch must be critical");
	require(status.message == "Kill switch active", "kill switch message mismatch");
}

void test_workspace_contract() {
	const auto* market = workspace_for_key('1');
	const auto* system = workspace_for_key('7');
	require(market != nullptr && market->name == "MARKET", "market workspace key mismatch");
	require(system != nullptr && system->name == "SYSTEM", "system workspace key mismatch");
	require(workspace_for_key('8') == nullptr, "unexpected workspace for unsupported key");
}

} // namespace

int main() {
	try {
		test_operator_priority_order();
		test_workspace_contract();
		std::cout << "terminal workspace policy tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "terminal workspace policy test failure: " << error.what() << '\n';
		return 1;
	}
}
