#include <sentum/market/RuntimePerformanceMetrics.hpp>
#include <sentum/market/SpscRingQueue.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void test_observed_spsc_push_reports_depth_and_empty_transition() {
    sentum::market::SpscRingQueue<int, 5> queue;

    const auto first = queue.try_push_observed(1);
    require(first.accepted, "first observed push was rejected");
    require(first.was_empty, "first observed push did not report empty-to-non-empty transition");
    require(first.depth == 1, "first observed push reported wrong queue depth");

    const auto second = queue.try_push_observed(2);
    require(second.accepted, "second observed push was rejected");
    require(!second.was_empty, "second observed push incorrectly reported empty queue");
    require(second.depth == 2, "second observed push reported wrong queue depth");

    require(queue.try_push_observed(3).accepted, "third observed push was rejected");
    require(queue.try_push_observed(4).accepted, "fourth observed push was rejected");
    const auto full = queue.try_push_observed(5);
    require(!full.accepted, "full observed push unexpectedly succeeded");
    require(full.depth == queue.usable_capacity(), "full observed push did not report usable capacity");

    int value = 0;
    require(queue.try_pop(value) && value == 1, "observed queue lost FIFO order");
    const auto wrapped = queue.try_push_observed(5);
    require(wrapped.accepted, "observed push failed after wrap-around pop");
    require(wrapped.depth == queue.usable_capacity(), "wrapped observed push reported wrong depth");
}

void test_latency_sampler_cadence() {
    sentum::market::LatencySampler sampler(64);
    std::uint32_t samples = 0;
    for (std::uint32_t i = 0; i < 128; ++i) {
        if (sampler.should_sample()) ++samples;
    }
    require(samples == 2, "latency sampler did not produce deterministic 1:64 cadence");
    require(sampler.sample_every() == 64, "latency sampler lost configured cadence");
}

void test_sampled_latency_records_only_selected_events() {
    sentum::market::LatencyHistogram histogram;
    sentum::market::LatencySampler sampler(8);
    for (int i = 0; i < 80; ++i) {
        sentum::market::SampledScopedLatency latency(histogram, sampler);
    }
    require(histogram.count() == 10, "sampled latency histogram recorded unexpected sample count");
}

void test_queue_pressure_metrics() {
    sentum::market::RuntimePerformanceMetrics metrics;
    metrics.observe_queue_depth(4);
    metrics.set_queue_pressure(sentum::market::QueuePressureLevel::Elevated);
    metrics.set_queue_pressure(sentum::market::QueuePressureLevel::Elevated);
    metrics.observe_queue_wakeup();
    metrics.observe_queue_saturation();

    const auto snapshot = metrics.snapshot();
    require(snapshot.value("queue_depth", 0ULL) == 4, "runtime metrics lost current queue depth");
    require(snapshot.value("queue_high_water", 0ULL) == 4, "runtime metrics lost queue high-water mark");
    require(snapshot.value("queue_wakeups", 0ULL) == 1, "runtime metrics lost queue wakeup count");
    require(snapshot.value("queue_saturation_events", 0ULL) == 1, "runtime metrics lost saturation count");
    require(snapshot.value("queue_pressure", std::string()) == "saturated", "runtime metrics lost pressure state");
    require(snapshot.value("queue_pressure_transitions", 0ULL) == 2,
            "runtime metrics pressure transitions were not edge-triggered");
}

} // namespace

int main() {
    try {
        test_observed_spsc_push_reports_depth_and_empty_transition();
        test_latency_sampler_cadence();
        test_sampled_latency_records_only_selected_events();
        test_queue_pressure_metrics();
        std::cout << "runtime observability tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime observability test failure: " << error.what() << '\n';
        return 1;
    }
}
