#pragma once

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
    }

    void merge(const nlohmann::json& value) {
        if (!value.is_object()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = value.begin(); it != value.end(); ++it) state_[it.key()] = it.value();
    }

    nlohmann::json snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

private:
    DashboardState() = default;
    mutable std::mutex mutex_;
    nlohmann::json state_ = {
        {"mode", "idle"},
        {"health", "starting"},
        {"market_data_connected", false},
        {"user_stream_connected", false},
        {"reconciliation_complete", false},
        {"kill_switch_active", false},
        {"scanner", nlohmann::json::array()},
        {"active_position", nullptr}
    };
};

} // namespace sentum::dashboard
