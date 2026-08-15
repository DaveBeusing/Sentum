#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

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
    void loop();
    void draw();
    void poll_input();
    void handle_key(char key);
    void cycle_strategy();

    std::chrono::milliseconds refresh_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    bool editing_symbol_ = false;
    std::string symbol_buffer_;
    std::string notice_;
};

bool stdout_is_terminal() noexcept;

} // namespace sentum::ui
