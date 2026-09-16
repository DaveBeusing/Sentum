#include <sentum/ui/OperationalSafetyPolicy.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using sentum::ui::OperationalState;
using sentum::ui::derive_operational_status;
using sentum::ui::operational_status_line;

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

nlohmann::json healthy_snapshot() {
	return {
		{"health", "healthy"},
		{"kill_switch_active", false},
		{"market_data_connected", true},
		{"performance", {{"queue_pressure", "normal"}}}
	};
}

void test_ready_and_degraded_states() {
	auto snapshot = healthy_snapshot();
	auto status = derive_operational_status(snapshot);
	require(status.state == OperationalState::Ready, "healthy runtime must be ready");
	require(status.label == "READY", "ready label mismatch");

	snapshot["market_data_connected"] = false;
	status = derive_operational_status(snapshot);
	require(status.state == OperationalState::Degraded, "market disconnect must be degraded");
	require(status.message == "Market data disconnected", "market disconnect message mismatch");

	snapshot = healthy_snapshot();
	snapshot["performance"]["queue_pressure"] = "critical";
	status = derive_operational_status(snapshot);
	require(status.state == OperationalState::Degraded, "critical persistence pressure must be degraded");
}

void test_halt_precedence() {
	auto snapshot = healthy_snapshot();
	snapshot["market_data_connected"] = false;
	snapshot["kill_switch_active"] = true;
	const auto status = derive_operational_status(snapshot);
	require(status.state == OperationalState::Halted, "kill switch must outrank degraded states");
	require(status.message == "Kill switch active", "kill switch message mismatch");
}

void test_shutdown_progress() {
	auto snapshot = healthy_snapshot();
	snapshot["health"] = "stopping";
	snapshot["shutdown_step"] = 3;
	snapshot["shutdown_total_steps"] = 6;
	snapshot["shutdown_detail"] = "Stopping persistence writer";
	const auto stopping = derive_operational_status(snapshot);
	require(stopping.state == OperationalState::Stopping, "stopping health must map to stopping state");
	require(std::abs(stopping.shutdown_progress - 0.5) < 0.0001, "shutdown progress mismatch");
	require(operational_status_line(stopping).find("step 3/6") != std::string::npos, "shutdown status line must include step progress");

	snapshot["health"] = "stopped";
	const auto stopped = derive_operational_status(snapshot);
	require(stopped.state == OperationalState::Stopped, "stopped health must map to stopped state");
	require(stopped.terminal_state, "stopped state must be terminal");
	require(stopped.shutdown_progress == 1.0, "stopped state must report complete progress");
}

void test_starting_state() {
	auto snapshot = healthy_snapshot();
	snapshot["health"] = "starting";
	const auto status = derive_operational_status(snapshot);
	require(status.state == OperationalState::Starting, "starting health must map to starting state");
}

} // namespace

int main() {
	try {
		test_ready_and_degraded_states();
		test_halt_precedence();
		test_shutdown_progress();
		test_starting_state();
		std::cout << "operational safety policy tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "operational safety policy test failure: " << error.what() << '\n';
		return 1;
	}
}
