#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include <sentum/trader/order/OrderTypes.hpp>
#include <sentum/api/BinanceEnvironment.hpp>
#include <sentum/trader/types/RiskConfig.hpp>
#include <sentum/trader/execution/ExecutionRecoveryState.hpp>

namespace sentum::execution {

class IExecutionVenue {
public:
    using UpdateHandler = std::function<void(const order::Snapshot&)>;
    using RecoveryHandler = std::function<void(const ExecutionRecoveryState&)>;
    using HealthHandler = std::function<void(bool)>;
    virtual ~IExecutionVenue() = default;
    virtual void start(UpdateHandler handler) = 0;
    virtual void stop() noexcept = 0;
    virtual order::Snapshot submit(const order::Request& request) = 0;
    virtual order::Snapshot cancel(const std::string& client_order_id) = 0;
    virtual void kill() noexcept = 0;
    virtual bool ready() const noexcept = 0;
    virtual bool killed() const noexcept = 0;
    virtual const char* name() const noexcept = 0;
    virtual void configure_market(const std::string&, const RiskConfig&) {}
    virtual void set_recovery_handler(RecoveryHandler) {}
    virtual void set_health_handler(HealthHandler) {}
    virtual bool user_stream_healthy() const noexcept { return ready(); }
    virtual bool reconciliation_complete() const noexcept { return ready(); }
    virtual bool resume_after_reconcile(const std::string&) { return false; }
    virtual std::optional<nlohmann::json> load_position_policy(const std::string&) const { return std::nullopt; }
    virtual void save_position_policy(const std::string&, const nlohmann::json&) {
        throw std::logic_error("Execution venue has no durable position policy store");
    }
    virtual api::BinanceEnvironment market_environment() const noexcept {
        return api::BinanceEnvironment::SpotTestnet;
    }
};

} // namespace sentum::execution
