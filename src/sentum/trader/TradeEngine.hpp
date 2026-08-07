#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sentum/api/BinanceRestClient.hpp>
#include <sentum/api/BinanceWebsocketClient.hpp>
#include <sentum/market/MarketEvent.hpp>
#include <sentum/time/Clock.hpp>
#include <sentum/trader/execution/IExecutionVenue.hpp>
#include <sentum/trader/execution/SimulatedExecutionVenue.hpp>
#include <sentum/trader/history/TradeHistoryRepository.hpp>
#include <sentum/trader/risk/RiskManager.hpp>
#include <sentum/trader/strategy/IStrategy.hpp>
#include <sentum/trader/types/RiskConfig.hpp>
#include <sentum/trader/types/TradeAction.hpp>
#include <sentum/trader/types/TradePosition.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>
#include <sentum/trader/utils/TradeLogger.hpp>
#include <sentum/utils/AsyncLogger.hpp>

class TradeEngine {
public:
    explicit TradeEngine(const std::string& symbol, BinanceRestClient& binance, bool paper_trading);
    TradeEngine(const std::string& symbol, RiskConfig config, std::shared_ptr<IClock> clock,
                std::unique_ptr<IStrategy> strategy, const std::string& history_path = ":memory:");
    ~TradeEngine();

    void run();
    void stop();
    TradeAction process_event(const MarketEvent& event);
    TradeAction evaluate(double price);
    const std::vector<TradePosition>& completed_trades() const { return completed_; }
    TradePosition get_current_position() const;
    double get_latest_price() const;
    double get_total_profit() const;
    int get_win_count() const;
    int get_lose_count() const;
    double get_winrate_percent() const;
    int get_total_trades() const;
    double get_average_profit() const;

private:
    void initialize_components();
    void enqueue_price(double price);
    TradeAction evaluate_at(double price, std::chrono::system_clock::time_point now, const std::string& source);
    TradeAction close_position(double market_price, const std::string& reason, std::chrono::system_clock::time_point now);
    sentum::order::Snapshot execute(sentum::order::Side side, double quantity, double price,
                                    std::chrono::system_clock::time_point now, const char* purpose);

    std::string symbol;
    BinanceRestClient* api = nullptr;
    std::atomic<bool> running{false};
    bool isPaperTrading = true;
    RiskConfig risk;
    TradePosition position;
    TradeLogger logger;
    AsyncLogger engine_logger;
    mutable std::mutex state_mutex;
    double total_profit = 0.0;
    int win_count = 0;
    int lose_count = 0;
    std::unique_ptr<IStrategy> strategy;
    std::unique_ptr<RiskManager> risk_manager;
    std::unique_ptr<TradeHistoryRepository> history;
    std::unique_ptr<sentum::execution::SimulatedExecutionVenue> execution_venue;
    std::shared_ptr<IClock> clock;
    std::string history_path = "log/klines.sqlite3";
    std::vector<TradePosition> completed_;
    std::chrono::system_clock::time_point last_exit{};
    std::unique_ptr<BinanceWebsocketClient> price_stream;
    std::atomic<double> latest_price{0.0};
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<MarketEvent> price_queue;
    static constexpr std::size_t max_queue_size = 4096;
};
