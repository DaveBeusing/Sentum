/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class BinanceWebsocketClient {
public:
	explicit BinanceWebsocketClient(const std::string& symbol);
	~BinanceWebsocketClient();

	void start();
	void stop();
	void set_on_price(const std::function<void(double)>& callback);

private:
	friend struct WebsocketLifecycleTestAccess;
	struct Impl;
	std::unique_ptr<Impl> impl;
	std::string symbol;
	std::atomic<bool> running;
	std::thread ws_thread;
	std::thread::id worker_id;
	std::mutex lifecycle_mutex;
	std::condition_variable lifecycle_cv;
	bool joining = false;
	std::mutex callback_mutex;
	std::function<void(double)> on_price;
	// Lifecycle qualification can pause preparation before any connection work.
	std::function<void()> before_connect;

	void run();
	void request_stop();
	void join_worker(std::unique_lock<std::mutex>& lock);
	void handle_message(const std::string& payload);
};
