#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/ui/TerminalWorkspacePolicy.hpp>

namespace sentum::ui {

enum class OperatorAlertSeverity {
	Info = 0,
	Attention = 1,
	Warning = 2,
	Critical = 3
};

struct OperatorAlert {
	std::string id;
	std::string source;
	OperatorAlertSeverity severity = OperatorAlertSeverity::Info;
	std::string title;
	std::string message;
	std::string guidance;
	std::string recommended_workspace = "SYSTEM";
	bool acknowledgement_required = false;
	int escalation_level = 0;
	bool notification_candidate = false;
	bool execution_authorized = false;
};

inline const char* operator_alert_severity_name(OperatorAlertSeverity severity) noexcept {
	switch (severity) {
		case OperatorAlertSeverity::Info: return "INFO";
		case OperatorAlertSeverity::Attention: return "ATTENTION";
		case OperatorAlertSeverity::Warning: return "WARNING";
		case OperatorAlertSeverity::Critical: return "CRITICAL";
	}
	return "CRITICAL";
}

inline OperatorAlert make_operator_alert(
	std::string id,
	std::string source,
	OperatorAlertSeverity severity,
	std::string title,
	std::string message,
	std::string guidance,
	std::string workspace,
	bool acknowledgement_required,
	int escalation_level,
	bool notification_candidate) {
	return {
		std::move(id),
		std::move(source),
		severity,
		std::move(title),
		std::move(message),
		std::move(guidance),
		std::move(workspace),
		acknowledgement_required,
		escalation_level,
		notification_candidate,
		false
	};
}

inline std::vector<OperatorAlert> derive_operator_alerts(const nlohmann::json& snapshot) {
	std::vector<OperatorAlert> alerts;
	const auto health = json_text(snapshot, "health", "starting");
	const bool kill_switch = json_bool(snapshot, "kill_switch_active");
	const bool market_connected = json_bool(snapshot, "market_data_connected");
	const bool entries_paused = json_bool(snapshot, "entries_paused");
	const auto performance = snapshot.value("performance", nlohmann::json::object());
	const auto pressure = json_text(performance, "queue_pressure", "normal");
	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	const auto governance = json_text(control_plane, "governance_state", "UNAVAILABLE");
	const auto evidence = json_text(control_plane, "evidence_status", json_text(control_plane, "evidence_state", "MISSING"));
	const auto incident = json_text(control_plane, "incident_state", "NONE");
	const auto pending_approvals = json_size(control_plane, "pending_approvals");

	if (kill_switch) {
		alerts.push_back(make_operator_alert(
			"runtime.kill_switch",
			"RUNTIME",
			OperatorAlertSeverity::Critical,
			"Kill switch active",
			"Runtime safety interlock is active",
			"Inspect runtime and execution state. Clearing the interlock requires the governed control-plane flow.",
			"MARKET",
			true,
			3,
			true));
	}

	if (health != "healthy" && health != "starting") {
		alerts.push_back(make_operator_alert(
			"runtime.health",
			"RUNTIME",
			OperatorAlertSeverity::Critical,
			"Runtime health degraded",
			"Runtime health: " + health,
			"Inspect SYSTEM diagnostics and preserve fail-closed operating state until health is understood.",
			"SYSTEM",
			true,
			3,
			true));
	}

	if (!market_connected) {
		alerts.push_back(make_operator_alert(
			"market.disconnected",
			"MARKET_DATA",
			OperatorAlertSeverity::Warning,
			"Market data disconnected",
			"Live market connectivity is unavailable",
			"Inspect market-data connectivity and keep entry decisions fail closed while disconnected.",
			"SYSTEM",
			true,
			2,
			true));
	}

	if (pressure == "saturated" || pressure == "critical") {
		alerts.push_back(make_operator_alert(
			"persistence.pressure",
			"PERSISTENCE",
			OperatorAlertSeverity::Warning,
			"Persistence pressure critical",
			"Persistence queue pressure: " + pressure,
			"Inspect persistence throughput, backlog and disk/database health.",
			"SYSTEM",
			true,
			2,
			true));
	} else if (pressure == "elevated") {
		alerts.push_back(make_operator_alert(
			"persistence.pressure",
			"PERSISTENCE",
			OperatorAlertSeverity::Attention,
			"Persistence pressure elevated",
			"Persistence queue pressure is elevated",
			"Observe queue trend and persistence latency before pressure becomes critical.",
			"SYSTEM",
			false,
			1,
			false));
	}

	if (governance == "UNAVAILABLE") {
		alerts.push_back(make_operator_alert(
			"governance.unavailable",
			"CONTROL_PLANE",
			OperatorAlertSeverity::Warning,
			"Governance evidence unavailable",
			"Operations control-plane governance state is unavailable",
			"Treat governed actions as unavailable and inspect control-plane evidence.",
			"SYSTEM",
			true,
			2,
			true));
	}

	if (evidence == "STALE") {
		alerts.push_back(make_operator_alert(
			"governance.evidence_stale",
			"CONTROL_PLANE",
			OperatorAlertSeverity::Warning,
			"Governance evidence stale",
			"Operations evidence is no longer fresh",
			"Do not rely on stale approval state. Restore fresh control-plane evidence before governed actions.",
			"SYSTEM",
			true,
			2,
			true));
	}

	if (incident != "NONE" && incident != "RESOLVED") {
		alerts.push_back(make_operator_alert(
			"operations.incident",
			"INCIDENT",
			OperatorAlertSeverity::Warning,
			"Operational incident active",
			"Incident state: " + incident,
			"Review incident context and follow the governed incident workflow.",
			"SYSTEM",
			true,
			2,
			true));
	}

	if (pending_approvals > 0) {
		alerts.push_back(make_operator_alert(
			"operations.approvals_pending",
			"CONTROL_PLANE",
			OperatorAlertSeverity::Attention,
			"Approvals pending",
			std::to_string(pending_approvals) + " governed approval(s) pending",
			"Review the approval queue. Presentation does not authorize or execute requests.",
			"SYSTEM",
			false,
			1,
			false));
	}

	if (entries_paused && !kill_switch) {
		alerts.push_back(make_operator_alert(
			"runtime.entries_paused",
			"RUNTIME",
			OperatorAlertSeverity::Attention,
			"New entries paused",
			"New entries are paused while existing position management remains active",
			"Inspect MARKET state. Resuming entries remains approval-gated.",
			"MARKET",
			false,
			1,
			false));
	}

	std::stable_sort(alerts.begin(), alerts.end(), [](const OperatorAlert& left, const OperatorAlert& right) {
		if (left.severity != right.severity) {
			return static_cast<int>(left.severity) > static_cast<int>(right.severity);
		}
		return left.id < right.id;
	});
	return alerts;
}

inline std::size_t count_operator_alerts_at_least(
	const std::vector<OperatorAlert>& alerts,
	OperatorAlertSeverity minimum) noexcept {
	return static_cast<std::size_t>(std::count_if(alerts.begin(), alerts.end(), [minimum](const OperatorAlert& alert) {
		return static_cast<int>(alert.severity) >= static_cast<int>(minimum);
	}));
}

} // namespace sentum::ui
