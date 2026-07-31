#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace sentum::observability {

struct RuntimeMetrics {
    std::atomic<std::uint64_t> rest_errors{0};
    std::atomic<std::uint64_t> websocket_reconnects{0};
    std::atomic<std::uint64_t> reconciliation_runs{0};
    std::atomic<std::uint64_t> reconciliation_failures{0};
    std::atomic<std::uint64_t> orders_submitted{0};
    std::atomic<std::uint64_t> orders_rejected{0};
    std::atomic<std::uint64_t> orders_filled{0};
    std::atomic<std::uint64_t> orders_cancelled{0};
    std::atomic<std::uint64_t> database_write_errors{0};
    std::chrono::steady_clock::time_point started_at=std::chrono::steady_clock::now();

    nlohmann::json snapshot(std::int64_t market_age_ms,std::int64_t user_age_ms,const std::string&kill_reason) const {
        const auto uptime=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-started_at).count();
        return {{"market_event_age_ms",market_age_ms},{"user_event_age_ms",user_age_ms},{"rest_error_count",rest_errors.load()},
            {"websocket_reconnect_count",websocket_reconnects.load()},{"reconciliation_count",reconciliation_runs.load()},
            {"reconciliation_failures",reconciliation_failures.load()},{"orders_submitted",orders_submitted.load()},
            {"orders_rejected",orders_rejected.load()},{"orders_filled",orders_filled.load()},{"orders_cancelled",orders_cancelled.load()},
            {"database_write_errors",database_write_errors.load()},{"kill_switch_reason",kill_reason},{"runtime_uptime_seconds",uptime}};
    }
};

} // namespace sentum::observability
