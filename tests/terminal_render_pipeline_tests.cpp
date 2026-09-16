#include <sentum/ui/TerminalOperatorSurfaceRenderer.hpp>
#include <sentum/ui/TerminalRenderPipeline.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

void test_unchanged_frame_emits_zero_bytes() {
	const std::string frame = "header\nrow 1\nrow 2\n";
	const auto previous = sentum::ui::split_terminal_lines(frame);
	const auto diff = sentum::ui::build_terminal_frame_diff(previous, frame, false);
	require(diff.payload.empty(), "unchanged frame emitted terminal payload");
	require(diff.changed_rows == 0, "unchanged frame reported dirty rows");
	require(!diff.full_redraw, "unchanged frame unexpectedly forced full redraw");
}

void test_single_row_change_emits_one_row() {
	const auto previous = sentum::ui::split_terminal_lines("header\nprice 100\nstatus ok\n");
	const auto diff = sentum::ui::build_terminal_frame_diff(previous, "header\nprice 101\nstatus ok\n", false);
	require(diff.changed_rows == 1, "single-row change did not stay row-local");
	require(diff.payload.find("\x1b[2;1H") != std::string::npos, "changed row cursor address missing");
	require(diff.payload.find("price 101") != std::string::npos, "changed row content missing");
	require(diff.payload.find("status ok") == std::string::npos, "unchanged row was rewritten");
}

void test_removed_row_is_cleared() {
	const auto previous = sentum::ui::split_terminal_lines("header\nrow 1\nrow 2\n");
	const auto diff = sentum::ui::build_terminal_frame_diff(previous, "header\nrow 1\n", false);
	require(diff.changed_rows == 1, "removed row did not produce exactly one dirty row");
	require(diff.payload.find("\x1b[3;1H\x1b[2K") != std::string::npos, "removed row was not cleared");
}

void test_full_redraw_is_explicit() {
	const auto previous = sentum::ui::split_terminal_lines("old\n");
	const auto diff = sentum::ui::build_terminal_frame_diff(previous, "new\n", true);
	require(diff.full_redraw, "forced redraw flag was lost");
	require(diff.payload.rfind("\x1b[H\x1b[2J", 0) == 0, "full redraw did not reset terminal viewport");
}

void test_frame_pacer_prevents_drift_and_catchup_bursts() {
	using clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	sentum::ui::TerminalFramePacer pacer(100ms);
	const auto start = clock::time_point{};
	const auto first = pacer.next_deadline(start);
	require(first == start + 100ms, "initial frame deadline is incorrect");

	const auto second = pacer.next_deadline(start + 40ms);
	require(second == start + 200ms, "frame pacer accumulated render time as drift");

	const auto recovered = pacer.next_deadline(start + 450ms);
	require(recovered == start + 550ms, "late frame did not rebase without catch-up burst");
}

void test_operator_surface_preserves_zero_write_contract() {
	const nlohmann::json snapshot = {
		{"health", "healthy"},
		{"kill_switch_active", false},
		{"market_data_connected", true},
		{"entries_paused", false},
		{"performance", {{"queue_pressure", "normal"}}},
		{"operations_control_plane", {
			{"governance_state", "CONTROLLED"},
			{"maintenance_state", "NORMAL"},
			{"incident_state", "NONE"},
			{"recovery_state", "IDLE"},
			{"pending_approvals", 1},
			{"last_audit_action", "enter_maintenance"},
			{"last_audit_actor", "operator-a"},
			{"operator_action", {
				{"action", "resume_entries"},
				{"classification", "APPROVAL_REQUIRED"}
			}},
			{"maintenance_workflow", {
				{"state", "REQUESTED"},
				{"action", "enter_maintenance"},
				{"classification", "APPROVAL_REQUIRED"},
				{"request_id", "maint-17"},
				{"reason", "planned database maintenance"},
				{"actor", "operator-a"}
			}},
			{"incident_workflow", {
				{"state", "OPEN"},
				{"action", "acknowledge_incident"},
				{"classification", "APPROVAL_REQUIRED"},
				{"request_id", "inc-9"}
			}},
			{"recovery_workflow", {
				{"state", "CANDIDATE"},
				{"action", "promote_recovery_candidate"},
				{"classification", "APPROVAL_REQUIRED"},
				{"request_id", "rec-4"}
			}},
			{"approval_queue", nlohmann::json::array({
				{{"request_id", "req-1"}, {"action", "enter_maintenance"}, {"classification", "APPROVAL_REQUIRED"}, {"actor", "operator-a"}, {"reason", "planned maintenance"}}
			})},
			{"audit_timeline", nlohmann::json::array({
				{{"timestamp_utc", "2026-09-16T16:00:00Z"}, {"request_id", "req-0"}, {"action", "ack_incident"}, {"actor", "operator-c"}, {"reason", "investigating"}, {"outcome", "RECORDED"}}
			})}
		}}
	};

	const auto frame = sentum::ui::operator_surface_frame_lines(snapshot, "SYSTEM");
	require(frame.find("OPERATOR NORMAL") != std::string::npos, "operator banner missing from surface frame");
	require(frame.find("[7] SYSTEM*") != std::string::npos, "active workspace missing from surface frame");
	require(frame.find("Governance CONTROLLED") != std::string::npos, "governance state missing from surface frame");
	require(frame.find("1 approval(s) pending") != std::string::npos, "approval summary missing from surface frame");
	require(frame.find("Last action: enter_maintenance by operator-a") != std::string::npos, "audit summary missing from surface frame");
	require(frame.find("Action CONFIRM APPROVAL REQUEST | resume_entries") != std::string::npos, "approval-required action UX missing from surface frame");
	require(frame.find("MAINTENANCE | REQUESTED | enter_maintenance | APPROVAL_REQUIRED | request maint-17") != std::string::npos, "maintenance workflow missing from surface frame");
	require(frame.find("INCIDENT | OPEN | acknowledge_incident | APPROVAL_REQUIRED | request inc-9") != std::string::npos, "incident workflow missing from surface frame");
	require(frame.find("RECOVERY | CANDIDATE | promote_recovery_candidate | APPROVAL_REQUIRED | request rec-4") != std::string::npos, "recovery workflow missing from surface frame");
	require(frame.find("APPROVAL QUEUE 1") != std::string::npos, "approval queue heading missing from surface frame");
	require(frame.find("Approval req-1 | enter_maintenance | APPROVAL_REQUIRED | APPROVAL REQUIRED") != std::string::npos, "approval queue row missing from surface frame");
	require(frame.find("AUDIT TIMELINE 1") != std::string::npos, "audit timeline heading missing from surface frame");
	require(frame.find("Audit 2026-09-16T16:00:00Z | req-0 | ack_incident | actor operator-c | outcome RECORDED") != std::string::npos, "audit timeline row missing from surface frame");

	const auto previous = sentum::ui::split_terminal_lines(frame);
	const auto diff = sentum::ui::build_terminal_frame_diff(previous, frame, false);
	require(diff.payload.empty(), "unchanged operator surface emitted terminal payload");
	require(diff.changed_rows == 0, "unchanged operator surface reported dirty rows");
}

} // namespace

int main() {
	try {
		test_unchanged_frame_emits_zero_bytes();
		test_single_row_change_emits_one_row();
		test_removed_row_is_cleared();
		test_full_redraw_is_explicit();
		test_frame_pacer_prevents_drift_and_catchup_bursts();
		test_operator_surface_preserves_zero_write_contract();
		std::cout << "terminal render pipeline tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "terminal render pipeline test failure: " << error.what() << '\n';
		return 1;
	}
}
