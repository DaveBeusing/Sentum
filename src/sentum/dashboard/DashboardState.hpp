#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace sentum::dashboard {

class DashboardState {
public:
    static DashboardState& global() {
        static DashboardState instance;
        return instance;
    }

    template <typename T>
    void set(const std::string& key, T&& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_[key] = std::forward<T>(value);
        generation_.fetch_add(1, std::memory_order_release);
    }

    void merge(const nlohmann::json& value) {
        if (!value.is_object()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = value.begin(); it != value.end(); ++it) state_[it.key()] = it.value();
        generation_.fetch_add(1, std::memory_order_release);
    }

    nlohmann::json snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    std::uint64_t generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

private:
    DashboardState() = default;
    mutable std::mutex mutex_;
    std::atomic<std::uint64_t> generation_{1};
    nlohmann::json state_ = {
        {"mode", "idle"},
        {"health", "starting"},
        {"scanner", nlohmann::json::array()},
        {"active_position", nullptr}
    };
};

} // namespace sentum::dashboard
