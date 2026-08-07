#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>

#include <sentum/trader/execution/IExecutionVenue.hpp>

namespace sentum::execution {

class SimulatedExecutionVenue final : public IExecutionVenue {
public:
    explicit SimulatedExecutionVenue(std::string name = "paper") : name_(std::move(name)) {}

    void start(UpdateHandler handler) override {
        handler_ = std::move(handler);
        killed_.store(false, std::memory_order_relaxed);
        ready_.store(true, std::memory_order_release);
    }

    void stop() noexcept override { ready_.store(false, std::memory_order_release); }

    order::Snapshot submit(const order::Request& request) override {
        if (!ready() || killed()) throw std::logic_error("Simulated venue is not ready");
        if (request.quantity <= 0.0 || market_price_ <= 0.0) throw std::invalid_argument("Invalid simulated order");

        const double half_spread = spread_percent_ * 0.5;
        const bool buy = request.side == order::Side::Buy;
        const double touch = buy ? market_price_ * (1.0 + half_spread)
                                 : market_price_ * (1.0 - half_spread);
        const double fill = buy ? touch * (1.0 + slippage_percent_)
                                : touch * (1.0 - slippage_percent_);

        order::Snapshot s;
        s.symbol = request.symbol;
        s.client_order_id = request.client_order_id;
        s.exchange_order_id = next_id_.fetch_add(1, std::memory_order_relaxed) + 1;
        s.side = request.side;
        s.state = order::State::Filled;
        s.requested_quantity = request.quantity;
        s.executed_quantity = request.quantity;
        s.average_fill_price = fill;
        s.cumulative_quote_quantity = request.quantity * fill;
        s.updated_at = market_time_;
        if (handler_) handler_(s);
        return s;
    }

    order::Snapshot cancel(const std::string&) override {
        throw std::logic_error("Simulated market orders fill immediately");
    }

    void kill() noexcept override {
        killed_.store(true, std::memory_order_release);
        ready_.store(false, std::memory_order_release);
    }

    bool ready() const noexcept override { return ready_.load(std::memory_order_acquire); }
    bool killed() const noexcept override { return killed_.load(std::memory_order_acquire); }
    const char* name() const noexcept override { return name_.c_str(); }

    void set_market(double price, std::chrono::system_clock::time_point timestamp) noexcept {
        market_price_ = price;
        market_time_ = timestamp;
    }

    void set_fill_model(double spread_percent, double slippage_percent) noexcept {
        spread_percent_ = std::max(0.0, spread_percent);
        slippage_percent_ = std::max(0.0, slippage_percent);
    }

private:
    std::string name_;
    UpdateHandler handler_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> killed_{false};
    std::atomic<std::int64_t> next_id_{0};
    double market_price_ = 0.0;
    double spread_percent_ = 0.0;
    double slippage_percent_ = 0.0;
    std::chrono::system_clock::time_point market_time_{};
};

} // namespace sentum::execution
