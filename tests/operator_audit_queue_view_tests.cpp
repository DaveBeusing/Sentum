#include <sentum/ui/OperatorAuditQueueView.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

nlohmann::json base_snapshot() {
	return {
		{"operations_control_plane", {
			{"approval_queue", nlohmann::json::array({
				{{"request_id", "req-1"}, {"action", "enter_maintenance"}, {"classification", "APPROVAL_REQUIRED"}, {"actor", "operator-a"}, {"reason", "planned maintenance"}},
				{{"request_id", "req-2"}, {"action", "synthesize_fill"}, {"classification", "FORBIDDEN"}, {"actor", "operator-b"}, {"reason", "test"}}
			})},
			{"audit_timeline", nlohmann::json::array({
				{{"timestamp_utc", "2026-09-16T16:00:00Z"}, {"request_id", "req-0"}, {"action", "ack_incident"}, {"actor", "operator-c"}, {"reason", "investigating"}, {"outcome", "RECORDED"}}
			})}
		}}
	};
}

void test_approval_queue_projection() {
	const auto view = sentum::ui::derive_operator_audit_queue_view(base_snapshot());
	require(view.approval_total == 2, "approval total mismatch");
	require(view.approvals.size() == 2, "approval projection size mismatch");
	require(view.approvals[0].classification == "APPROVAL_REQUIRED", "approval class lost");
	require(view.approvals[0].status == "APPROVAL REQUIRED", "approval status mismatch");
	require(!view.approvals[0].execution_authorized, "approval row authorized execution");
	require(view.approvals[1].classification == "FORBIDDEN", "forbidden class lost");
	require(view.approvals[1].status == "BLOCKED", "forbidden row not blocked");
}

void test_missing_classification_fails_closed() {
	auto snapshot = base_snapshot();
	snapshot["operations_control_plane"]["approval_queue"][0].erase("classification");
	const auto view = sentum::ui::derive_operator_audit_queue_view(snapshot);
	require(view.approvals[0].classification == "FORBIDDEN", "missing classification did not fail closed");
	require(view.approvals[0].status == "BLOCKED", "missing classification was not blocked");
}

void test_audit_projection_is_read_only_context() {
	const auto view = sentum::ui::derive_operator_audit_queue_view(base_snapshot());
	require(view.audit_total == 1, "audit total mismatch");
	require(view.audit.size() == 1, "audit projection size mismatch");
	require(view.audit[0].request_id == "req-0", "audit request id mismatch");
	require(view.audit[0].outcome == "RECORDED", "audit outcome mismatch");
}

void test_projection_is_bounded() {
	auto snapshot = base_snapshot();
	for (int i = 0; i < 20; ++i) {
		snapshot["operations_control_plane"]["approval_queue"].push_back({
			{"request_id", "extra-" + std::to_string(i)},
			{"action", "enter_maintenance"},
			{"classification", "APPROVAL_REQUIRED"}
		});
	}
	const auto view = sentum::ui::derive_operator_audit_queue_view(snapshot, 3, 1);
	require(view.approvals.size() == 3, "approval limit ignored");
	require(view.audit.size() == 1, "audit limit ignored");
	require(view.truncated, "bounded projection did not report truncation");
}

void test_empty_evidence_stays_empty() {
	const auto view = sentum::ui::derive_operator_audit_queue_view(nlohmann::json::object());
	require(view.approval_total == 0 && view.audit_total == 0, "empty evidence invented rows");
	require(view.approvals.empty() && view.audit.empty(), "empty evidence projected rows");
}

} // namespace

int main() {
	try {
		test_approval_queue_projection();
		test_missing_classification_fails_closed();
		test_audit_projection_is_read_only_context();
		test_projection_is_bounded();
		test_empty_evidence_stays_empty();
		std::cout << "operator audit queue view tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "operator audit queue view test failure: " << error.what() << '\n';
		return 1;
	}
}
