#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace sentum::dashboard {

class DashboardServer {
public:
    explicit DashboardServer(std::uint16_t port = 8080);
    ~DashboardServer();

    void start();
    void stop() noexcept;
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    std::uint16_t port() const noexcept { return port_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint16_t port_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    void run();
};

} // namespace sentum::dashboard
