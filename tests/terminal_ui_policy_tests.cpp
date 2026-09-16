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
        test_renderer_uses_zero_write_diff_contract();
        std::cout << "terminal UI policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "terminal UI policy test failure: " << error.what() << '\n';
        return 1;
    }
}
