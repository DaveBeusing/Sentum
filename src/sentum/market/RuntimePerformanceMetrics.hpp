#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace sentum::market {

enum class QueuePressureLevel : std::uint8_t {
    Normal = 0,
    Elevated = 1,
    Critical = 2,
    Saturated = 3
};

inline const char* queue_pressure_name(QueuePressureLevel level) noexcept {
    switch (level) {
    case QueuePressureLevel::Normal: return "normal";
    case QueuePressureLevel::Elevated: return "elevated";
    case QueuePressureLevel::Critical: return "critical";
    case QueuePressureLevel::Saturated: return "saturated";
    }
    return "unknown";
}

class LatencyHistogram {
public:
    void observe(std::uint64_t microseconds) noexcept {
        count_.fetch_add(1, std::memory_order_relaxed);
        total_.fetch_add(microseconds, std::memory_order_relaxed);
        auto current = max_.load(std::memory_order_relaxed);
        while (microseconds > current && !max_.compare_exchange_weak(current, microseconds, std::memory_order_relaxed)) {}
        buckets_[bucket_for(microseconds)].fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }

    nlohmann::json snapshot() const {
        const auto count = count_.load(std::memory_order_relaxed);
        return {{"count",count},{"avg_us",count ? static_cast<double>(total_.load(std::memory_order_relaxed))/count : 0.0},
                {"p50_us",percentile(0.50)},{"p95_us",percentile(0.95)},{"p99_us",percentile(0.99)},
                {"max_us",max_.load(std::memory_order_relaxed)}};
    }

private:
    static constexpr std::array<std::uint64_t,16> limits_{1,2,4,8,16,32,64,128,256,512,1000,2000,4000,8000,16000,32000};
    static std::size_t bucket_for(std::uint64_t value) noexcept {
        for (std::size_t i=0;i<limits_.size();++i) if(value<=limits_[i]) return i;
        return limits_.size();
    }
    std::uint64_t percentile(double p) const noexcept {
        const auto count=count_.load(std::memory_order_relaxed); if(!count) return 0;
        const auto target=static_cast<std::uint64_t>(static_cast<double>(count)*p + 0.999999);
        std::uint64_t seen=0;
        for(std::size_t i=0;i<buckets_.size();++i){seen+=buckets_[i].load(std::memory_order_relaxed);if(seen>=target)return i<limits_.size()?limits_[i]:max_.load(std::memory_order_relaxed);}
        return max_.load(std::memory_order_relaxed);
    }
    std::array<std::atomic<std::uint64_t>,17> buckets_{};
    std::atomic<std::uint64_t> count_{0}, total_{0}, max_{0};
};

class LatencySampler {
public:
    explicit constexpr LatencySampler(std::uint32_t sample_every = 1) noexcept
        : sample_every_(sample_every == 0 ? 1 : sample_every) {}

    bool should_sample() noexcept {
        ++counter_;
        if (sample_every_ <= 1) return true;
        return (counter_ % sample_every_) == 0;
    }

    std::uint32_t sample_every() const noexcept { return sample_every_; }

private:
    const std::uint32_t sample_every_;
    std::uint32_t counter_ = 0;
};

class RuntimePerformanceMetrics {
public:
    static RuntimePerformanceMetrics& global(){static RuntimePerformanceMetrics instance;return instance;}
    LatencyHistogram parse_latency;
    LatencyHistogram event_dispatch_latency;
    LatencyHistogram strategy_decision_latency;
    LatencyHistogram sqlite_batch_latency;
    std::atomic<std::uint64_t> market_events{0};
    std::atomic<std::uint64_t> queue_depth{0};
    std::atomic<std::uint64_t> queue_high_water{0};
    std::atomic<std::uint64_t> queue_pressure_transitions{0};
    std::atomic<std::uint64_t> queue_saturation_events{0};
    std::atomic<std::uint64_t> queue_wakeups{0};
    std::atomic<std::uint8_t> queue_pressure_level{static_cast<std::uint8_t>(QueuePressureLevel::Normal)};

    void observe_queue_depth(std::uint64_t depth) noexcept {
        queue_depth.store(depth, std::memory_order_relaxed);
        auto current=queue_high_water.load(std::memory_order_relaxed);
        while(depth>current && !queue_high_water.compare_exchange_weak(current,depth,std::memory_order_relaxed)){}
    }

    void set_queue_pressure(QueuePressureLevel level) noexcept {
        const auto raw = static_cast<std::uint8_t>(level);
        const auto previous = queue_pressure_level.exchange(raw, std::memory_order_relaxed);
        if (previous != raw) queue_pressure_transitions.fetch_add(1, std::memory_order_relaxed);
    }

    void observe_queue_saturation() noexcept {
        queue_saturation_events.fetch_add(1, std::memory_order_relaxed);
        set_queue_pressure(QueuePressureLevel::Saturated);
    }

    void observe_queue_wakeup() noexcept {
        queue_wakeups.fetch_add(1, std::memory_order_relaxed);
    }

    nlohmann::json snapshot() const {
        const auto pressure = static_cast<QueuePressureLevel>(queue_pressure_level.load(std::memory_order_relaxed));
        return {{"market_events_total",market_events.load(std::memory_order_relaxed)},
                {"queue_depth",queue_depth.load(std::memory_order_relaxed)},
                {"queue_high_water",queue_high_water.load(std::memory_order_relaxed)},
                {"queue_pressure",queue_pressure_name(pressure)},
                {"queue_pressure_level",static_cast<std::uint8_t>(pressure)},
                {"queue_pressure_transitions",queue_pressure_transitions.load(std::memory_order_relaxed)},
                {"queue_saturation_events",queue_saturation_events.load(std::memory_order_relaxed)},
                {"queue_wakeups",queue_wakeups.load(std::memory_order_relaxed)},
                {"parse_latency",parse_latency.snapshot()},
                {"event_dispatch_latency",event_dispatch_latency.snapshot()},
                {"strategy_decision_latency",strategy_decision_latency.snapshot()},
                {"sqlite_batch_latency",sqlite_batch_latency.snapshot()}};
    }
};

class ScopedLatency {
public:
    explicit ScopedLatency(LatencyHistogram& histogram):histogram_(histogram),start_(std::chrono::steady_clock::now()){}
    ~ScopedLatency(){const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start_).count();histogram_.observe(static_cast<std::uint64_t>(us<0?0:us));}
private:
    LatencyHistogram& histogram_; std::chrono::steady_clock::time_point start_;
};

class SampledScopedLatency {
public:
    SampledScopedLatency(LatencyHistogram& histogram, LatencySampler& sampler) noexcept
        : histogram_(&histogram), active_(sampler.should_sample()) {
        if (active_) start_ = std::chrono::steady_clock::now();
    }

    ~SampledScopedLatency() {
        if (!active_) return;
        const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start_).count();
        histogram_->observe(static_cast<std::uint64_t>(us<0?0:us));
    }

private:
    LatencyHistogram* histogram_;
    bool active_;
    std::chrono::steady_clock::time_point start_{};
};

} // namespace sentum::market
