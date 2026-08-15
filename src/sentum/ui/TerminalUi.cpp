#include <sentum/ui/TerminalUi.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/core/RuntimeControl.hpp>
#include <sentum/dashboard/DashboardState.hpp>

#if defined(_WIN32)
#include <conio.h>
#include <io.h>
#else
#include <sys/select.h>
#include <termios.h>
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

#if !defined(_WIN32)
termios saved_termios{};
bool raw_enabled = false;
void enable_raw_input() {
    if (!::isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_termios) != 0) return;
    termios raw = saved_termios;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) raw_enabled = true;
}
void restore_input() { if (raw_enabled) { tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios); raw_enabled = false; } }
#endif

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

void latency_line(std::ostringstream& out, const nlohmann::json& perf, const char* key, const char* label) {
    if (!perf.contains(key) || !perf[key].is_object()) return;
    const auto& p = perf[key];
    out << "  " << std::left << std::setw(20) << label
        << " p50 " << std::setw(8) << number<double>(p, "p50_us")
        << " p95 " << std::setw(8) << number<double>(p, "p95_us")
        << " p99 " << std::setw(8) << number<double>(p, "p99_us") << " us\n";
}

nlohmann::json preset(const std::string& type) {
    if (type == "trend") return {{"type","trend"},{"parameters",{{"fast_period",12},{"slow_period",26},{"threshold",0.001}}}};
    if (type == "mean_reversion") return {{"type","mean_reversion"},{"parameters",{{"period",14},{"oversold",30.0}}}};
    if (type == "breakout") return {{"type","breakout"},{"parameters",{{"lookback",20},{"buffer",0.001}}}};
    if (type == "multi_timeframe_trend") return {{"type","multi_timeframe_trend"},{"parameters",{{"fast_timeframe_seconds",60},{"slow_timeframe_seconds",300},{"ema_period",8},{"threshold",0.001}}}};
    if (type == "ensemble") return {{"type","ensemble"},{"threshold",0.55},{"members",nlohmann::json::array({
        {{"type","momentum"},{"weight",1.0},{"parameters",{{"lookback",20},{"entry_threshold",0.001}}}},
        {{"type","trend"},{"weight",1.0},{"parameters",{{"fast_period",12},{"slow_period",26},{"threshold",0.001}}}}
    })}};
    return {{"type","momentum"},{"parameters",{{"lookback",20},{"entry_threshold",0.001}}}};
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
#if !defined(_WIN32)
    enable_raw_input();
#endif
    std::cout << hide_cursor << std::flush;
    thread_ = std::thread(&TerminalUi::loop, this);
}

void TerminalUi::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
#if !defined(_WIN32)
    restore_input();
#endif
    std::cout << show_cursor << reset << '\n' << std::flush;
}

void TerminalUi::loop() {
    while (running_.load(std::memory_order_relaxed)) {
        poll_input();
        draw();
        std::this_thread::sleep_for(refresh_);
    }
}

void TerminalUi::poll_input() {
    while (true) {
        char key = 0;
#if defined(_WIN32)
        if (!_kbhit()) break;
        key = static_cast<char>(_getch());
#else
        fd_set set; FD_ZERO(&set); FD_SET(STDIN_FILENO, &set);
        timeval timeout{0, 0};
        const int ready = select(STDIN_FILENO + 1, &set, nullptr, nullptr, &timeout);
        if (ready <= 0) break;
        const auto read_count = ::read(STDIN_FILENO, &key, 1);
        if (read_count != 1) break;
#endif
        handle_key(key);
    }
}

void TerminalUi::handle_key(char key) {
    auto& control = sentum::runtime::RuntimeControl::global();
    if (editing_symbol_) {
        if (key == '\r' || key == '\n') {
            if (!symbol_buffer_.empty()) {
                std::transform(symbol_buffer_.begin(), symbol_buffer_.end(), symbol_buffer_.begin(), [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
                control.set_manual_symbol(symbol_buffer_);
                notice_ = "Manual symbol requested: " + symbol_buffer_ + " (applies between positions)";
            }
            symbol_buffer_.clear(); editing_symbol_ = false; return;
        }
        if (key == 27) { symbol_buffer_.clear(); editing_symbol_ = false; notice_ = "Manual symbol edit cancelled"; return; }
        if (key == 8 || key == 127) { if (!symbol_buffer_.empty()) symbol_buffer_.pop_back(); return; }
        if (std::isalnum(static_cast<unsigned char>(key)) && symbol_buffer_.size() < 20) symbol_buffer_.push_back(key);
        return;
    }

    switch (static_cast<char>(std::tolower(static_cast<unsigned char>(key)))) {
        case 'p': control.pause_entries(!control.entries_paused()); notice_ = control.entries_paused() ? "New entries paused; open position remains managed" : "New entries resumed"; break;
        case 'c': control.request_manual_close(); notice_ = "Manual close requested through simulated execution"; break;
        case 'a': control.set_auto_symbol(true); notice_ = "Scanner auto-selection requested"; break;
        case 'm': editing_symbol_ = true; symbol_buffer_.clear(); notice_ = "Enter symbol and press Enter; Esc cancels"; break;
        case 's': cycle_strategy(); break;
        default: break;
    }
}

void TerminalUi::cycle_strategy() {
    static const std::vector<std::string> types{"momentum","trend","mean_reversion","breakout","multi_timeframe_trend","ensemble"};
    const auto current = sentum::runtime::RuntimeControl::global().strategy().value("type", std::string("momentum"));
    auto it = std::find(types.begin(), types.end(), current);
    const auto index = it == types.end() ? 0u : (static_cast<std::size_t>(std::distance(types.begin(), it)) + 1u) % types.size();
    sentum::runtime::RuntimeControl::global().set_strategy(preset(types[index]));
    notice_ = "Strategy requested: " + types[index] + " (applies between positions)";
}

void TerminalUi::draw() {
    const auto snapshot = sentum::dashboard::DashboardState::global().snapshot();
    const auto mode = text(snapshot, "mode", "idle");
    const auto status = text(snapshot, "health", "starting");

    std::ostringstream out;
    out << clear << bold << "SENTUM" << reset << "  " << dim << "Interactive Paper Trading Console" << reset << "\n";
    out << "Mode: " << bold << mode << reset << "   Health: " << health(status)
        << "   Symbol: " << text(snapshot, "current_symbol", text(snapshot, "symbol", "-")) << "\n";
    out << "Strategy: " << bold << text(snapshot, "strategy_name", "-") << reset
        << "   Symbol mode: " << text(snapshot, "symbol_mode", "auto")
        << "   Entries: " << (number<bool>(snapshot, "entries_paused") ? std::string(yellow)+"PAUSED"+reset : std::string(green)+"ENABLED"+reset) << "\n\n";

    out << bold << "Paper Account" << reset << '\n'
        << "  Equity: " << std::fixed << std::setprecision(2) << number<double>(snapshot, "balance") << ' ' << text(snapshot, "quote_asset", "USDC")
        << "   Realized P/L: " << number<double>(snapshot.value("paper_account", nlohmann::json::object()), "realized_profit") << '\n';

    out << bold << "Services" << reset << '\n'
        << "  Collector  " << state(number<bool>(snapshot, "collector_active"))
        << "   Scanner  " << state(number<bool>(snapshot, "scanner_active"))
        << "   Trader  " << state(number<bool>(snapshot, "trader_active")) << '\n';

    out << bold << "Market" << reset << '\n'
        << "  Markets: " << number<std::size_t>(snapshot, "markets")
        << "   Events/s: " << std::fixed << std::setprecision(1) << number<double>(snapshot, "events_per_second")
        << "   Queue: " << number<std::size_t>(snapshot, "queue_depth")
        << "   Drop: " << std::setprecision(4) << number<double>(snapshot, "drop_rate") * 100.0 << "%\n"
        << "  Top: " << text(snapshot, "top_asset") << "  Return: " << std::setprecision(2) << number<double>(snapshot, "top_return_percent") << "%\n";

    out << bold << "Signal / Risk" << reset << '\n'
        << "  Signal: " << text(snapshot, "last_signal") << "  Confidence: " << std::setprecision(3) << number<double>(snapshot, "signal_confidence")
        << "  Risk: " << text(snapshot, "last_risk_decision") << '\n'
        << "  Signal reason: " << text(snapshot, "signal_reason") << '\n'
        << "  Risk reason:   " << text(snapshot, "risk_reason") << '\n';

    if (snapshot.contains("risk_config") && snapshot["risk_config"].is_object()) {
        const auto& r = snapshot["risk_config"];
        out << bold << "Risk" << reset << '\n'
            << "  Capital " << number<double>(r,"max_total_capital") << "  Risk/trade " << number<double>(r,"risk_per_trade")*100.0 << "%"
            << "  SL " << number<double>(r,"stop_loss_percent")*100.0 << "%  TP " << number<double>(r,"take_profit_percent")*100.0 << "%"
            << "  Max hold " << number<long long>(r,"max_holding_seconds") << "s\n";
    }

    out << bold << "Trading" << reset << '\n'
        << "  Trades: " << number<int>(snapshot, "total_trades") << "   W/L: " << number<int>(snapshot, "wins") << '/' << number<int>(snapshot, "losses")
        << "   Win rate: " << std::setprecision(2) << number<double>(snapshot, "win_rate") << "%   P/L: " << number<double>(snapshot, "total_profit") << '\n';

    if (snapshot.contains("active_position") && snapshot["active_position"].is_object()) {
        const auto& p = snapshot["active_position"];
        out << "  Position: " << text(p, "symbol") << "  entry " << number<double>(p, "entry_price")
            << "  last " << number<double>(p, "current_price") << "  unrealized " << number<double>(p, "unrealized_profit")
            << "  SL " << number<double>(p,"stop_loss") << "  TP " << number<double>(p,"take_profit") << '\n';
    }

    if (snapshot.contains("performance") && snapshot["performance"].is_object()) {
        out << bold << "Latency" << reset << '\n';
        const auto& perf = snapshot["performance"];
        latency_line(out, perf, "parse_latency", "Parser");
        latency_line(out, perf, "event_dispatch_latency", "Event dispatch");
        latency_line(out, perf, "strategy_decision_latency", "Decision");
        latency_line(out, perf, "sqlite_batch_latency", "SQLite batch");
    }

    out << '\n' << bold << "Controls" << reset << "  [S] Strategy  [A] Auto symbol  [M] Manual symbol  [P] Pause/Resume entries  [C] Close paper position\n";
    if (editing_symbol_) out << yellow << "Manual symbol> " << symbol_buffer_ << "_" << reset << '\n';
    if (!notice_.empty()) out << dim << notice_ << reset << '\n';
    if (snapshot.contains("control_pending") && !snapshot["control_pending"].is_null()) out << yellow << "Pending control: " << snapshot["control_pending"].dump() << reset << '\n';
    out << dim << "Strategy/symbol changes are applied only between positions. Ctrl+C stops Sentum." << reset;
    std::cout << out.str() << std::flush;
}

} // namespace sentum::ui
