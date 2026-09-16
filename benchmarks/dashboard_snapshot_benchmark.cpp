#include <sentum/dashboard/DashboardState.hpp>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>

int main(int argc, char** argv) {
    const std::uint64_t iterations = argc > 1 ? static_cast<std::uint64_t>(std::stoull(argv[1])) : 250000;
    auto& state = sentum::dashboard::DashboardState::global();

    nlohmann::json scanner = nlohmann::json::array();
    for (int i = 0; i < 64; ++i) {
        scanner.push_back({{"symbol", "SYMBOL" + std::to_string(i)}, {"return", static_cast<double>(i) / 10000.0}});
    }
    state.merge({
        {"mode", "paper"}, {"health", "healthy"}, {"current_symbol", "BTCUSDT"},
        {"strategy_name", "momentum"}, {"balance", 10000.0}, {"scanner", scanner},
        {"performance", {{"queue_depth", 4}, {"queue_pressure", "normal"}}}
    });

    std::uint64_t copied_bytes = 0;
    const auto unconditional_started = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        const auto snapshot = state.snapshot();
        copied_bytes += snapshot.dump().size();
    }
    const auto unconditional_elapsed = std::chrono::steady_clock::now() - unconditional_started;

    const auto baseline = state.snapshot_versioned();
    std::uint64_t conditional_copies = 0;
    const auto conditional_started = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        sentum::dashboard::DashboardSnapshot snapshot;
        if (state.snapshot_if_changed(baseline.generation, snapshot)) ++conditional_copies;
    }
    const auto conditional_elapsed = std::chrono::steady_clock::now() - conditional_started;

    const auto unconditional_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(unconditional_elapsed).count();
    const auto conditional_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(conditional_elapsed).count();

    std::cout << std::fixed << std::setprecision(2)
              << "iterations=" << iterations << '\n'
              << "unconditional_ns_per_poll=" << static_cast<double>(unconditional_ns) / static_cast<double>(iterations) << '\n'
              << "unchanged_conditional_ns_per_poll=" << static_cast<double>(conditional_ns) / static_cast<double>(iterations) << '\n'
              << "conditional_snapshot_copies=" << conditional_copies << '\n'
              << "copied_payload_bytes=" << copied_bytes << '\n';

    return conditional_copies == 0 ? 0 : 1;
}
