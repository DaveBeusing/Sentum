/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>

#include <sentum/core/ExecutionEngine.hpp>

namespace {
std::atomic<bool> shutdown_requested{false};

void handle_signal(int) noexcept {
	shutdown_requested.store(true, std::memory_order_relaxed);
}
}

int main() {
	std::signal(SIGINT, handle_signal);
	std::signal(SIGTERM, handle_signal);

	try {
		auto engine = std::make_unique<ExecutionEngine>();
		engine->start();

		while (engine->is_running() && !shutdown_requested.load(std::memory_order_relaxed)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		engine->stop();
		return EXIT_SUCCESS;
	} catch (const std::exception& e) {
		std::cerr << "[FATAL] " << e.what() << '\n';
	} catch (...) {
		std::cerr << "[FATAL] Unknown unhandled exception\n";
	}

	return EXIT_FAILURE;
}
