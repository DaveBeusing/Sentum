#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>

#include <sentum/trader/execution/IExecutionVenue.hpp>

namespace sentum::execution {

class SimulatedExecutionVenue final : public IExecutionVenue {
public:
    explicit SimulatedExecutionVenue(std::string name = "paper") : name_(std::move(name)) {}
    void start(UpdateHandler handler) override { handler_ = std::move(handler); ready_.store(true); killed_.store(false); }
    void stop() noexcept override { ready_.store(false); }
    order::Snapshot submit(const order::Request& request) override {
        if (!ready() || killed()) throw std::logic_error("Simulated venue is not ready");
        order::Snapshot s; s.symbol=request.symbol; s.client_order_id=request.client_order_id; s.exchange_order_id=++next_id_; s.side=request.side; s.state=order::State::Filled; s.requested_quantity=request.quantity; s.executed_quantity=request.quantity; s.average_fill_price=last_price_; s.cumulative_quote_quantity=request.quantity*last_price_; s.updated_at=std::chrono::system_clock::now();
        if (handler_) handler_(s); return s;
    }
    order::Snapshot cancel(const std::string&) override { throw std::logic_error("Simulated market orders fill immediately"); }
    void kill() noexcept override { killed_.store(true); ready_.store(false); }
    bool ready() const noexcept override { return ready_.load(); }
    bool killed() const noexcept override { return killed_.load(); }
    const char* name() const noexcept override { return name_.c_str(); }
    void set_market_price(double price) noexcept { last_price_=price; }
private:
    std::string name_; UpdateHandler handler_; std::atomic<bool> ready_{false}; std::atomic<bool> killed_{false}; std::int64_t next_id_=0; double last_price_=0.0;
};

} // namespace sentum::execution
