#include <sentum/ui/TerminalUi.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/core/RuntimeControl.hpp>
#include <sentum/dashboard/DashboardRepository.hpp>
#include <sentum/dashboard/DashboardState.hpp>

#if defined(_WIN32)
#include <conio.h>
#include <io.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace sentum::ui {
namespace {
constexpr const char* clear_screen = "\x1b[2J\x1b[H";
constexpr const char* cursor_home = "\x1b[H";
constexpr const char* enter_alt_screen = "\x1b[?1049h";
constexpr const char* leave_alt_screen = "\x1b[?1049l";
constexpr const char* hide_cursor = "\x1b[?25l";
constexpr const char* show_cursor = "\x1b[?25h";
constexpr const char* bold = "\x1b[1m";
constexpr const char* dim = "\x1b[2m";
constexpr const char* green = "\x1b[32m";
constexpr const char* yellow = "\x1b[33m";
constexpr const char* red = "\x1b[31m";
constexpr const char* cyan = "\x1b[36m";
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
void restore_input() {
    if (raw_enabled) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        raw_enabled = false;
    }
}
#endif

int terminal_width() {
#if defined(_WIN32)
    return 120;
#else
    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) return std::max(80, static_cast<int>(size.ws_col));
    return 120;
#endif
}

std::vector<std::string> split_lines(const std::string& frame) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < frame.size()) {
        const auto end = frame.find('\n', start);
        if (end == std::string::npos) {
            lines.emplace_back(frame.substr(start));
            break;
        }
        lines.emplace_back(frame.substr(start, end - start));
        start = end + 1;
    }
    if (!frame.empty() && frame.back() == '\n') lines.emplace_back();
    return lines;
}

std::string text(const nlohmann::json& j, const char* key, const std::string& fallback = "-") {
    if (!j.is_object() || !j.contains(key) || j[key].is_null()) return fallback;
    if (j[key].is_string()) return j[key].get<std::string>();
    return j[key].dump();
}

template <typename T>
T number(const nlohmann::json& j, const char* key, T fallback = T{}) {
    try { return j.is_object() && j.contains(key) ? j[key].get<T>() : fallback; }
    catch (...) { return fallback; }
}

std::string clip(std::string value, std::size_t width) {
    if (value.size() <= width) return value;
    if (width <= 3) return value.substr(0, width);
    return value.substr(0, width - 3) + "...";
}

std::string format_number(double value, int precision = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string format_pct(double value, int precision = 2) {
    std::ostringstream out;
    if (value > 0.0) out << '+';
    out << std::fixed << std::setprecision(precision) << value << '%';
    return out.str();
}

std::string colored_value(double value, const std::string& formatted) {
    if (value > 0.0) return std::string(green) + formatted + reset;
    if (value < 0.0) return std::string(red) + formatted + reset;
    return formatted;
}

std::string health(const std::string& value) {
    const char* color = value == "healthy" ? green : (value == "starting" ? yellow : red);
    return std::string(color) + value + reset;
}

std::string on_off(bool active) {
    return active ? std::string(green) + "ON" + reset : std::string(red) + "OFF" + reset;
}

std::string timestamp_text(std::int64_t epoch_ms) {
    if (epoch_ms <= 0) return "-";
    const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &tm);
    return buffer;
}

std::string sparkline(const std::deque<double>& values, std::size_t width = 40) {
    if (values.empty()) return std::string(width, '-');
    const std::size_t count = std::min(width, values.size());
    auto begin = values.end() - static_cast<std::ptrdiff_t>(count);
    const auto [min_it, max_it] = std::minmax_element(begin, values.end());
    const double lo = *min_it, hi = *max_it;
    static constexpr char levels[] = ".:-=+*#@";
    std::string out;
    out.reserve(count);
    for (auto it = begin; it != values.end(); ++it) {
        const double normalized = hi > lo ? (*it - lo) / (hi - lo) : 0.5;
        const auto index = static_cast<std::size_t>(std::clamp(normalized, 0.0, 1.0) * 7.0);
        out.push_back(levels[index]);
    }
    return out;
}

std::string bar(double value, std::size_t width = 18) {
    const double v = std::clamp(value, 0.0, 1.0);
    const std::size_t filled = static_cast<std::size_t>(std::round(v * static_cast<double>(width)));
    return std::string(filled, '#') + std::string(width - filled, '.');
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

void separator(std::ostringstream& out, int width) {
    out << dim << std::string(static_cast<std::size_t>(std::max(20, width)), '-') << reset << '\n';
}

void title(std::ostringstream& out, const std::string& value) {
    out << bold << cyan << value << reset << '\n';
}

void latency_row(std::ostringstream& out, const nlohmann::json& perf, const char* key, const char* label) {
    if (!perf.contains(key) || !perf[key].is_object()) return;
    const auto& p = perf[key];
    out << "  " << std::left << std::setw(18) << label
        << std::right << std::setw(10) << format_number(number<double>(p, "p50_us"), 1)
        << std::setw(10) << format_number(number<double>(p, "p95_us"), 1)
        << std::setw(10) << format_number(number<double>(p, "p99_us"), 1)
        << std::setw(10) << format_number(number<double>(p, "max_us"), 1) << '\n';
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
    previous_lines_.clear();
    force_full_redraw_ = true;
    ui_dirty_ = true;
    last_terminal_width_ = terminal_width();
    last_dashboard_generation_ = 0;
    std::cout << enter_alt_screen << clear_screen << hide_cursor << std::flush;
    thread_ = std::thread(&TerminalUi::loop, this);
}

void TerminalUi::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
#if !defined(_WIN32)
    restore_input();
#endif
    std::cout << show_cursor << reset << leave_alt_screen << std::flush;
}

void TerminalUi::loop() {
    while (running_.load(std::memory_order_relaxed)) {
        poll_input();

        auto& dashboard = sentum::dashboard::DashboardState::global();
        const auto generation = dashboard.generation();
        const int width = terminal_width();
        const bool resized = width != last_terminal_width_;
        const auto now = std::chrono::steady_clock::now();
        const bool repository_due = last_repository_refresh_ == std::chrono::steady_clock::time_point{} ||
                                    now - last_repository_refresh_ >= std::chrono::seconds(2);
        const bool equity_due = last_equity_sample_ == std::chrono::steady_clock::time_point{} ||
                                now - last_equity_sample_ >= std::chrono::seconds(2);
        const bool state_changed = generation != last_dashboard_generation_;

        if (ui_dirty_ || resized || state_changed || repository_due || equity_due) {
            draw(force_full_redraw_ || resized);
            last_dashboard_generation_ = dashboard.generation();
            last_terminal_width_ = width;
            ui_dirty_ = false;
            force_full_redraw_ = false;
        }

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
            symbol_buffer_.clear(); editing_symbol_ = false; ui_dirty_ = true; return;
        }
        if (key == 27) { symbol_buffer_.clear(); editing_symbol_ = false; notice_ = "Manual symbol edit cancelled"; ui_dirty_ = true; return; }
        if (key == 8 || key == 127) { if (!symbol_buffer_.empty()) symbol_buffer_.pop_back(); ui_dirty_ = true; return; }
        if (std::isalnum(static_cast<unsigned char>(key)) && symbol_buffer_.size() < 20) { symbol_buffer_.push_back(key); ui_dirty_ = true; }
        return;
    }

    if (key >= '1' && key <= '7') {
        tab_ = static_cast<Tab>(key - '1');
        notice_.clear();
        ui_dirty_ = true;
        force_full_redraw_ = true;
        return;
    }

    switch (static_cast<char>(std::tolower(static_cast<unsigned char>(key)))) {
        case 'p': control.pause_entries(!control.entries_paused()); notice_ = control.entries_paused() ? "New entries PAUSED; open position remains managed" : "New entries RESUMED"; ui_dirty_ = true; break;
        case 'c': control.request_manual_close(); notice_ = "Manual close requested through simulated execution"; ui_dirty_ = true; break;
        case 'a': control.set_auto_symbol(true); notice_ = "Scanner auto-selection requested"; ui_dirty_ = true; break;
        case 'm': editing_symbol_ = true; symbol_buffer_.clear(); notice_ = "Enter symbol and press Enter; Esc cancels"; ui_dirty_ = true; break;
        case 's': cycle_strategy(); ui_dirty_ = true; break;
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

void TerminalUi::refresh_repository_data(const nlohmann::json& snapshot) {
    const auto now = std::chrono::steady_clock::now();
    if (last_repository_refresh_ != std::chrono::steady_clock::time_point{} && now - last_repository_refresh_ < std::chrono::seconds(2)) return;
    repository_db_path_ = text(snapshot, "db_path", "log/klines.sqlite3");
    sentum::dashboard::DashboardRepository repository(repository_db_path_);
    recent_trades_ = repository.recent_trades(12);
    recent_orders_ = repository.recent_orders(12);
    models_ = repository.models(12);
    last_repository_refresh_ = now;
}

void TerminalUi::sample_equity(const nlohmann::json& snapshot) {
    const auto now = std::chrono::steady_clock::now();
    if (last_equity_sample_ != std::chrono::steady_clock::time_point{} && now - last_equity_sample_ < std::chrono::seconds(2)) return;
    equity_history_.push_back(number<double>(snapshot, "balance"));
    while (equity_history_.size() > 120) equity_history_.pop_front();
    last_equity_sample_ = now;
}

void TerminalUi::render_frame(const std::string& frame, bool force_full) {
    auto current_lines = split_lines(frame);
    std::ostringstream terminal;

    if (force_full || previous_lines_.empty()) {
        terminal << cursor_home << "\x1b[2J" << frame;
    } else {
        const std::size_t rows = std::max(previous_lines_.size(), current_lines.size());
        for (std::size_t i = 0; i < rows; ++i) {
            const std::string current = i < current_lines.size() ? current_lines[i] : std::string{};
            const std::string previous = i < previous_lines_.size() ? previous_lines_[i] : std::string{};
            if (current == previous) continue;
            terminal << "\x1b[" << (i + 1) << ";1H\x1b[2K" << current << reset;
        }
    }

    const auto payload = terminal.str();
    if (!payload.empty()) {
        std::cout.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        std::cout.flush();
    }
    previous_lines_ = std::move(current_lines);
}

void TerminalUi::draw(bool force_full) {
    const auto snapshot = sentum::dashboard::DashboardState::global().snapshot();
    refresh_repository_data(snapshot);
    sample_equity(snapshot);

    const int width = terminal_width();
    const auto mode = text(snapshot, "mode", "idle");
    const auto status = text(snapshot, "health", "starting");
    const auto symbol = text(snapshot, "current_symbol", text(snapshot, "symbol", "-"));
    const auto strategy = text(snapshot, "strategy_name", "-");
    const bool paused = number<bool>(snapshot, "entries_paused");
    const double equity = number<double>(snapshot, "balance");
    const double realized = number<double>(snapshot.value("paper_account", nlohmann::json::object()), "realized_profit");
    const double total_profit = number<double>(snapshot, "total_profit");

    double last_price = 0.0, unrealized = 0.0;
    const bool has_position = snapshot.contains("active_position") && snapshot["active_position"].is_object();
    if (has_position) {
        last_price = number<double>(snapshot["active_position"], "current_price");
        unrealized = number<double>(snapshot["active_position"], "unrealized_profit");
    }

    std::ostringstream out;
    out << bold << "SENTUM" << reset << "  " << cyan << mode << reset
        << "  " << health(status) << "  " << bold << symbol << reset;
    if (last_price > 0.0) out << "  " << format_number(last_price, last_price < 10.0 ? 5 : 2);
    out << "  Strategy " << bold << strategy << reset
        << "  Entries " << (paused ? std::string(yellow)+"PAUSED"+reset : std::string(green)+"RUNNING"+reset) << '\n';

    out << "Equity " << format_number(equity) << ' ' << text(snapshot, "quote_asset", "USDC")
        << "  Realized " << colored_value(realized, format_number(realized))
        << "  Runtime P/L " << colored_value(total_profit, format_number(total_profit));
    if (has_position) out << "  Unrealized " << colored_value(unrealized, format_number(unrealized));
    out << "  Market " << on_off(number<bool>(snapshot, "market_data_connected")) << '\n';
    separator(out, width);

    static const char* tab_names[] = {"MARKET","SCANNER","ORDERS","TRADES","STRATEGY","MODELS","SYSTEM"};
    for (int i = 0; i < 7; ++i) {
        const bool selected = static_cast<int>(tab_) == i;
        out << (selected ? std::string(bold)+cyan : std::string(dim)) << '[' << (i + 1) << "] " << tab_names[i] << reset;
        if (i != 6) out << "   ";
    }
    out << '\n';
    separator(out, width);

    const auto scanner = snapshot.value("scanner", nlohmann::json::array());
    const auto risk = snapshot.value("risk_config", nlohmann::json::object());
    const auto strategy_config = snapshot.value("strategy_config", nlohmann::json::object());

    if (tab_ == Tab::Market) {
        title(out, "MARKET / POSITION / RISK");
        const double spread = number<double>(risk, "spread_percent");
        const double indicative_bid = last_price > 0.0 ? last_price * (1.0 - spread * 0.5) : 0.0;
        const double indicative_ask = last_price > 0.0 ? last_price * (1.0 + spread * 0.5) : 0.0;
        out << "  Symbol " << std::left << std::setw(12) << symbol
            << "Last " << std::right << std::setw(14) << (last_price > 0.0 ? format_number(last_price, last_price < 10.0 ? 5 : 2) : "-")
            << "   Fill-model Bid/Ask " << (indicative_bid > 0.0 ? format_number(indicative_bid, 2) + " / " + format_number(indicative_ask, 2) : "-") << '\n';

        if (has_position) {
            const auto& p = snapshot["active_position"];
            out << "  " << green << bold << "LONG" << reset
                << "   Entry " << format_number(number<double>(p,"entry_price"),2)
                << "   Qty " << format_number(number<double>(p,"quantity"),6)
                << "   U-P/L " << colored_value(unrealized, format_number(unrealized))
                << "   SL " << format_number(number<double>(p,"stop_loss"),2)
                << "   TP " << format_number(number<double>(p,"take_profit"),2) << '\n';
        } else {
            out << "  " << dim << "FLAT - no open paper position" << reset << '\n';
        }

        out << "  Risk/trade " << format_pct(number<double>(risk,"risk_per_trade") * 100.0)
            << "   Stop " << format_pct(number<double>(risk,"stop_loss_percent") * 100.0)
            << "   Target " << format_pct(number<double>(risk,"take_profit_percent") * 100.0)
            << "   Max hold " << number<long long>(risk,"max_holding_seconds") << "s"
            << "   Kill switch " << (number<bool>(snapshot,"kill_switch_active") ? std::string(red)+"ON"+reset : std::string(green)+"OFF"+reset) << '\n';

        out << "  Equity curve  " << cyan << sparkline(equity_history_, static_cast<std::size_t>(std::min(60, width - 18))) << reset << '\n';
        separator(out, width);
        title(out, "MARKET WATCH");
        out << "  " << std::left << std::setw(14) << "Symbol" << std::right << std::setw(12) << "1m Return" << std::setw(12) << "Rank" << std::setw(16) << "State" << '\n';
        std::size_t rank = 1;
        for (const auto& row : scanner) {
            const double r = number<double>(row,"return") * 100.0;
            const std::string row_symbol = text(row,"symbol");
            out << "  " << std::left << std::setw(14) << row_symbol << std::right << std::setw(12) << format_pct(r)
                << std::setw(12) << rank++ << std::setw(16) << (row_symbol == symbol ? "TRADING" : "WATCH") << '\n';
        }
        if (scanner.empty()) out << "  " << dim << "Waiting for scanner data..." << reset << '\n';

        separator(out, width);
        title(out, "RECENT TRADES");
        out << "  " << std::left << std::setw(10) << "Time" << std::setw(12) << "Symbol" << std::setw(16) << "Strategy"
            << std::right << std::setw(12) << "Entry" << std::setw(12) << "Exit" << std::setw(12) << "P/L" << "  Exit reason\n";
        int shown = 0;
        for (const auto& row : recent_trades_) {
            if (shown++ >= 5) break;
            const double pnl = number<double>(row,"net_profit");
            out << "  " << std::left << std::setw(10) << timestamp_text(number<std::int64_t>(row,"exit_ts"))
                << std::setw(12) << text(row,"symbol") << std::setw(16) << clip(text(row,"strategy"),15)
                << std::right << std::setw(12) << format_number(number<double>(row,"entry_price"),2)
                << std::setw(12) << format_number(number<double>(row,"exit_price"),2)
                << std::setw(12) << format_number(pnl,2) << "  " << clip(text(row,"exit_reason"),22) << '\n';
        }
        if (recent_trades_.empty()) out << "  " << dim << "No completed trades yet." << reset << '\n';
    }

    if (tab_ == Tab::Scanner) {
        title(out, "SCANNER / WATCHLIST");
        out << "  Markets " << number<std::size_t>(snapshot,"markets")
            << "   Events/s " << format_number(number<double>(snapshot,"events_per_second"),1)
            << "   Leader " << text(snapshot,"top_asset")
            << "   Leader return " << format_pct(number<double>(snapshot,"top_return_percent")) << "\n\n";
        out << "  " << std::left << std::setw(6) << "Rank" << std::setw(16) << "Symbol" << std::right << std::setw(14) << "1m Return" << std::setw(16) << "Role" << '\n';
        std::size_t rank = 1;
        for (const auto& row : scanner) {
            const auto s = text(row,"symbol");
            const double r = number<double>(row,"return") * 100.0;
            out << "  " << std::left << std::setw(6) << rank++ << std::setw(16) << s
                << std::right << std::setw(14) << format_pct(r) << std::setw(16) << (s == symbol ? "ACTIVE" : "CANDIDATE") << '\n';
        }
        if (scanner.empty()) out << "  Waiting for closed candles and ranking data.\n";
        out << "\n  " << dim << "Scanner ranking is market-performance based; strategy approval happens only inside TradeEngine." << reset << '\n';
    }

    if (tab_ == Tab::Orders) {
        title(out, "ORDER / EXECUTION HISTORY");
        out << "  " << std::left << std::setw(10) << "Time" << std::setw(13) << "Symbol" << std::setw(8) << "Side"
            << std::setw(18) << "State" << std::right << std::setw(14) << "Requested" << std::setw(14) << "Executed" << std::setw(14) << "Fill" << "  Source\n";
        for (const auto& row : recent_orders_) {
            out << "  " << std::left << std::setw(10) << timestamp_text(number<std::int64_t>(row,"local_ts"))
                << std::setw(13) << text(row,"symbol") << std::setw(8) << text(row,"side") << std::setw(18) << clip(text(row,"state"),17)
                << std::right << std::setw(14) << format_number(number<double>(row,"requested_quantity"),6)
                << std::setw(14) << format_number(number<double>(row,"executed_quantity"),6)
                << std::setw(14) << format_number(number<double>(row,"average_fill_price"),2) << "  " << clip(text(row,"source"),18) << '\n';
        }
        if (recent_orders_.empty()) out << "  " << dim << "No persisted order events." << reset << '\n';
    }

    if (tab_ == Tab::Trades) {
        title(out, "TRADE HISTORY / P&L");
        out << "  Total " << number<int>(snapshot,"total_trades") << "   Wins " << number<int>(snapshot,"wins")
            << "   Losses " << number<int>(snapshot,"losses") << "   Win rate " << format_pct(number<double>(snapshot,"win_rate"))
            << "   Avg P/L " << format_number(number<double>(snapshot,"average_profit")) << "\n\n";
        out << "  " << std::left << std::setw(10) << "Time" << std::setw(13) << "Symbol" << std::setw(18) << "Strategy"
            << std::right << std::setw(13) << "Entry" << std::setw(13) << "Exit" << std::setw(13) << "Fees" << std::setw(13) << "Net P/L" << "  Reason\n";
        for (const auto& row : recent_trades_) {
            out << "  " << std::left << std::setw(10) << timestamp_text(number<std::int64_t>(row,"exit_ts"))
                << std::setw(13) << text(row,"symbol") << std::setw(18) << clip(text(row,"strategy"),17)
                << std::right << std::setw(13) << format_number(number<double>(row,"entry_price"),2)
                << std::setw(13) << format_number(number<double>(row,"exit_price"),2)
                << std::setw(13) << format_number(number<double>(row,"fees"),2)
                << std::setw(13) << format_number(number<double>(row,"net_profit"),2) << "  " << clip(text(row,"exit_reason"),20) << '\n';
        }
        if (recent_trades_.empty()) out << "  " << dim << "No completed trades yet." << reset << '\n';
        out << "\n  Equity  " << cyan << sparkline(equity_history_, static_cast<std::size_t>(std::min(70, width - 12))) << reset << '\n';
    }

    if (tab_ == Tab::Strategy) {
        title(out, "STRATEGY DECISION MATRIX");
        const double confidence = number<double>(snapshot,"signal_confidence");
        const auto signal_reason = text(snapshot,"signal_reason");
        out << "  Active strategy  " << bold << strategy << reset << '\n'
            << "  Last signal      " << text(snapshot,"last_signal") << '\n'
            << "  Aggregate score  " << format_number(confidence,3) << "  [" << bar(confidence) << "]\n"
            << "  Risk decision    " << text(snapshot,"last_risk_decision") << '\n'
            << "  Signal reason    " << signal_reason << '\n'
            << "  Risk reason      " << text(snapshot,"risk_reason") << "\n\n";

        if (strategy_config.value("type", std::string{}) == "ensemble" && strategy_config.contains("members")) {
            out << "  " << std::left << std::setw(24) << "Member" << std::setw(12) << "Weight" << std::setw(16) << "Last confirm" << '\n';
            for (const auto& member : strategy_config["members"]) {
                const auto member_name = member.value("type", std::string("unknown"));
                const bool confirmed = signal_reason.find(member_name) != std::string::npos;
                out << "  " << std::left << std::setw(24) << member_name << std::setw(12) << format_number(member.value("weight",1.0),2)
                    << std::setw(16) << (confirmed ? "YES" : "-") << '\n';
            }
            out << "\n  " << dim << "Member confirmation is derived from the actual ensemble signal reason; aggregate confidence is the engine score." << reset << '\n';
        } else {
            out << "  Configuration: " << strategy_config.dump() << '\n';
        }
        out << "\n  Controls: [S] cycle preset   [P] pause/resume entries   [A] scanner symbol   [M] manual symbol\n";
    }

    if (tab_ == Tab::Models) {
        title(out, "MODEL LIFECYCLE");
        out << "  " << std::left << std::setw(24) << "Model" << std::setw(13) << "Symbol" << std::setw(13) << "Stage" << "Promotion path\n";
        for (const auto& row : models_) {
            const auto stage = text(row,"stage");
            std::string path = "research -> shadow -> paper -> testnet";
            out << "  " << std::left << std::setw(24) << clip(text(row,"name",text(row,"model_id")),23)
                << std::setw(13) << text(row,"symbol") << std::setw(13) << stage << path << '\n';
        }
        if (models_.empty()) out << "  " << dim << "No registered models in log/models.sqlite3." << reset << '\n';
        out << "\n  Active paper model: " << text(snapshot,"paper_model_id","-") << '\n';
        out << "  " << dim << "Promotion remains CLI-controlled and is intentionally not writable from the TUI." << reset << '\n';
    }

    if (tab_ == Tab::System) {
        title(out, "SYSTEM / LATENCY / DATA PATH");
        out << "  Collector " << on_off(number<bool>(snapshot,"collector_active"))
            << "   Scanner " << on_off(number<bool>(snapshot,"scanner_active"))
            << "   Trader " << on_off(number<bool>(snapshot,"trader_active"))
            << "   Queue " << number<std::size_t>(snapshot,"queue_depth")
            << "   Drop " << format_pct(number<double>(snapshot,"drop_rate") * 100.0,4)
            << "   Events/s " << format_number(number<double>(snapshot,"events_per_second"),1) << '\n';
        out << "  DB " << text(snapshot,"db_path") << "   Size " << format_number(number<double>(snapshot,"db_size_bytes") / 1024.0 / 1024.0,2) << " MiB\n\n";
        out << "  " << std::left << std::setw(18) << "Pipeline" << std::right << std::setw(10) << "p50 us" << std::setw(10) << "p95 us" << std::setw(10) << "p99 us" << std::setw(10) << "max us" << '\n';
        const auto perf = snapshot.value("performance", nlohmann::json::object());
        latency_row(out, perf, "parse_latency", "Parser");
        latency_row(out, perf, "event_dispatch_latency", "Event dispatch");
        latency_row(out, perf, "strategy_decision_latency", "Decision");
        latency_row(out, perf, "sqlite_batch_latency", "SQLite batch");
        out << "\n  Dashboard bind: " << text(snapshot,"dashboard_host","-") << ':' << number<int>(snapshot,"dashboard_port") << '\n';
    }

    separator(out, width);
    out << bold << "TRADING CONTROLS" << reset
        << "  [S] Strategy  [A] Auto  [M] Manual symbol  [P] Pause/Resume  [C] Close paper position  [Ctrl+C] Quit\n";
    if (editing_symbol_) out << yellow << "Manual symbol> " << symbol_buffer_ << "_" << reset << '\n';
    if (!notice_.empty()) out << dim << notice_ << reset << '\n';
    if (snapshot.contains("control_pending") && !snapshot["control_pending"].is_null()) out << yellow << "Pending: " << snapshot["control_pending"].dump() << reset << '\n';

    render_frame(out.str(), force_full);
}

} // namespace sentum::ui
