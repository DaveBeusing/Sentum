#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace sentum::ui {

enum class OperationalState {
	Ready,
	Starting,
	Degraded,
	Halted,
	Stopping,
	Stopped
};

struct OperationalStatus {
	OperationalState state = OperationalState::Starting;
	std::string label = "STARTING";
	std::string message = "Runtime starting";
	std::string operator_action = "Observe SYSTEM until runtime is healthy";
	std::size_t shutdown_step = 0;
	std::size_t shutdown_total_steps = 0;
	double shutdown_progress = 0.0;
	bool terminal_state = false;
};

inline std::string safety_text(const nlohmann::json& value, const char* key, const std::string& fallback = {}) {
	if (!value.is_object() || !value.contains(key) || value[key].is_null()) return fallback;
	if (value[key].is_string()) return value[key].get<std::string>();
	return fallback;
}

inline bool safety_bool(const nlohmann::json& value, const char* key, bool fallback = false) {
	if (!value.is_object() || !value.contains(key)) return fallback;
	try { return value[key].get<bool>(); } catch (...) { return fallback; }
}

inline std::size_t safety_size(const nlohmann::json& value, const char* key) {
	if (!value.is_object() || !value.contains(key)) return 0;
	try { return value[key].get<std::size_t>(); } catch (...) { return 0; }
}

inline OperationalStatus derive_operational_status(const nlohmann::json& snapshot) {
	const auto health = safety_text(snapshot, "health", "starting");
	const bool kill_switch = safety_bool(snapshot, "kill_switch_active");
	const bool market_connected = safety_bool(snapshot, "market_data_connected");
	const auto perf = snapshot.value("performance", nlohmann::json::object());
	const auto pressure = safety_text(perf, "queue_pressure", "normal");

	if (health == "stopped") {
		OperationalStatus result;
		result.state = OperationalState::Stopped;
		result.label = "STOPPED";
		result.message = "Runtime stopped";
		result.operator_action = "Runtime is no longer active";
		result.shutdown_step = safety_size(snapshot, "shutdown_step");
		result.shutdown_total_steps = safety_size(snapshot, "shutdown_total_steps");
		result.shutdown_progress = 1.0;
		result.terminal_state = true;
		return result;
	}

	if (health == "stopping") {
		OperationalStatus result;
		result.state = OperationalState::Stopping;
		result.label = "STOPPING";
		result.shutdown_step = safety_size(snapshot, "shutdown_step");
		result.shutdown_total_steps = safety_size(snapshot, "shutdown_total_steps");
		result.message = safety_text(snapshot, "shutdown_detail", "Shutdown in progress");
		result.operator_action = "Do not start new work; allow shutdown to complete";
		if (result.shutdown_total_steps > 0) {
			result.shutdown_progress = std::clamp(
				static_cast<double>(result.shutdown_step) / static_cast<double>(result.shutdown_total_steps),
				0.0,
				1.0);
		}
		return result;
	}

	if (kill_switch) {
		return {OperationalState::Halted, "HALTED", "Kill switch active", "Inspect MARKET and SYSTEM; do not resume until the halt cause is resolved"};
	}

	if (health != "healthy" && health != "starting") {
		return {OperationalState::Halted, "HALTED", "Runtime health: " + health, "Inspect SYSTEM; trading must remain fail-closed"};
	}

	if (health == "starting") {
		return {OperationalState::Starting, "STARTING", "Runtime starting", "Observe SYSTEM until runtime is healthy"};
	}

	if (!market_connected) {
		return {OperationalState::Degraded, "DEGRADED", "Market data disconnected", "Inspect SYSTEM connectivity before relying on market state"};
	}

	if (pressure == "saturated" || pressure == "critical") {
		return {OperationalState::Degraded, "DEGRADED", "Persistence pressure: " + pressure, "Inspect SYSTEM persistence diagnostics"};
	}

	return {OperationalState::Ready, "READY", "Runtime operational", "No operator intervention required"};
}

inline std::string operational_status_line(const OperationalStatus& status) {
	std::string line = status.label + " | " + status.message;
	if (status.state == OperationalState::Stopping && status.shutdown_total_steps > 0) {
		line += " | step " + std::to_string(status.shutdown_step) + "/" + std::to_string(status.shutdown_total_steps);
	}
	if (!status.operator_action.empty()) line += " | " + status.operator_action;
	return line;
}

} // namespace sentum::ui
