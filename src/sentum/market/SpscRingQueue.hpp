#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace sentum::market {

template <typename T, std::size_t Capacity>
class SpscRingQueue {
    static_assert(Capacity > 1, "SPSC queue capacity must exceed one");
public:
    bool try_push(T value) noexcept(std::is_nothrow_move_constructible_v<T>) {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = increment(head);
        if (next == tail_.load(std::memory_order_acquire)) return false;
        slots_[head].emplace(std::move(value));
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = std::move(*slots_[tail]);
        slots_[tail].reset();
        tail_.store(increment(tail), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

    std::size_t size_approx() const noexcept {
        const auto head = head_.load(std::memory_order_acquire);
        const auto tail = tail_.load(std::memory_order_acquire);
        return head >= tail ? head - tail : Capacity - (tail - head);
    }

    static constexpr std::size_t usable_capacity() noexcept { return Capacity - 1; }

private:
    static constexpr std::size_t increment(std::size_t value) noexcept { return (value + 1) % Capacity; }
    std::array<std::optional<T>, Capacity> slots_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace sentum::market
