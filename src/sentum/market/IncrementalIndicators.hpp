#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace sentum::market {

class RollingReturn {
public:
    explicit RollingReturn(std::size_t period) : values_(std::max<std::size_t>(2, period), 0.0) {}

    void push(double value) noexcept {
        values_[head_] = value;
        head_ = (head_ + 1) % values_.size();
        if (size_ < values_.size()) ++size_;
    }

    bool ready() const noexcept { return size_ == values_.size(); }
    double value() const noexcept {
        if (!ready()) return 0.0;
        const double first = values_[head_];
        const double last = values_[(head_ + values_.size() - 1) % values_.size()];
        return first > 0.0 ? (last - first) / first : 0.0;
    }

    void reset() noexcept { head_ = 0; size_ = 0; }

private:
    std::vector<double> values_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

class RollingSma {
public:
    explicit RollingSma(std::size_t period) : values_(std::max<std::size_t>(1, period), 0.0) {}

    double push(double value) noexcept {
        if (size_ < values_.size()) {
            values_[head_] = value;
            sum_ += value;
            ++size_;
        } else {
            sum_ -= values_[head_];
            values_[head_] = value;
            sum_ += value;
        }
        head_ = (head_ + 1) % values_.size();
        return current();
    }

    bool ready() const noexcept { return size_ == values_.size(); }
    double current() const noexcept { return size_ == 0 ? 0.0 : sum_ / static_cast<double>(size_); }

private:
    std::vector<double> values_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    double sum_ = 0.0;
};

class Ema {
public:
    explicit Ema(std::size_t period) : alpha_(2.0 / (static_cast<double>(std::max<std::size_t>(1, period)) + 1.0)) {}

    double push(double value) noexcept {
        if (!initialized_) { value_ = value; initialized_ = true; }
        else value_ += alpha_ * (value - value_);
        return value_;
    }

    double current() const noexcept { return value_; }

private:
    double alpha_;
    double value_ = 0.0;
    bool initialized_ = false;
};

class Rsi {
public:
    explicit Rsi(std::size_t period = 14) : period_(std::max<std::size_t>(1, period)) {}

    double push(double price) noexcept {
        if (!has_previous_) { previous_ = price; has_previous_ = true; return 50.0; }
        const double delta = price - previous_;
        previous_ = price;
        const double gain = std::max(0.0, delta);
        const double loss = std::max(0.0, -delta);
        if (samples_ < period_) {
            avg_gain_ += gain;
            avg_loss_ += loss;
            ++samples_;
            if (samples_ == period_) { avg_gain_ /= period_; avg_loss_ /= period_; }
        } else {
            avg_gain_ = (avg_gain_ * (period_ - 1) + gain) / period_;
            avg_loss_ = (avg_loss_ * (period_ - 1) + loss) / period_;
        }
        if (samples_ < period_) return 50.0;
        if (avg_loss_ <= 0.0) return 100.0;
        const double rs = avg_gain_ / avg_loss_;
        return 100.0 - (100.0 / (1.0 + rs));
    }

    bool ready() const noexcept { return samples_ >= period_; }

private:
    std::size_t period_;
    std::size_t samples_ = 0;
    double previous_ = 0.0;
    double avg_gain_ = 0.0;
    double avg_loss_ = 0.0;
    bool has_previous_ = false;
};

} // namespace sentum::market
