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

#include <sentum/utils/AsyncLogger.hpp>

AsyncLogger::AsyncLogger(const std::string& path) : file_path(path), running(false) {}

AsyncLogger::~AsyncLogger() {
	stop();
}

void AsyncLogger::start() {
	running = true;
	worker = std::thread(&AsyncLogger::run, this);
}

void AsyncLogger::stop() {
	running = false;
	cv.notify_all();
	if (worker.joinable()) worker.join();
}

void AsyncLogger::log(const std::string& message) {
	std::lock_guard<std::mutex> lock(mtx);
	auto t = std::time(nullptr);
	auto tm = *std::localtime(&t);
	std::ostringstream timestamped;
	timestamped << "[" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "] " << message;
	messages.push(timestamped.str());
	cv.notify_one();
}

void AsyncLogger::run() {
	std::ofstream file(file_path, std::ios::app);
	while (running || !messages.empty()) {
		std::unique_lock<std::mutex> lock(mtx);
		cv.wait(lock, [this] { return !messages.empty() || !running; });
		while (!messages.empty()) {
			file << messages.front() << std::endl;
			messages.pop();
		}
		file.flush();
	}
}