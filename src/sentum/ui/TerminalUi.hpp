#pragma once

#include <atomic>
#include <chrono>
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

    std::chrono::milliseconds refresh_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

bool stdout_is_terminal() noexcept;

} // namespace sentum::ui
