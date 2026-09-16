#include <sentum/ui/TerminalRenderPipeline.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
	const std::size_t iterations = argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 250000;
	const std::string frame =
		"SENTUM paper healthy BTCUSDT\n"
		"Equity 10000.00 USDC\n"
		"[1] MARKET [2] SCANNER [3] ORDERS [4] TRADES [5] STRATEGY [6] MODELS [7] SYSTEM\n"
		"MARKET / POSITION / RISK\n"
		"Symbol BTCUSDT Last 100000.00\n"
		"FLAT - no open paper position\n"
		"Risk/trade 1.00% Stop 1.00% Target 2.00%\n"
		"MARKET WATCH\n"
		"BTCUSDT +0.42% 1 TRADING\n"
		"ETHUSDT +0.31% 2 WATCH\n";
	const auto previous = sentum::ui::split_terminal_lines(frame);

	std::size_t unchanged_bytes = 0;
	const auto unchanged_start = std::chrono::steady_clock::now();
	for (std::size_t i = 0; i < iterations; ++i) {
		auto diff = sentum::ui::build_terminal_frame_diff(previous, frame, false);
		unchanged_bytes += diff.payload.size();
	}
	const auto unchanged_elapsed = std::chrono::steady_clock::now() - unchanged_start;

	const std::string changed = frame + "notice: refresh\n";
	std::size_t changed_bytes = 0;
	const auto changed_start = std::chrono::steady_clock::now();
	for (std::size_t i = 0; i < iterations; ++i) {
		auto diff = sentum::ui::build_terminal_frame_diff(previous, changed, false);
		changed_bytes += diff.payload.size();
	}
	const auto changed_elapsed = std::chrono::steady_clock::now() - changed_start;

	const auto unchanged_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(unchanged_elapsed).count();
	const auto changed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(changed_elapsed).count();
	std::cout << "iterations=" << iterations << '\n'
		<< "unchanged_ns_per_frame=" << static_cast<double>(unchanged_ns) / static_cast<double>(iterations) << '\n'
		<< "unchanged_payload_bytes=" << unchanged_bytes << '\n'
		<< "changed_ns_per_frame=" << static_cast<double>(changed_ns) / static_cast<double>(iterations) << '\n'
		<< "changed_payload_bytes=" << changed_bytes << '\n';

	return unchanged_bytes == 0 ? 0 : 2;
}
