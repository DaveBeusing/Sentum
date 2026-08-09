#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>

#include <sentum/collector/FastBinanceKlineParser.hpp>

namespace {
std::atomic<std::uint64_t> allocations{0};
}

void* operator new(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* ptr = std::malloc(size)) return ptr;
    throw std::bad_alloc();
}

void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }

int main() {
    const std::string payload = R"({"stream":"btcusdt@kline_1s","data":{"e":"kline","E":1720000000000,"s":"BTCUSDT","k":{"t":1720000000000,"T":1720000000999,"s":"BTCUSDT","i":"1s","f":1,"L":2,"o":"60123.12000000","c":"60125.34000000","h":"60130.00000000","l":"60120.00000000","v":"12.34560000","x":true}}})";

    sentum::collector::ParsedKline parsed;
    if (!sentum::collector::FastBinanceKlineParser::parse(payload, parsed)) return 2;

    allocations.store(0, std::memory_order_relaxed);
    constexpr std::size_t iterations = 1000000;
    for (std::size_t i = 0; i < iterations; ++i) {
        if (!sentum::collector::FastBinanceKlineParser::parse(payload, parsed)) return 3;
    }
    const auto count = allocations.load(std::memory_order_relaxed);
    std::cout << "iterations=" << iterations << '\n'
              << "allocations=" << count << '\n'
              << "allocations_per_parse=" << static_cast<double>(count) / iterations << '\n';
    return count == 0 ? 0 : 1;
}
