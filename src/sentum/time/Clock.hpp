#pragma once

#include <chrono>

class IClock {
public:
    virtual ~IClock() = default;
    virtual std::chrono::system_clock::time_point now() const = 0;
};

class SystemClock final : public IClock {
public:
    std::chrono::system_clock::time_point now() const override {
        return std::chrono::system_clock::now();
    }
};

class ReplayClock final : public IClock {
public:
    explicit ReplayClock(std::chrono::system_clock::time_point initial = {}) : current_(initial) {}
    std::chrono::system_clock::time_point now() const override { return current_; }
    void advance_to(std::chrono::system_clock::time_point value) {
        if (value < current_) throw std::runtime_error("Replay clock cannot move backwards");
        current_ = value;
    }
private:
    std::chrono::system_clock::time_point current_;
};
