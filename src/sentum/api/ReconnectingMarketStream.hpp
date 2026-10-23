#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace sentum::api {

struct MarketStreamSettings {
    std::chrono::milliseconds initial_backoff{250};
    std::chrono::milliseconds maximum_backoff{10000};
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds maximum_event_age{2000};
    std::chrono::milliseconds idle_reconnect_timeout{10000};
};

struct MarketStreamHealth {
    bool worker_running = false;
    bool connected = false;
    bool fresh = false;
    std::int64_t last_event_age_ms = -1;
    std::int64_t last_event_time_ms = 0;
    std::uint64_t connection_attempts = 0;
    std::uint64_t successful_connections = 0;
    std::uint64_t reconnects = 0;
    std::string state = "stopped";

    nlohmann::json json() const {
        return {{"worker_running", worker_running}, {"connected", connected}, {"fresh", fresh},
                {"last_event_age_ms", last_event_age_ms}, {"last_event_time_ms", last_event_time_ms},
                {"connection_attempts", connection_attempts}, {"successful_connections", successful_connections},
                {"reconnects", reconnects}, {"state", state}};
    }
};

// One owning worker calls run(); cancellation and health reads are thread-safe.
// The owner must stop and join that worker before destroying this object.
// Each connection attempt owns a fresh endpoint, so cancelled handlers cannot
// carry connection state into the next attempt or a later worker lifetime.
class ReconnectingMarketStream {
public:
    using MessageHandler = std::function<void(const std::string&)>;
    using HealthHandler = std::function<void(const MarketStreamHealth&)>;

    ReconnectingMarketStream(std::string url, std::string hostname, MarketStreamSettings settings,
                             MessageHandler on_message, HealthHandler on_health = {});
    ~ReconnectingMarketStream();
    ReconnectingMarketStream(const ReconnectingMarketStream&) = delete;
    ReconnectingMarketStream& operator=(const ReconnectingMarketStream&) = delete;

    void run();
    void stop() noexcept;
    bool accept_event(std::int64_t event_time_ms) noexcept;
    MarketStreamHealth health() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sentum::api
