#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace sentum::runtime {

class RuntimeControl {
public:
    static RuntimeControl& global() { static RuntimeControl instance; return instance; }

    void configure(nlohmann::json strategy, bool auto_symbol, std::string manual_symbol) {
        std::lock_guard<std::mutex> lock(mutex_);
        strategy_ = std::move(strategy);
        auto_symbol_ = auto_symbol;
        manual_symbol_ = std::move(manual_symbol);
    }

    nlohmann::json strategy() const { std::lock_guard<std::mutex> lock(mutex_); return strategy_; }
    void set_strategy(nlohmann::json value) { std::lock_guard<std::mutex> lock(mutex_); strategy_ = std::move(value); ++generation_; }

    bool auto_symbol() const { std::lock_guard<std::mutex> lock(mutex_); return auto_symbol_; }
    std::string manual_symbol() const { std::lock_guard<std::mutex> lock(mutex_); return manual_symbol_; }
    void set_auto_symbol(bool value) { std::lock_guard<std::mutex> lock(mutex_); auto_symbol_ = value; ++generation_; }
    void set_manual_symbol(std::string value) { std::lock_guard<std::mutex> lock(mutex_); manual_symbol_ = std::move(value); auto_symbol_ = false; ++generation_; }

    void pause_entries(bool value) noexcept { entries_paused_.store(value, std::memory_order_release); }
    bool entries_paused() const noexcept { return entries_paused_.load(std::memory_order_acquire); }
    void request_manual_close() noexcept { manual_close_.store(true, std::memory_order_release); }
    bool consume_manual_close() noexcept { return manual_close_.exchange(false, std::memory_order_acq_rel); }
    std::uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }

private:
    mutable std::mutex mutex_;
    nlohmann::json strategy_ = {{"type","momentum"},{"parameters",{{"lookback",20},{"entry_threshold",0.001}}}};
    bool auto_symbol_ = true;
    std::string manual_symbol_;
    std::atomic<bool> entries_paused_{false};
    std::atomic<bool> manual_close_{false};
    std::atomic<std::uint64_t> generation_{0};
};

} // namespace sentum::runtime
