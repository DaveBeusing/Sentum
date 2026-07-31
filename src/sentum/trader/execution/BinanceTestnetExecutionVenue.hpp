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

    void start(UpdateHandler handler) override {
        session_->set_update_handler(std::move(handler));
        session_->start();
        ready_ = true;
    }

    void stop() noexcept override {
        ready_ = false;
        if (session_) session_->stop();
    }

    order::Snapshot submit(const order::Request& request) override {
        if (!ready()) throw std::logic_error("Testnet execution venue is not ready");
        return session_->submit(request);
    }

    order::Snapshot cancel(const std::string& client_order_id) override {
        return session_->cancel(client_order_id);
    }

    void kill() noexcept override { if (session_) session_->kill(); }
    bool ready() const noexcept override { return ready_ && session_ && !session_->killed(); }
    bool killed() const noexcept override { return !session_ || session_->killed(); }
    const char* name() const noexcept override { return "binance-spot-testnet"; }

private:
    std::unique_ptr<order::LiveOrderSession> session_;
    bool ready_ = false;
};

} // namespace sentum::execution
