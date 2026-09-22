#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/ui/TerminalWorkspacePolicy.hpp>

namespace sentum::ui {

enum class OperatorEvidenceState {
	Available,
	Missing,
	Stale
};

struct OperatorApprovalItem {
	std::string request_id;
	std::string action;
	std::string classification = "FORBIDDEN";
	std::string actor;
	std::string reason;
	std::string status = "BLOCKED";
	bool execution_authorized = false;
};

struct OperatorAuditItem {
	std::string request_id;
	std::string action;
	std::string actor;
	std::string reason;
	std::string outcome;
	std::string timestamp_utc;
};

struct OperatorAuditQueueView {
	std::vector<OperatorApprovalItem> approvals;
	std::vector<OperatorAuditItem> audit;
	std::size_t approval_total = 0;
	std::size_t audit_total = 0;
	bool truncated = false;
	OperatorEvidenceState evidence_state = OperatorEvidenceState::Missing;
	std::string evidence_status = "MISSING";
};

inline OperatorEvidenceState operator_evidence_state(const nlohmann::json& snapshot) {
	if (!snapshot.contains("operations_control_plane") || !snapshot["operations_control_plane"].is_object()) {
		return OperatorEvidenceState::Missing;
	}
	const auto& control_plane = snapshot["operations_control_plane"];
	const auto status = json_text(control_plane, "evidence_status", "AVAILABLE");
	if (status == "STALE" || control_plane.value("evidence_stale", false)) return OperatorEvidenceState::Stale;
	return OperatorEvidenceState::Available;
}

inline const char* operator_evidence_state_text(OperatorEvidenceState state) noexcept {
	if (state == OperatorEvidenceState::Available) return "AVAILABLE";
	if (state == OperatorEvidenceState::Stale) return "STALE";
	return "MISSING";
}

inline OperatorAuditQueueView derive_operator_audit_queue_view(
	const nlohmann::json& snapshot,
	std::size_t approval_limit = 8,
	std::size_t audit_limit = 12) {
	OperatorAuditQueueView view;
	view.evidence_state = operator_evidence_state(snapshot);
	view.evidence_status = operator_evidence_state_text(view.evidence_state);

	if (view.evidence_state == OperatorEvidenceState::Missing) return view;

	const auto control_plane = snapshot.value("operations_control_plane", nlohmann::json::object());
	const auto approvals = control_plane.value("approval_queue", nlohmann::json::array());
	const auto audit = control_plane.value("audit_timeline", nlohmann::json::array());

	if (approvals.is_array()) {
		view.approval_total = std::max<std::size_t>(
			approvals.size(),
			control_plane.value("approval_queue_total", approvals.size()));
		const auto count = std::min<std::size_t>(approval_limit, approvals.size());
		view.approvals.reserve(count);
		for (std::size_t index = 0; index < count; ++index) {
			const auto& item = approvals[index];
			OperatorApprovalItem row;
			row.request_id = json_text(item, "request_id", "-");
			row.action = json_text(item, "action", "-");
			row.classification = json_text(item, "classification", "");
			row.actor = json_text(item, "actor", "-");
			row.reason = json_text(item, "reason", "-");
			const auto presentation = operator_action_presentation(row.classification);
			row.classification = presentation.classification;
			row.status = presentation.blocked ? "BLOCKED" : presentation.label;
			if (view.evidence_state == OperatorEvidenceState::Stale) {
				row.classification = "FORBIDDEN";
				row.status = "BLOCKED - STALE EVIDENCE";
			}
			row.execution_authorized = false;
			view.approvals.push_back(std::move(row));
		}
	}

	if (audit.is_array()) {
		view.audit_total = std::max<std::size_t>(
			audit.size(),
			control_plane.value("audit_timeline_total", audit.size()));
		const auto count = std::min<std::size_t>(audit_limit, audit.size());
		view.audit.reserve(count);
		for (std::size_t index = 0; index < count; ++index) {
			const auto& item = audit[index];
			OperatorAuditItem row;
			row.request_id = json_text(item, "request_id", "-");
			row.action = json_text(item, "action", "-");
			row.actor = json_text(item, "actor", "-");
			row.reason = json_text(item, "reason", "-");
			row.outcome = json_text(item, "outcome", "-");
			row.timestamp_utc = json_text(item, "timestamp_utc", "-");
			view.audit.push_back(std::move(row));
		}
	}

	view.truncated =
		control_plane.value("approval_queue_truncated", false) ||
		control_plane.value("audit_timeline_truncated", false) ||
		view.approval_total > view.approvals.size() ||
		view.audit_total > view.audit.size();
	return view;
}

inline std::string operator_approval_item_text(const OperatorApprovalItem& item) {
	return item.request_id + " | " + item.action + " | " + item.classification + " | " + item.status +
		" | actor " + item.actor + " | reason " + item.reason;
}

inline std::string operator_audit_item_text(const OperatorAuditItem& item) {
	return item.timestamp_utc + " | " + item.request_id + " | " + item.action + " | actor " + item.actor +
		" | outcome " + item.outcome + " | reason " + item.reason;
}

} // namespace sentum::ui
