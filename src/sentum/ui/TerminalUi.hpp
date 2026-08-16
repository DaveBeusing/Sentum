#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace sentum::ui {

class TerminalUi {
public:
    explicit TerminalUi(std::chrono::milliseconds refresh = std::chrono::milliseconds(500));
    ~TerminalUi();
    TerminalUi(const TerminalUi&) = delete;
    TerminalUi& operator=(const TerminalUi&) = delete;

    void start();
    void stop();
    bool running() const noexcept { return running_.load(std::memory_order_relaxed); }

private:
    enum class Tab { Market = 0, Scanner, Orders, Trades, Strategy, Models, System };

    void loop();
    void draw();
    void poll_input();
    void handle_key(char key);
    void cycle_strategy();
    void refresh_repository_data(const nlohmann::json& snapshot);
    void sample_equity(const nlohmann::json& snapshot);

    std::chrono::milliseconds refresh_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    Tab tab_ = Tab::Market;
    bool editing_symbol_ = false;
    std::string symbol_buffer_;
    std::string notice_;
    std::chrono::steady_clock::time_point last_repository_refresh_{};
    std::chrono::steady_clock::time_point last_equity_sample_{};
    std::string repository_db_path_;
    nlohmann::json recent_trades_ = nlohmann::json::array();
    nlohmann::json recent_orders_ = nlohmann::json::array();
    nlohmann::json models_ = nlohmann::json::array();
    std::deque<double> equity_history_;
};

bool stdout_is_terminal() noexcept;

} // namespace sentum::ui
