#include <sentum/dashboard/DashboardState.hpp>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_versioned_snapshot_and_change_detection() {
    auto& state = sentum::dashboard::DashboardState::global();
    const auto initial = state.snapshot_versioned();
    require(initial.generation > 0, "initial dashboard generation must be non-zero");

    sentum::dashboard::DashboardSnapshot unchanged;
    require(!state.snapshot_if_changed(initial.generation, unchanged),
            "unchanged dashboard unexpectedly produced a new snapshot");

    state.merge({{"snapshot_test_value", 42}, {"snapshot_test_pair", 42}});

    sentum::dashboard::DashboardSnapshot changed;
    require(state.snapshot_if_changed(initial.generation, changed),
            "changed dashboard did not produce a new snapshot");
    require(changed.generation > initial.generation, "dashboard generation did not advance");
    require(changed.state.value("snapshot_test_value", 0) == 42, "snapshot missed merged value");
    require(changed.state.value("snapshot_test_pair", 0) == 42, "snapshot missed merged pair value");

    sentum::dashboard::DashboardSnapshot repeated;
    require(!state.snapshot_if_changed(changed.generation, repeated),
            "same generation produced a duplicate snapshot copy");
}

void test_concurrent_snapshot_consistency() {
    auto& state = sentum::dashboard::DashboardState::global();
    constexpr std::uint64_t iterations = 5000;
    std::atomic<bool> writer_done{false};
    std::atomic<std::uint64_t> mismatches{0};

    std::thread writer([&] {
        for (std::uint64_t sequence = 1; sequence <= iterations; ++sequence) {
            state.merge({{"snapshot_sequence_a", sequence}, {"snapshot_sequence_b", sequence}});
        }
        writer_done.store(true, std::memory_order_release);
    });

    std::uint64_t generation = 0;
    while (!writer_done.load(std::memory_order_acquire)) {
        sentum::dashboard::DashboardSnapshot snapshot;
        if (!state.snapshot_if_changed(generation, snapshot)) continue;
        generation = snapshot.generation;
        const auto a = snapshot.state.value("snapshot_sequence_a", std::uint64_t{0});
        const auto b = snapshot.state.value("snapshot_sequence_b", std::uint64_t{0});
        if (a != b) mismatches.fetch_add(1, std::memory_order_relaxed);
    }

    writer.join();
    const auto final_snapshot = state.snapshot_versioned();
    require(final_snapshot.state.value("snapshot_sequence_a", std::uint64_t{0}) == iterations,
            "final dashboard snapshot missed writer completion");
    require(final_snapshot.state.value("snapshot_sequence_b", std::uint64_t{0}) == iterations,
            "final dashboard snapshot pair missed writer completion");
    require(mismatches.load(std::memory_order_relaxed) == 0,
            "dashboard snapshot observed a torn multi-field merge");
}

} // namespace

int main() {
    try {
        test_versioned_snapshot_and_change_detection();
        test_concurrent_snapshot_consistency();
        std::cout << "dashboard snapshot tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dashboard snapshot test failure: " << error.what() << '\n';
        return 1;
    }
}
