#include <sentum/ui/TerminalWorkspacePolicy.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using sentum::ui::OperatorSeverity;
using sentum::ui::derive_operator_status;
using sentum::ui::operator_banner_text;
using sentum::ui::workspace_for_key;
using sentum::ui::workspace_help_text;
using sentum::ui::workspace_navigation_text;

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

void test_operator_presentation_contract() {
	auto status = derive_operator_status(healthy_snapshot());
	require(operator_banner_text(status) == "NORMAL", "normal banner text mismatch");

	auto warning = healthy_snapshot();
	warning["market_data_connected"] = false;
	status = derive_operator_status(warning);
	require(operator_banner_text(status) == "WARNING | Market data disconnected | Inspect SYSTEM", "warning banner text mismatch");

	const auto navigation = workspace_navigation_text("TRADES");
	require(navigation.find("[1] MARKET") != std::string::npos, "market shortcut missing from navigation");
	require(navigation.find("[4] TRADES*") != std::string::npos, "active workspace marker missing from navigation");
	require(navigation.find("[7] SYSTEM") != std::string::npos, "system shortcut missing from navigation");

	require(workspace_help_text("SYSTEM") == "SYSTEM | latency, queue, persistence and runtime health", "system workspace help mismatch");
	require(workspace_help_text("UNKNOWN").empty(), "unknown workspace must not produce help text");
}

} // namespace

int main() {
	try {
		test_operator_priority_order();
		test_workspace_contract();
		test_operator_presentation_contract();
		std::cout << "terminal workspace policy tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "terminal workspace policy test failure: " << error.what() << '\n';
		return 1;
	}
}
