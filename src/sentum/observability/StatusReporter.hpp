#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace sentum::observability {

class StatusReporter {
public:
    explicit StatusReporter(std::string path = "log/status.json") : path_(std::move(path)) {}

    void set(const std::string& key, nlohmann::json value) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_[key] = std::move(value);
        state_["updated_at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        flush_unlocked();
    }

private:
    void flush_unlocked() {
        const std::filesystem::path path(path_);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        const std::filesystem::path temp(path.string() + ".tmp");
        { std::ofstream out(temp, std::ios::trunc); if (!out) return; out << state_.dump(2) << '\n'; }
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec) { std::filesystem::remove(path, ec); std::filesystem::rename(temp, path, ec); }
    }

    std::string path_;
    std::mutex mutex_;
    nlohmann::json state_ = nlohmann::json::object();
};

} // namespace sentum::observability
