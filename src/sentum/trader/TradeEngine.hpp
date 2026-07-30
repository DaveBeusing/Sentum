/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <sentum/trader/types/RiskConfig.hpp>
#include <sentum/trader/types/TradePosition.hpp>
#include <sentum/trader/types/TradeAction.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>
#include <sentum/trader/utils/TradeLogger.hpp>
#include <sentum/utils/AsyncLogger.hpp>
#include <sentum/api/BinanceRestClient.hpp>
#include <sentum/api/BinanceWebsocketClient.hpp>

class TradeEngine {
public:
	explicit TradeEngine(const std::string& symbol, BinanceRestClient& binance, bool paper_trading);
	~TradeEngine();

	void run();
	void stop();

	TradeAction evaluate(double price);
	TradePosition get_current_position() const;
	double get_latest_price() const;
	double get_total_profit() const;
	int get_win_count() const;
	int get_lose_count() const;
	double get_winrate_percent() const;
	int get_total_trades() const;
	double get_average_profit() const;

private:
	void enqueue_price(double price);

	std::string symbol;
	BinanceRestClient& api;
	std::atomic<bool> running;
	bool isPaperTrading;

	RiskConfig risk;
	TradePosition position;
	TradeLogger logger;
	AsyncLogger engine_logger;

	mutable std::mutex state_mutex;
	double total_profit = 0.0;
	int win_count = 0;
	int lose_count = 0;

	std::unique_ptr<BinanceWebsocketClient> price_stream;
	std::atomic<double> latest_price{0.0};

	std::mutex queue_mutex;
	std::condition_variable queue_cv;
	std::deque<double> price_queue;
	static constexpr std::size_t max_queue_size = 4096;
};
