/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */
#pragma once

#include <sentum/api/ReconnectingMarketStream.hpp>
#include <sentum/api/BinanceEnvironment.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class BinanceWebsocketClient {
public:
	using TradeCallback = std::function<void(double, std::chrono::system_clock::time_point)>;
    using HealthCallback = std::function<void(const sentum::api::MarketStreamHealth&)>;
    explicit BinanceWebsocketClient(const std::string& symbol,
        std::chrono::milliseconds maximum_event_age = std::chrono::milliseconds(2000),
        sentum::api::BinanceEnvironment environment = sentum::api::BinanceEnvironment::PublicMarketData);
	~BinanceWebsocketClient();

	void start();
	void stop();
	void set_on_price(const std::function<void(double)>& callback);
    void set_on_trade(const TradeCallback& callback);
    void set_on_health(const HealthCallback& callback);
    sentum::api::MarketStreamHealth stream_health() const;
    sentum::api::BinanceEnvironment environment() const noexcept { return environment_; }
    std::string endpoint() const { return sentum::api::market_trade_url(environment_, symbol); }

private:
	friend struct WebsocketLifecycleTestAccess;
    friend struct MarketStreamTestAccess;
	struct Impl;
	std::unique_ptr<Impl> impl;
	std::string symbol;
    const sentum::api::BinanceEnvironment environment_;
	std::atomic<bool> running;
	std::thread ws_thread;
	std::thread::id worker_id;
	mutable std::mutex lifecycle_mutex;
	std::condition_variable lifecycle_cv;
	bool joining = false;
	std::mutex callback_mutex;
	std::function<void(double)> on_price;
    TradeCallback on_trade;
    HealthCallback on_health;
    sentum::api::MarketStreamSettings stream_settings;
    sentum::api::MarketStreamHealth stopped_health;
    std::string test_url;
    std::string test_hostname;
	// Lifecycle qualification can pause preparation before any connection work.
	std::function<void()> before_connect;

	void run();
	void request_stop();
	void join_worker(std::unique_lock<std::mutex>& lock);
	void handle_message(const std::string& payload, bool network_message = true);
};
