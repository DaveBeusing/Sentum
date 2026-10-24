#pragma once

#include <memory>
#include <stdexcept>
#include <utility>

#include <sentum/trader/execution/IExecutionVenue.hpp>
#include <sentum/trader/order/LiveOrderSession.hpp>

namespace sentum::execution {

class BinanceTestnetExecutionVenue final : public IExecutionVenue {
public:
    BinanceTestnetExecutionVenue() : session_(order::LiveOrderSession::from_environment()) {}
    explicit BinanceTestnetExecutionVenue(std::unique_ptr<order::LiveOrderSession> session)
        : session_(std::move(session)) {
        if (!session_) throw std::invalid_argument("Testnet execution requires a session");
    }

    void configure_market(const std::string& symbol, const RiskConfig& risk) override { session_->configure_market(symbol, risk); }
    void set_recovery_handler(RecoveryHandler handler) override { session_->set_recovery_handler(std::move(handler)); }
    void set_health_handler(HealthHandler handler) override { session_->set_health_handler(std::move(handler)); }
    bool user_stream_healthy() const noexcept override { return session_ && session_->user_stream_healthy(); }
    bool reconciliation_complete() const noexcept override { return session_ && session_->reconciliation_complete(); }
    bool resume_after_reconcile(const std::string& confirmation) override { return session_->resume_after_reconcile(confirmation); }
    std::optional<nlohmann::json> load_position_policy(const std::string& symbol) const override {
        return session_->load_position_policy(symbol);
    }
    void save_position_policy(const std::string& symbol, const nlohmann::json& checkpoint) override {
        session_->save_position_policy(symbol, checkpoint);
    }
    api::BinanceEnvironment market_environment() const noexcept override { return BinanceSpotExecutionClient::environment(); }

    void start(UpdateHandler handler) override {
        session_->set_update_handler(std::move(handler));
        session_->start();
    }

    void stop() noexcept override { if (session_) session_->stop(); }

    order::Snapshot submit(const order::Request& request) override {
        if (!ready()) throw std::logic_error("Testnet execution venue is not reconciled and ready");
        return session_->submit(request);
    }

    order::Snapshot cancel(const std::string& client_order_id) override {
        return session_->cancel(client_order_id);
    }

    void kill() noexcept override { if (session_) session_->kill(); }
    bool ready() const noexcept override { return session_ && session_->ready(); }
    bool killed() const noexcept override { return !session_ || session_->killed(); }
    const char* name() const noexcept override { return "binance-spot-testnet"; }

private:
    std::unique_ptr<order::LiveOrderSession> session_;
};

} // namespace sentum::execution
