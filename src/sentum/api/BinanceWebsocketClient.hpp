/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
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
	struct Impl;
	std::unique_ptr<Impl> impl;
	std::string symbol;
	std::atomic<bool> running;
	std::thread ws_thread;
	std::function<void(double)> on_price;

	void run();
};
