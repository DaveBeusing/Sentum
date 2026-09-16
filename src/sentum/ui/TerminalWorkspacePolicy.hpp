#pragma once

#include <array>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace sentum::ui {

enum class OperatorSeverity {
	Normal = 0,
	Attention = 1,
	Warning = 2,
	Critical = 3
};

struct OperatorStatus {
	OperatorSeverity severity = OperatorSeverity::Normal;
	std::string label = "NORMAL";
	std::string message;
	std::string recommended_workspace = "MARKET";
};

struct WorkspaceDescriptor {
	char key;
	std::string_view name;
	std::string_view purpose;
};

inline constexpr std::array<WorkspaceDescriptor, 7> terminal_workspaces{{
	{'1', "MARKET", "position, price, risk and market watch"},
	{'2', "SCANNER", "candidate ranking and watchlist"},
	{'3', "ORDERS", "execution and order-state history"},
	{'4', "TRADES", "realized trade history and P&L"},
	{'5', "STRATEGY", "signal, confidence and risk decision"},
	{'6', "MODELS", "model lifecycle and promotion state"},
	{'7', "SYSTEM", "latency, queue, persistence and runtime health"}
}};

inline std::string json_text(const nlohmann::json& value, const char* key, const std::string& fallback = {}) {
	if (!value.is_object() || !value.contains(key) || value[key].is_null()) return fallback;
	if (value[key].is_string()) return value[key].get<std::string>();
	return fallback;
}

inline bool json_bool(const nlohmann::json& value, const char* key, bool fallback = false) {
	if (!value.is_object() || !value.contains(key)) return fallback;
	try { return value[key].get<bool>(); } catch (...) { return fallback; }
}

inline OperatorStatus derive_operator_status(const nlohmann::json& snapshot) {
	const auto health = json_text(snapshot, "health", "starting");
	const bool kill_switch = json_bool(snapshot, "kill_switch_active");
	const bool market_connected = json_bool(snapshot, "market_data_connected");
	const bool entries_paused = json_bool(snapshot, "entries_paused");
	const auto perf = snapshot.value("performance", nlohmann::json::object());
	const auto pressure = json_text(perf, "queue_pressure", "normal");

	if (kill_switch) {
		return {OperatorSeverity::Critical, "CRITICAL", "Kill switch active", "MARKET"};
	}
	if (health != "healthy" && health != "starting") {
		return {OperatorSeverity::Critical, "CRITICAL", "Runtime health: " + health, "SYSTEM"};
	}
	if (!market_connected) {
		return {OperatorSeverity::Warning, "WARNING", "Market data disconnected", "SYSTEM"};
	}
	if (pressure == "saturated" || pressure == "critical") {
		return {OperatorSeverity::Warning, "WARNING", "Persistence pressure: " + pressure, "SYSTEM"};
	}
	if (pressure == "elevated") {
		return {OperatorSeverity::Attention, "ATTENTION", "Persistence pressure elevated", "SYSTEM"};
	}
	if (entries_paused) {
		return {OperatorSeverity::Attention, "ATTENTION", "New entries paused", "MARKET"};
	}
	if (health == "starting") {
		return {OperatorSeverity::Attention, "ATTENTION", "Runtime starting", "SYSTEM"};
	}
	return {};
}

inline const WorkspaceDescriptor* workspace_for_key(char key) noexcept {
	for (const auto& workspace : terminal_workspaces) {
		if (workspace.key == key) return &workspace;
	}
	return nullptr;
}

inline const WorkspaceDescriptor* workspace_by_name(std::string_view name) noexcept {
	for (const auto& workspace : terminal_workspaces) {
		if (workspace.name == name) return &workspace;
	}
	return nullptr;
}

inline std::string operator_banner_text(const OperatorStatus& status) {
	std::ostringstream out;
	out << status.label;
	if (!status.message.empty()) out << " | " << status.message;
	if (!status.recommended_workspace.empty() && status.severity != OperatorSeverity::Normal) {
		out << " | Inspect " << status.recommended_workspace;
	}
	return out.str();
}

inline std::string workspace_navigation_text(std::string_view active_workspace) {
	std::ostringstream out;
	for (std::size_t index = 0; index < terminal_workspaces.size(); ++index) {
		const auto& workspace = terminal_workspaces[index];
		if (index != 0) out << "   ";
		out << '[' << workspace.key << "] " << workspace.name;
		if (workspace.name == active_workspace) out << '*';
	}
	return out.str();
}

inline std::string workspace_help_text(std::string_view active_workspace) {
	const auto* workspace = workspace_by_name(active_workspace);
	if (workspace == nullptr) return {};
	return std::string(workspace->name) + " | " + std::string(workspace->purpose);
}

} // namespace sentum::ui
