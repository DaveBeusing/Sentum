#include <sentum/ui/TerminalUi.hpp>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <sentum/dashboard/DashboardState.hpp>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace sentum::ui {
namespace {
constexpr const char* clear = "\x1b[2J\x1b[H";
constexpr const char* hide_cursor = "\x1b[?25l";
constexpr const char* show_cursor = "\x1b[?25h";
constexpr const char* bold = "\x1b[1m";
constexpr const char* dim = "\x1b[2m";
constexpr const char* green = "\x1b[32m";
constexpr const char* yellow = "\x1b[33m";
constexpr const char* red = "\x1b[31m";
constexpr const char* reset = "\x1b[0m";

std::string text(const nlohmann::json& j, const char* key, const std::string& fallback = "-") {
    if (!j.contains(key) || j[key].is_null()) return fallback;
    if (j[key].is_string()) return j[key].get<std::string>();
    return j[key].dump();
}

template <typename T>
T number(const nlohmann::json& j, const char* key, T fallback = T{}) {
    try { return j.contains(key) ? j[key].get<T>() : fallback; } catch (...) { return fallback; }
}

std::string state(bool active) { return active ? std::string(green) + "ACTIVE" + reset : std::string(red) + "STOPPED" + reset; }
std::string health(const std::string& value) {
    const char* color = value == "healthy" ? green : (value == "starting" ? yellow : red);
    return std::string(color) + value + reset;
}

void latency_line(const nlohmann::json& perf, const char* key, const char* label) {
    if (!perf.contains(key) || !perf[key].is_object()) return;
    const auto& p = perf[key];
    std::cout << "  " << std::left << std::setw(20) << label
              << " p50 " << std::setw(8) << number<double>(p, "p50_us")
              << " p95 " << std::setw(8) << number<double>(p, "p95_us")
              << " p99 " << std::setw(8) << number<double>(p, "p99_us") << " us\n";
}
} // namespace

bool stdout_is_terminal() noexcept {
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

TerminalUi::TerminalUi(std::chrono::milliseconds refresh) : refresh_(refresh) {}
TerminalUi::~TerminalUi() { stop(); }

void TerminalUi::start() {
    if (running_.exchange(true)) return;
    std::cout << hide_cursor << std::flush;
    thread_ = std::thread(&TerminalUi::loop, this);
}

void TerminalUi::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    std::cout << show_cursor << reset << '\n' << std::flush;
}

void TerminalUi::loop() {
    while (running_.load(std::memory_order_relaxed)) {
        draw();
        std::this_thread::sleep_for(refresh_);
    }
}

void TerminalUi::draw() {
    const auto snapshot = sentum::dashboard::DashboardState::global().snapshot();
    const auto mode = text(snapshot, "mode", "idle");
    const auto status = text(snapshot, "health", "starting");

    std::ostringstream out;
    out << clear << bold << "SENTUM" << reset << "  " << dim << "Unified Runtime Console" << reset << "\n";
    out << "Mode: " << bold << mode << reset << "   Health: " << health(status)
        << "   Symbol: " << text(snapshot, "current_symbol", text(snapshot, "symbol", "-")) << "\n\n";

    out << bold << "Services" << reset << '\n'
        << "  Collector  " << state(number<bool>(snapshot, "collector_active"))
        << "   Scanner  " << state(number<bool>(snapshot, "scanner_active"))
        << "   Trader  " << state(number<bool>(snapshot, "trader_active")) << '\n';

    out << bold << "Market" << reset << '\n'
        << "  Markets: " << number<std::size_t>(snapshot, "markets")
        << "   Events/s: " << std::fixed << std::setprecision(1) << number<double>(snapshot, "events_per_second")
        << "   Queue: " << number<std::size_t>(snapshot, "queue_depth")
        << "   Drop rate: " << std::setprecision(4) << number<double>(snapshot, "drop_rate") * 100.0 << "%\n"
        << "  Top asset: " << text(snapshot, "top_asset") << "   Return: " << std::setprecision(2)
        << number<double>(snapshot, "top_return_percent") << "%   DB: " << number<std::size_t>(snapshot, "db_size_bytes") / 1024 << " KiB\n";

    out << bold << "Trading" << reset << '\n'
        << "  Trades: " << number<int>(snapshot, "total_trades")
        << "   W/L: " << number<int>(snapshot, "wins") << '/' << number<int>(snapshot, "losses")
        << "   Win rate: " << std::setprecision(2) << number<double>(snapshot, "win_rate") << "%"
        << "   P/L: " << number<double>(snapshot, "total_profit") << '\n';

    if (snapshot.contains("active_position") && snapshot["active_position"].is_object()) {
        const auto& p = snapshot["active_position"];
        out << "  Position: " << text(p, "symbol") << "  entry " << number<double>(p, "entry_price")
            << "  last " << number<double>(p, "current_price") << "  unrealized " << number<double>(p, "unrealized_profit") << '\n';
    }

    if (snapshot.contains("performance") && snapshot["performance"].is_object()) {
        out << bold << "Latency" << reset << '\n';
        std::cout << out.str();
        const auto& perf = snapshot["performance"];
        latency_line(perf, "parse_latency", "Parser");
        latency_line(perf, "event_dispatch_latency", "Event dispatch");
        latency_line(perf, "strategy_decision_latency", "Decision");
        latency_line(perf, "sqlite_batch_latency", "SQLite batch");
        std::cout << '\n' << dim << "Ctrl+C to stop. Web dashboard uses the same runtime state." << reset << std::flush;
        return;
    }
    out << '\n' << dim << "Ctrl+C to stop. Web dashboard uses the same runtime state." << reset;
    std::cout << out.str() << std::flush;
}

} // namespace sentum::ui
