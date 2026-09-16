/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * 
 * MIT License - https://opensource.org/license/mit/
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the “Software”), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is furnished 
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all 
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A 
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION 
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 */

#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

#include <sentum/utils/AsyncLogger.hpp>

namespace {
std::tm local_time(std::time_t value) {
    std::tm result{};
#if defined(_WIN32)
    localtime_s(&result, &value);
#else
    localtime_r(&value, &result);
#endif
    return result;
}
} // namespace

AsyncLogger::AsyncLogger(const std::string& path) : file_path(path), running(false) {}

AsyncLogger::~AsyncLogger() {
	stop();
}

void AsyncLogger::start() {
	bool expected = false;
	if (!running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
	if (worker.joinable()) worker.join();
	worker = std::thread(&AsyncLogger::run, this);
}

void AsyncLogger::stop() {
	running.store(false, std::memory_order_release);
	cv.notify_all();
	if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
}

void AsyncLogger::log(const std::string& message) {
	auto t = std::time(nullptr);
	const auto tm = local_time(t);
	std::ostringstream timestamped;
	timestamped << "[" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "] " << message;
	{
		std::lock_guard<std::mutex> lock(mtx);
		messages.push(timestamped.str());
	}
	cv.notify_one();
}

void AsyncLogger::run() {
	std::ofstream file(file_path, std::ios::app);
	for (;;) {
		std::queue<std::string> pending;
		{
			std::unique_lock<std::mutex> lock(mtx);
			cv.wait(lock, [this] {
				return !messages.empty() || !running.load(std::memory_order_acquire);
			});
			if (messages.empty() && !running.load(std::memory_order_acquire)) break;
			pending.swap(messages);
		}

		while (!pending.empty()) {
			file << pending.front() << std::endl;
			pending.pop();
		}
		file.flush();
	}
}
