#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/market/IncrementalIndicators.hpp>
#include <sentum/trader/strategy/IStrategy.hpp>
#include <sentum/trader/strategy/MomentumStrategy.hpp>

namespace sentum::strategy {

struct StrategyDefinition {
    std::string type = "momentum";
    double weight = 1.0;
    nlohmann::json parameters = nlohmann::json::object();
};

struct AggregatedFrame {
    std::int64_t bucket = -1;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    bool ready = false;
};

class TimeframeAggregator {
public:
    explicit TimeframeAggregator(std::int64_t seconds) : seconds_(std::max<std::int64_t>(1, seconds)) {}

    bool push(const MarketEvent& event) {
        const double price = event.price > 0.0 ? event.price : event.close;
        if (!(price > 0.0)) return false;
        const auto epoch_seconds = std::chrono::duration_cast<std::chrono::seconds>(event.timestamp.time_since_epoch()).count();
        const std::int64_t bucket = epoch_seconds / seconds_;
        bool closed = false;
        if (current_.bucket >= 0 && bucket != current_.bucket) {
            latest_closed_ = current_;
            latest_closed_.ready = true;
            closed = true;
            current_ = {};
        }
        if (current_.bucket < 0) {
            current_.bucket = bucket;
            current_.open = current_.high = current_.low = current_.close = price;
        } else {
            current_.high = std::max(current_.high, price);
            current_.low = std::min(current_.low, price);
            current_.close = price;
        }
        current_.volume += event.volume;
        return closed;
    }

    const AggregatedFrame& latest_closed() const noexcept { return latest_closed_; }
    void reset() noexcept { current_ = {}; latest_closed_ = {}; }

private:
    std::int64_t seconds_;
    AggregatedFrame current_;
    AggregatedFrame latest_closed_;
};

class TrendStrategy final : public IStrategy {
public:
    TrendStrategy(std::size_t fast = 12, std::size_t slow = 26, double threshold = 0.001)
        : fast_period_(fast), slow_period_(slow), threshold_(threshold), fast_(fast), slow_(slow) {
        if (fast == 0 || slow <= fast) throw std::invalid_argument("trend strategy requires 0 < fast < slow");
    }

    StrategySignal on_price(double price, std::chrono::system_clock::time_point at) override {
        const double f = fast_.push(price), s = slow_.push(price);
        ++samples_;
        if (samples_ < slow_period_ || !(s > 0.0)) return {};
        const double spread = (f - s) / s;
        if (spread < threshold_) return {};
        const double confidence = std::clamp(spread / std::max(threshold_, 1e-9), 0.0, 2.0) / 2.0;
        return {TradeAction::BUY, name(), "fast EMA above slow EMA", price, at, confidence};
    }

    void reset() override { fast_ = sentum::market::Ema(fast_period_); slow_ = sentum::market::Ema(slow_period_); samples_ = 0; }
    std::string name() const override { return "trend"; }

private:
    std::size_t fast_period_, slow_period_, samples_ = 0;
    double threshold_;
    sentum::market::Ema fast_, slow_;
};

class MeanReversionStrategy final : public IStrategy {
public:
    MeanReversionStrategy(std::size_t period = 14, double oversold = 30.0)
        : period_(period), oversold_(oversold), rsi_(period) {
        if (!(oversold > 0.0 && oversold < 50.0)) throw std::invalid_argument("oversold must be between 0 and 50");
    }

    StrategySignal on_price(double price, std::chrono::system_clock::time_point at) override {
        const double rsi = rsi_.push(price);
        if (!rsi_.ready() || rsi > oversold_) return {};
        const double confidence = std::clamp((oversold_ - rsi) / std::max(oversold_, 1.0), 0.0, 1.0);
        return {TradeAction::BUY, name(), "RSI entered oversold region", price, at, confidence};
    }

    void reset() override { rsi_ = sentum::market::Rsi(period_); }
    std::string name() const override { return "mean_reversion"; }

private:
    std::size_t period_;
    double oversold_;
    sentum::market::Rsi rsi_;
};

class BreakoutStrategy final : public IStrategy {
public:
    BreakoutStrategy(std::size_t lookback = 20, double buffer = 0.001)
        : lookback_(std::max<std::size_t>(2, lookback)), buffer_(std::max(0.0, buffer)) {}

    StrategySignal on_price(double price, std::chrono::system_clock::time_point at) override {
        if (window_.size() < lookback_) { window_.push_back(price); return {}; }
        const double prior_high = *std::max_element(window_.begin(), window_.end());
        window_.pop_front();
        window_.push_back(price);
        if (price <= prior_high * (1.0 + buffer_)) return {};
        const double breakout = prior_high > 0.0 ? (price - prior_high) / prior_high : 0.0;
        return {TradeAction::BUY, name(), "price broke rolling high", price, at,
                std::clamp(breakout / std::max(buffer_, 1e-6), 0.0, 2.0) / 2.0};
    }

    void reset() override { window_.clear(); }
    std::string name() const override { return "breakout"; }

private:
    std::size_t lookback_;
    double buffer_;
    std::deque<double> window_;
};

class MultiTimeframeTrendStrategy final : public IStrategy {
public:
    MultiTimeframeTrendStrategy(std::int64_t fast_tf_seconds = 60, std::int64_t slow_tf_seconds = 300,
                                std::size_t ema_period = 8, double threshold = 0.001)
        : fast_tf_seconds_(fast_tf_seconds), slow_tf_seconds_(slow_tf_seconds), ema_period_(ema_period), threshold_(threshold),
          fast_frame_(fast_tf_seconds), slow_frame_(slow_tf_seconds), fast_ema_(ema_period), slow_ema_(ema_period) {
        if (slow_tf_seconds <= fast_tf_seconds) throw std::invalid_argument("slow timeframe must exceed fast timeframe");
    }

    StrategySignal on_price(double price, std::chrono::system_clock::time_point at) override {
        MarketEvent event; event.type = MarketEvent::Type::Trade; event.price = price; event.close = price; event.timestamp = at;
        return on_event(event);
    }

    StrategySignal on_event(const MarketEvent& event) override {
        if (fast_frame_.push(event)) { fast_value_ = fast_ema_.push(fast_frame_.latest_closed().close); ++fast_samples_; }
        if (slow_frame_.push(event)) { slow_value_ = slow_ema_.push(slow_frame_.latest_closed().close); ++slow_samples_; }
        const double price = event.price > 0.0 ? event.price : event.close;
        if (fast_samples_ < ema_period_ || slow_samples_ < ema_period_ || !(slow_value_ > 0.0)) return {};
        const double spread = (fast_value_ - slow_value_) / slow_value_;
        if (spread < threshold_) return {};
        return {TradeAction::BUY, name(), "fast timeframe trend confirmed by slow timeframe", price, event.timestamp,
                std::clamp(spread / std::max(threshold_, 1e-9), 0.0, 2.0) / 2.0};
    }

    void reset() override {
        fast_frame_.reset(); slow_frame_.reset();
        fast_ema_ = sentum::market::Ema(ema_period_); slow_ema_ = sentum::market::Ema(ema_period_);
        fast_samples_ = slow_samples_ = 0; fast_value_ = slow_value_ = 0.0;
    }
    std::string name() const override { return "multi_timeframe_trend"; }

private:
    std::int64_t fast_tf_seconds_, slow_tf_seconds_;
    std::size_t ema_period_;
    double threshold_;
    TimeframeAggregator fast_frame_, slow_frame_;
    sentum::market::Ema fast_ema_, slow_ema_;
    std::size_t fast_samples_ = 0, slow_samples_ = 0;
    double fast_value_ = 0.0, slow_value_ = 0.0;
};

class EnsembleStrategy final : public IStrategy {
public:
    explicit EnsembleStrategy(double threshold = 0.55) : threshold_(threshold) {}

    void add(std::unique_ptr<IStrategy> strategy, double weight) {
        if (!strategy || !(weight > 0.0)) throw std::invalid_argument("ensemble member requires positive weight");
        members_.push_back({std::move(strategy), weight});
    }

    StrategySignal on_price(double price, std::chrono::system_clock::time_point at) override {
        MarketEvent event; event.type = MarketEvent::Type::Trade; event.price = price; event.close = price; event.timestamp = at;
        return on_event(event);
    }

    StrategySignal on_event(const MarketEvent& event) override {
        double weighted = 0.0, total = 0.0;
        std::string reasons;
        for (auto& member : members_) {
            const auto signal = member.strategy->on_event(event);
            total += member.weight;
            if (signal.action == TradeAction::BUY) {
                const double confidence = signal.confidence > 0.0 ? signal.confidence : 1.0;
                weighted += member.weight * confidence;
                if (!reasons.empty()) reasons += "; ";
                reasons += signal.strategy;
            }
        }
        const double score = total > 0.0 ? weighted / total : 0.0;
        if (score < threshold_) return {};
        const double price = event.price > 0.0 ? event.price : event.close;
        return {TradeAction::BUY, name(), "ensemble confirmation: " + reasons, price, event.timestamp, std::clamp(score, 0.0, 1.0)};
    }

    void reset() override { for (auto& member : members_) member.strategy->reset(); }
    std::string name() const override { return "ensemble"; }

private:
    struct Member { std::unique_ptr<IStrategy> strategy; double weight; };
    double threshold_;
    std::vector<Member> members_;
};

class StrategyFactory {
public:
    static std::unique_ptr<IStrategy> create(const StrategyDefinition& definition) {
        const auto& p = definition.parameters;
        if (definition.type == "momentum")
            return std::make_unique<MomentumStrategy>(p.value("lookback", std::size_t{20}), p.value("entry_threshold", 0.001));
        if (definition.type == "trend")
            return std::make_unique<TrendStrategy>(p.value("fast_period", std::size_t{12}), p.value("slow_period", std::size_t{26}), p.value("threshold", 0.001));
        if (definition.type == "mean_reversion")
            return std::make_unique<MeanReversionStrategy>(p.value("period", std::size_t{14}), p.value("oversold", 30.0));
        if (definition.type == "breakout")
            return std::make_unique<BreakoutStrategy>(p.value("lookback", std::size_t{20}), p.value("buffer", 0.001));
        if (definition.type == "multi_timeframe_trend")
            return std::make_unique<MultiTimeframeTrendStrategy>(p.value("fast_timeframe_seconds", std::int64_t{60}),
                p.value("slow_timeframe_seconds", std::int64_t{300}), p.value("ema_period", std::size_t{8}), p.value("threshold", 0.001));
        throw std::runtime_error("Unsupported strategy type: " + definition.type);
    }

    static std::unique_ptr<IStrategy> create(const nlohmann::json& json) {
        if (!json.is_object()) throw std::runtime_error("strategy configuration must be an object");
        const std::string type = json.value("type", std::string("momentum"));
        if (type != "ensemble") {
            StrategyDefinition definition;
            definition.type = type;
            definition.weight = json.value("weight", 1.0);
            definition.parameters = json.value("parameters", nlohmann::json::object());
            return create(definition);
        }
        auto ensemble = std::make_unique<EnsembleStrategy>(json.value("threshold", 0.55));
        if (!json.contains("members") || !json.at("members").is_array() || json.at("members").empty())
            throw std::runtime_error("ensemble strategy requires members");
        for (const auto& member : json.at("members")) {
            const double weight = member.value("weight", 1.0);
            ensemble->add(create(member), weight);
        }
        return ensemble;
    }
};

} // namespace sentum::strategy
