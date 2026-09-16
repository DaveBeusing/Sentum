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

struct OperatorActionPresentation {
	std::string classification = "FORBIDDEN";
	std::string label = "BLOCKED";
	std::string guidance = "Action classification unavailable; fail closed";
	bool confirmation_required = true;
	bool blocked = true;
};

struct OperatorControlSurface {
	OperatorStatus status;
	std::string active_workspace;
	std::string banner;
	std::string navigation;
	std::string workspace_help;
	std::string governance_state = "UNAVAILABLE";
	std::string maintenance_state = "NORMAL";
	std::string incident_state = "NONE";
	std::string recovery_state = "IDLE";
	std::size_t pending_approvals = 0;
	std::string approval_summary;
	std::string audit_summary;
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

inline std::size_t json_size(const nlohmann::json& value, const char* key, std::size_t fallback = 0) {
	if (!value.is_object() || !value.contains(key)) return fallback;
	try { return value[key].get<std::size_t>(); } catch (...) { return fallback; }
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

inline OperatorActionPresentation operator_action_presentation(std::string_view classification) {
	if (classification == "AUTOMATED") {
		return {"AUTOMATED", "AVAILABLE", "Operational automation may execute within existing guardrails", false, false};
	}
	if (classification == "APPROVAL_REQUIRED") {
		return {"APPROVAL_REQUIRED", "APPROVAL REQUIRED", "Explicit operator approval is required before execution", true, false};
	}
	if (classification == "FORBIDDEN") {
		return {"FORBIDDEN", "BLOCKED", "Action is prohibited by operational governance", true, true};
	}
	return {};
}

inline OperatorControlSurface derive_operator_control_surface(
	const nlohmann::json& snapshot,
	std::string_view active_workspace) {
	OperatorControlSurface surface;
	surface.status = derive_operator_status(snapshot);
	surface.active_workspace = std::string(active_workspace);
	surface.banner = operator_banner_text(surface.status);
	surface.navigation = workspace_navigation_text(active_workspace);
	surface.workspace_help = workspace_help_text(active_workspace);

	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	surface.governance_state = json_text(control_plane, "governance_state", "UNAVAILABLE");
	surface.maintenance_state = json_text(control_plane, "maintenance_state", "NORMAL");
	surface.incident_state = json_text(control_plane, "incident_state", "NONE");
	surface.recovery_state = json_text(control_plane, "recovery_state", "IDLE");
	surface.pending_approvals = json_size(control_plane, "pending_approvals");

	if (surface.pending_approvals > 0) {
		surface.approval_summary = std::to_string(surface.pending_approvals) + " approval(s) pending";
	} else if (surface.governance_state == "UNAVAILABLE") {
		surface.approval_summary = "Governance evidence unavailable";
	} else {
		surface.approval_summary = "No approvals pending";
	}

	const auto last_action = json_text(control_plane, "last_audit_action");
	const auto last_actor = json_text(control_plane, "last_audit_actor");
	if (!last_action.empty()) {
		surface.audit_summary = "Last action: " + last_action;
		if (!last_actor.empty()) surface.audit_summary += " by " + last_actor;
	} else {
		surface.audit_summary = "No governed action recorded";
	}
	return surface;
}

} // namespace sentum::ui
