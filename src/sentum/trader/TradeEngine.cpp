/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */

#include <chrono>
#include <stdexcept>

#include <sentum/trader/TradeEngine.hpp>

TradeEngine::TradeEngine(const std::string& symbol_, BinanceRestClient& api_, bool paper_)
	: symbol(symbol_), api(api_), running(false), isPaperTrading(paper_), engine_logger("log/engine.log") {}

TradeEngine::~TradeEngine() {
	stop();
}

void TradeEngine::enqueue_price(double price) {
	latest_price.store(price, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		if (price_queue.size() >= max_queue_size) {
			price_queue.pop_front();
		}
		price_queue.push_back(price);
	}
	queue_cv.notify_one();
}

void TradeEngine::stop() {
	if (!running.exchange(false)) return;
	if (price_stream) price_stream->stop();
	queue_cv.notify_all();
}

void TradeEngine::run() {
	if (!isPaperTrading) {
		throw std::runtime_error("Live trading is disabled until order execution is production-ready");
	}
	if (running.exchange(true)) return;

	engine_logger.start();
	try {
		risk = load_risk_config("config/risk.json");
		price_stream = std::make_unique<BinanceWebsocketClient>(symbol);
		price_stream->set_on_price([this](double price) { enqueue_price(price); });
		price_stream->start();

		while (running.load()) {
			double price = 0.0;
			{
				std::unique_lock<std::mutex> lock(queue_mutex);
				queue_cv.wait(lock, [this] { return !running.load() || !price_queue.empty(); });
				if (!running.load() && price_queue.empty()) break;
				price = price_queue.front();
				price_queue.pop_front();
			}

			const auto start = std::chrono::steady_clock::now();
			evaluate(price);
			const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - start).count();
			if (latency > 500) {
				engine_logger.log("[WARNING] High strategy latency: " + std::to_string(latency) + " us");
			}
		}
	} catch (...) {
		stop();
		engine_logger.stop();
		throw;
	}

	if (price_stream) price_stream->stop();
	engine_logger.stop();
}

TradeAction TradeEngine::evaluate(double price) {
	std::lock_guard<std::mutex> lock(state_mutex);

	if (!position.open) {
		position.open = true;
		position.simulated = true;
		position.symbol = symbol;
		position.entry_price = price;
		position.executed_price = price;
		position.entry_time = std::chrono::system_clock::now();
		position.signal_time = std::chrono::system_clock::now();
		position.quantity = (risk.max_total_capital * risk.risk_per_trade) / price;
		position.highest_price = price;
		position.lowest_price = price;
		position.stop_loss_price = price * (1.0 - risk.stop_loss_percent);
		position.take_profit_price = price * (1.0 + risk.take_profit_percent);
		position.risk_per_trade = risk.risk_per_trade;
		position.stop_loss_percent = risk.stop_loss_percent;
		position.take_profit_percent = risk.take_profit_percent;
		position.trailing_sl_enabled = risk.trailing_sl_enabled;
		position.trailing_sl_percent = risk.trailing_sl_percent;
		position.trailing_tp_enabled = risk.trailing_tp_enabled;
		position.trailing_tp_percent = risk.trailing_tp_percent;
		position.buy_fee_percent = risk.buy_fee_percent;
		position.sell_fee_percent = risk.sell_fee_percent;
		logger.log(position, TradeAction::BUY);
		return TradeAction::BUY;
	}

	if (price > position.highest_price) {
		position.highest_price = price;
		if (position.trailing_sl_enabled) {
			position.stop_loss_price = price * (1.0 - position.trailing_sl_percent);
		}
		if (position.trailing_tp_enabled) {
			position.take_profit_price = price * (1.0 - position.trailing_tp_percent);
		}
	}
	if (price < position.lowest_price) position.lowest_price = price;

	if (price <= position.stop_loss_price || price >= position.take_profit_price) {
		position.exit_price = price;
		position.exit_time = std::chrono::system_clock::now();
		position.stop_loss_triggered = price <= position.stop_loss_price;
		position.take_profit_triggered = price >= position.take_profit_price;
		position.gross_profit = (price - position.entry_price) * position.quantity;
		position.fee_entry = position.entry_price * position.quantity * position.buy_fee_percent;
		position.fee_exit = price * position.quantity * position.sell_fee_percent;
		position.net_profit = position.gross_profit - position.fee_entry - position.fee_exit;
		total_profit += position.net_profit;
		if (position.net_profit >= 0.0) ++win_count;
		else ++lose_count;
		logger.log(position, TradeAction::SELL);
		position = TradePosition();
		return TradeAction::SELL;
	}

	return TradeAction::NONE;
}

TradePosition TradeEngine::get_current_position() const {
	std::lock_guard<std::mutex> lock(state_mutex);
	return position;
}

double TradeEngine::get_latest_price() const {
	return latest_price.load(std::memory_order_relaxed);
}

double TradeEngine::get_total_profit() const {
	std::lock_guard<std::mutex> lock(state_mutex);
	return total_profit;
}

int TradeEngine::get_win_count() const {
	std::lock_guard<std::mutex> lock(state_mutex);
	return win_count;
}

int TradeEngine::get_lose_count() const {
	std::lock_guard<std::mutex> lock(state_mutex);
	return lose_count;
}

double TradeEngine::get_winrate_percent() const {
	std::lock_guard<std::mutex> lock(state_mutex);
	const int total = win_count + lose_count;
	return total == 0 ? 0.0 : (static_cast<double>(win_count) / total) * 100.0;
}

int TradeEngine::get_total_trades() const {
	std::lock_guard<std::mutex> lock(state_mutex);
	return win_count + lose_count;
}

double TradeEngine::get_average_profit() const {
	std::lock_guard<std::mutex> lock(state_mutex);
	const int total = win_count + lose_count;
	return total == 0 ? 0.0 : total_profit / static_cast<double>(total);
}
