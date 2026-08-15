#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace sentum::dashboard {

class DashboardServer {
public:
    DashboardServer();
    explicit DashboardServer(std::uint16_t port);
    DashboardServer(std::string host, std::uint16_t port);
    ~DashboardServer();

    void start();
    void stop() noexcept;
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    const std::string& host() const noexcept { return host_; }
    std::uint16_t port() const noexcept { return port_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string host_;
    std::uint16_t port_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    void run();
};

} // namespace sentum::dashboard
