#include <sentum/ui/TerminalUi.hpp>

#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace sentum::ui {

struct TerminalUiTestAccess {
    static void select_market(TerminalUi& ui) { ui.tab_ = TerminalUi::Tab::Market; }
    static void select_scanner(TerminalUi& ui) { ui.tab_ = TerminalUi::Tab::Scanner; }
    static void select_orders(TerminalUi& ui) { ui.tab_ = TerminalUi::Tab::Orders; }
    static void select_trades(TerminalUi& ui) { ui.tab_ = TerminalUi::Tab::Trades; }
    static void select_strategy(TerminalUi& ui) { ui.tab_ = TerminalUi::Tab::Strategy; }
    static void select_models(TerminalUi& ui) { ui.tab_ = TerminalUi::Tab::Models; }
    static void select_system(TerminalUi& ui) { ui.tab_ = TerminalUi::Tab::System; }

    static bool is_market(const TerminalUi& ui) { return ui.tab_ == TerminalUi::Tab::Market; }
    static bool is_scanner(const TerminalUi& ui) { return ui.tab_ == TerminalUi::Tab::Scanner; }
    static bool operator_navigation_active(const TerminalUi& ui) { return ui.operator_navigation_active_; }
    static const OperatorNavigationState& operator_navigation(const TerminalUi& ui) { return ui.operator_navigation_; }
    static void set_snapshot(TerminalUi& ui, nlohmann::json snapshot) { ui.cached_snapshot_ = std::move(snapshot); }
    static void key(TerminalUi& ui, char key) { ui.handle_key(key); }

    static bool uses_repository(const TerminalUi& ui) { return ui.active_tab_uses_repository(); }
    static bool repository_due(const TerminalUi& ui, std::chrono::steady_clock::time_point now) {
        return ui.repository_refresh_due(now);
    }
    static bool equity_due(const TerminalUi& ui, std::chrono::steady_clock::time_point now) {
        return ui.equity_sample_due(now);
    }
    static void mark_repository_refreshed(TerminalUi& ui, std::chrono::steady_clock::time_point when) {
        ui.last_repository_refresh_ = when;
    }
    static void mark_equity_sampled(TerminalUi& ui, std::chrono::steady_clock::time_point when) {
        ui.last_equity_sample_ = when;
    }
    static void render_frame(TerminalUi& ui, const std::string& frame, bool force_full) {
        ui.render_frame(frame, force_full);
    }
};

} // namespace sentum::ui

namespace {
using sentum::ui::TerminalUi;
using sentum::ui::TerminalUiTestAccess;
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void test_workspace_refresh_policy() {
    TerminalUi ui;
    const auto now = std::chrono::steady_clock::now();

    TerminalUiTestAccess::select_market(ui);
    require(!TerminalUiTestAccess::uses_repository(ui), "market workspace must not poll repository history");
    require(!TerminalUiTestAccess::repository_due(ui, now), "market workspace unexpectedly scheduled repository work");
    require(TerminalUiTestAccess::equity_due(ui, now), "market workspace must own equity sampling");
    TerminalUiTestAccess::mark_equity_sampled(ui, now);
    require(!TerminalUiTestAccess::equity_due(ui, now + 1s), "equity sampling ignored two-second cadence");
    require(TerminalUiTestAccess::equity_due(ui, now + 2s), "equity sampling did not become due after two seconds");

    TerminalUiTestAccess::select_scanner(ui);
    require(!TerminalUiTestAccess::uses_repository(ui), "scanner workspace must not poll repository history");
    require(!TerminalUiTestAccess::equity_due(ui, now + 3s), "scanner workspace unexpectedly sampled equity");

    TerminalUiTestAccess::select_orders(ui);
    require(TerminalUiTestAccess::uses_repository(ui), "orders workspace must use repository history");
    require(TerminalUiTestAccess::repository_due(ui, now), "orders workspace initial repository refresh not due");
    TerminalUiTestAccess::mark_repository_refreshed(ui, now);
    require(!TerminalUiTestAccess::repository_due(ui, now + 1s), "orders workspace ignored repository cadence");
    require(TerminalUiTestAccess::repository_due(ui, now + 2s), "orders workspace refresh did not become due");

    TerminalUiTestAccess::select_trades(ui);
    require(TerminalUiTestAccess::uses_repository(ui), "trades workspace must use repository history");
    require(!TerminalUiTestAccess::equity_due(ui, now + 3s), "trades workspace unexpectedly sampled equity");

    TerminalUiTestAccess::select_strategy(ui);
    require(!TerminalUiTestAccess::uses_repository(ui), "strategy workspace must not poll repository history");

    TerminalUiTestAccess::select_models(ui);
    require(TerminalUiTestAccess::uses_repository(ui), "models workspace must use repository history");

    TerminalUiTestAccess::select_system(ui);
    require(!TerminalUiTestAccess::uses_repository(ui), "system workspace must not poll repository history");
}

void test_modal_operator_input_suppresses_normal_hotkeys() {
    TerminalUi ui;
    TerminalUiTestAccess::select_market(ui);
    TerminalUiTestAccess::set_snapshot(ui, {
        {"operations_control_plane", {
            {"approval_queue", nlohmann::json::array({
                {
                    {"request_id", "req-1"},
                    {"action", "resume_entries"},
                    {"classification", "APPROVAL_REQUIRED"},
                    {"actor", "operator-a"},
                    {"reason", "recovery validated"}
                }
            })},
            {"audit_timeline", nlohmann::json::array()}
        }}
    });

    TerminalUiTestAccess::key(ui, 'o');
    require(TerminalUiTestAccess::operator_navigation_active(ui), "operator navigation did not activate");

    TerminalUiTestAccess::key(ui, '2');
    require(TerminalUiTestAccess::is_market(ui), "workspace hotkey escaped operator navigation mode");

    TerminalUiTestAccess::key(ui, '\n');
    const auto& confirmation = TerminalUiTestAccess::operator_navigation(ui);
    require(confirmation.confirmation_open, "approval confirmation did not open");
    require(!confirmation.execution_authorized, "approval confirmation authorized execution");

    TerminalUiTestAccess::key(ui, '\n');
    require(!TerminalUiTestAccess::operator_navigation(ui).execution_authorized, "second Enter authorized execution");

    TerminalUiTestAccess::key(ui, 27);
    require(!TerminalUiTestAccess::operator_navigation_active(ui), "Esc did not leave operator navigation mode");
    require(!TerminalUiTestAccess::operator_navigation(ui).execution_authorized, "leaving operator navigation authorized execution");

    TerminalUiTestAccess::key(ui, '2');
    require(TerminalUiTestAccess::is_scanner(ui), "workspace hotkey did not resume after operator navigation closed");
}

void test_renderer_uses_zero_write_diff_contract() {
    TerminalUi ui;
    std::ostringstream captured;
    auto* previous_buffer = std::cout.rdbuf(captured.rdbuf());

    TerminalUiTestAccess::render_frame(ui, "header\nvalue\n", true);
    const auto full_size = captured.str().size();
    require(full_size > 0, "forced terminal frame did not emit output");

    TerminalUiTestAccess::render_frame(ui, "header\nvalue\n", false);
    const auto unchanged_size = captured.str().size();
    require(unchanged_size == full_size, "unchanged terminal frame emitted output");

    TerminalUiTestAccess::render_frame(ui, "header\nchanged\n", false);
    const auto changed_size = captured.str().size();
    require(changed_size > unchanged_size, "changed terminal row did not emit output");

    std::cout.rdbuf(previous_buffer);
}

} // namespace

int main() {
    try {
        test_workspace_refresh_policy();
        test_modal_operator_input_suppresses_normal_hotkeys();
        test_renderer_uses_zero_write_diff_contract();
        std::cout << "terminal UI policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "terminal UI policy test failure: " << error.what() << '\n';
        return 1;
    }
}
