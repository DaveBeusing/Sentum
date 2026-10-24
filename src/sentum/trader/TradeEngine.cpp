#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <type_traits>

#include <sentum/core/RuntimeControl.hpp>
#include <sentum/dashboard/DashboardState.hpp>
#include <sentum/market/RuntimePerformanceMetrics.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/strategy/MomentumStrategy.hpp>
#include <sentum/trader/risk/PositionExitPolicy.hpp>

namespace {
constexpr std::uint32_t kStrategyLatencySampleEvery = 64;
}

TradeEngine::TradeEngine(const std::string& symbol_, BinanceRestClient& api_, bool paper_)
    : symbol(symbol_), api(&api_), isPaperTrading(paper_), engine_logger("log/engine.log"),
      clock(std::make_shared<SystemClock>()) {}

TradeEngine::TradeEngine(const std::string& symbol_, BinanceRestClient& api_, RiskConfig config,
                         std::unique_ptr<IStrategy> strategy_, const std::string& history_path_)
    : symbol(symbol_), api(&api_), isPaperTrading(true), runtime_configured_(true), risk(std::move(config)),
      engine_logger("log/engine.log"), strategy(std::move(strategy_)), clock(std::make_shared<SystemClock>()),
      history_path(history_path_) {
    if (!strategy) throw std::invalid_argument("Paper engine requires strategy");
}

TradeEngine::TradeEngine(const std::string& symbol_, RiskConfig config, std::shared_ptr<IClock> clock_,
                         std::unique_ptr<IStrategy> strategy_, const std::string& history_path_)
    : symbol(symbol_), isPaperTrading(true), runtime_configured_(true), risk(std::move(config)), engine_logger("log/replay-engine.log"),
      strategy(std::move(strategy_)), clock(std::move(clock_)), history_path(history_path_) {
    if (!clock || !strategy) throw std::invalid_argument("Replay engine requires clock and strategy");
    initialize_components();
}

TradeEngine::~TradeEngine() { stop(); }

void TradeEngine::initialize_components() {
    if (!strategy) strategy = std::make_unique<MomentumStrategy>();
    risk_manager = std::make_unique<RiskManager>(risk, clock);
    history = std::make_unique<TradeHistoryRepository>(history_path);
    execution_venue = std::make_unique<sentum::execution::SimulatedExecutionVenue>(api ? "paper" : "replay");
    execution_venue->set_fill_model(risk.spread_percent, risk.slippage_percent);
    execution_venue->start([](const sentum::order::Snapshot&) {});
}

void TradeEngine::set_entry_readiness(std::function<bool()> ready) {
    std::lock_guard<std::mutex> lock(state_mutex);
    entry_readiness_ = std::move(ready);
}

sentum::api::MarketStreamHealth TradeEngine::price_stream_health() const {
    std::lock_guard<std::mutex> lock(queue_mutex);
    return price_stream ? price_stream->stream_health() : sentum::api::MarketStreamHealth{};
}

bool TradeEngine::live_price_ready() const {
    if (!api) return true;
    std::lock_guard<std::mutex> lock(queue_mutex);
    return !run_started_ || (price_stream && price_stream->stream_health().fresh);
}

void TradeEngine::enqueue_price(double price) { enqueue_price(price, clock->now()); }

void TradeEngine::enqueue_price(double price, std::chrono::system_clock::time_point timestamp) {
    if (!std::isfinite(price) || price <= 0.0) return;
    latest_price.store(price, std::memory_order_relaxed);
    MarketEvent event;
    event.type = MarketEvent::Type::Trade;
    event.symbol = symbol;
    event.timestamp = timestamp;
    event.price = price;
    event.close = price;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (stop_requested_.load(std::memory_order_acquire)) return;
        if (price_queue.size() >= max_queue_size) price_queue.pop_front();
        price_queue.push_back(std::move(event));
    }
    queue_cv.notify_one();
}

void TradeEngine::stop() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        stop_requested_.store(true, std::memory_order_release);
        running.store(false, std::memory_order_release);
    }
    queue_cv.notify_all();
}

bool TradeEngine::try_stop_when_flat() {
    // Serializing the stop request with decisions prevents an entry between
    // observing a flat position and cancelling a worker for reconfiguration.
    std::lock_guard<std::mutex> lock(state_mutex);
    if (position.open) return false;
    stop();
    return true;
}

void TradeEngine::run() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (run_started_ || stop_requested_.load(std::memory_order_acquire)) return;
        run_started_ = true;
        running.store(true, std::memory_order_release);
    }
    try {
        if (!isPaperTrading) throw std::runtime_error("Live trading is disabled until order execution is production-ready");
        if (!api) throw std::runtime_error("Network run requires BinanceRestClient");
        engine_logger.start();
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!runtime_configured_) risk = load_risk_config("config/risk.json");
            initialize_components();
            sentum::dashboard::DashboardState::global().merge({
                {"strategy_name", strategy->name()}, {"entries_paused", sentum::runtime::RuntimeControl::global().entries_paused()}
            });
        }
        {
            // The stream is owned by this worker. Cancellation only signals it;
            // no other thread can access partially initialized runtime objects.
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (!stop_requested_.load(std::memory_order_acquire)) {
                price_stream = std::make_unique<BinanceWebsocketClient>(symbol, std::chrono::milliseconds(risk.max_data_age_ms));
                price_stream->set_on_trade([this](double price, std::chrono::system_clock::time_point timestamp) {
                    enqueue_price(price, timestamp);
                });
                price_stream->start();
            }
        }
        run_event_loop();
    } catch (...) {
        stop();
        if (price_stream) price_stream->stop();
        if (execution_venue) execution_venue->stop();
        engine_logger.stop();
        throw;
    }
    stop();
    if (price_stream) price_stream->stop();
    if (execution_venue) execution_venue->stop();
    engine_logger.stop();
}

void TradeEngine::run_event_loop() {
    while (true) {
        MarketEvent event;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [this] {
                return stop_requested_.load(std::memory_order_acquire) || !price_queue.empty();
            });
            if (stop_requested_.load(std::memory_order_acquire)) return;
            event = std::move(price_queue.front());
            price_queue.pop_front();
        }
        process_event(event);
    }
}

sentum::order::Snapshot TradeEngine::execute(sentum::order::Side side, double quantity, double price,
                                              std::chrono::system_clock::time_point now, const char* purpose) {
    if (!execution_venue) throw std::logic_error("Execution venue is not initialized");
    if (!std::isfinite(price) || price <= 0.0 || !std::isfinite(quantity) || quantity <= 0.0 ||
        !std::isfinite(price * quantity) || price * quantity <= 0.0)
        throw std::invalid_argument("Execution price, quantity and notional must be finite and positive");
    execution_venue->set_market(price, now);
    sentum::order::Request request;
    request.symbol = symbol;
    request.side = side;
    request.quantity = quantity;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    request.client_order_id = "sentum-" + symbol + "-" + purpose + "-" + std::to_string(micros);
    return execution_venue->submit(request);
}

TradeAction TradeEngine::process_event(const MarketEvent& event) {
    thread_local sentum::market::LatencySampler decision_sampler(kStrategyLatencySampleEvery);
    sentum::market::SampledScopedLatency decision_latency(
        sentum::market::RuntimePerformanceMetrics::global().strategy_decision_latency,
        decision_sampler);
    if (event.symbol != symbol || !std::isfinite(event.price) || event.price <= 0.0) return TradeAction::NONE;
    const auto current = clock->now();
    using Ticks = std::make_unsigned_t<std::chrono::system_clock::duration::rep>;
    const auto age = static_cast<Ticks>(current.time_since_epoch().count()) -
        static_cast<Ticks>(event.timestamp.time_since_epoch().count());
    const auto maximum_age = static_cast<Ticks>(std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::milliseconds(risk.max_data_age_ms)).count());
    if (current >= event.timestamp && age > maximum_age) {
        engine_logger.log("[RISK] stale market event rejected");
        return TradeAction::NONE;
    }
    latest_price.store(event.price, std::memory_order_relaxed);
    return evaluate_at(event.price, event.timestamp, api ? "binance-websocket" : "replay", &event);
}

TradeAction TradeEngine::evaluate(double price) {
    thread_local sentum::market::LatencySampler decision_sampler(kStrategyLatencySampleEvery);
    sentum::market::SampledScopedLatency decision_latency(
        sentum::market::RuntimePerformanceMetrics::global().strategy_decision_latency,
        decision_sampler);
    return evaluate_at(price, clock->now(), api ? "binance-websocket" : "replay", nullptr);
}

TradeAction TradeEngine::evaluate_at(double price, std::chrono::system_clock::time_point now,
                                     const std::string& source, const MarketEvent* event) {
    if (!std::isfinite(price) || price <= 0.0) return TradeAction::NONE;
    std::lock_guard<std::mutex> lock(state_mutex);
    if (finalization_error_) std::rethrow_exception(finalization_error_);
    if (stop_requested_.load(std::memory_order_acquire) || !live_price_ready()) return TradeAction::NONE;
    auto& dashboard = sentum::dashboard::DashboardState::global();
    auto& control = sentum::runtime::RuntimeControl::global();

    if (position.open && control.consume_manual_close()) return close_position(price, "manual_close", now);

    if (!position.open) {
        // A close request issued while flat must never leak into the next position.
        (void)control.consume_manual_close();
        if (entry_readiness_ && !entry_readiness_()) {
            dashboard.merge({{"last_signal", "blocked"}, {"last_risk_decision", "market_data_unavailable"}});
            return TradeAction::NONE;
        }
        if (control.entries_paused()) {
            dashboard.merge({{"entries_paused", true}, {"last_signal", "paused"}, {"last_risk_decision", "entry_paused"}});
            return TradeAction::NONE;
        }
        const StrategySignal signal = event ? strategy->on_event(*event) : strategy->on_price(price, now);
        dashboard.merge({{"entries_paused", false}, {"strategy_name", strategy->name()},
                         {"last_signal", signal.action == TradeAction::BUY ? "BUY" : "NONE"},
                         {"signal_confidence", signal.confidence}, {"signal_reason", signal.reason}});
        if (signal.action != TradeAction::BUY) return TradeAction::NONE;
        const RiskDecision decision = risk_manager->approve_entry(signal, price, now, last_exit);
        dashboard.merge({{"last_risk_decision", decision.approved ? "APPROVED" : "REJECTED"}, {"risk_reason", decision.reason}});
        if (!decision.approved) return TradeAction::NONE;

        const auto fill = execute(sentum::order::Side::Buy, decision.quantity, price, now, "buy");
        if (!fill.exchange_confirmed_fill()) return TradeAction::NONE;

        position = TradePosition{};
        position.open = true;
        position.simulated = true;
        position.risk_approved = true;
        position.symbol = symbol;
        position.source = source;
        position.strategy = signal.strategy;
        position.signal_reason = signal.reason;
        position.risk_reason = decision.reason;
        position.reference_price = signal.reference_price;
        position.entry_price = fill.average_fill_price;
        position.executed_price = fill.average_fill_price;
        position.entry_time = fill.updated_at;
        position.signal_time = signal.created_at;
        position.quantity = fill.executed_quantity;
        position.highest_price = fill.average_fill_price;
        position.lowest_price = fill.average_fill_price;
        position.stop_loss_price = fill.average_fill_price * (1.0 - risk.stop_loss_percent);
        position.take_profit_price = fill.average_fill_price * (1.0 + risk.take_profit_percent);
        position.risk_per_trade = risk.risk_per_trade;
        position.capital_at_risk = risk.max_total_capital * risk.risk_per_trade;
        position.stop_loss_percent = risk.stop_loss_percent;
        position.take_profit_percent = risk.take_profit_percent;
        position.trailing_sl_enabled = risk.trailing_sl_enabled;
        position.trailing_sl_percent = risk.trailing_sl_percent;
        position.buy_fee_percent = risk.buy_fee_percent;
        position.sell_fee_percent = risk.sell_fee_percent;
        position.fee_entry = fill.average_fill_price * position.quantity * risk.buy_fee_percent;
        logger.log(position, TradeAction::BUY);
        return TradeAction::BUY;
    }
    const auto reason = sentum::risk::evaluate_position_exit(price, now, position.entry_time,
        position.highest_price, position.lowest_price, position.stop_loss_price, position.take_profit_price,
        position.trailing_sl_enabled, position.trailing_sl_percent, std::chrono::seconds(risk.max_holding_seconds));
    if (reason) return close_position(price, reason, now);
    return TradeAction::NONE;
}

TradeAction TradeEngine::close_position(double market_price, const std::string& reason,
                                        std::chrono::system_clock::time_point now) {
    try {
        // Ordinary writer contention must resolve before producing an exit fill.
        auto finalization = history->begin_finalization();
        const auto fill = execute(sentum::order::Side::Sell, position.quantity, market_price, now, "sell");
        if (!fill.exchange_confirmed_fill()) return TradeAction::NONE;

        TradePosition closed = position;
        closed.open = false;
        closed.exit_price = fill.average_fill_price;
        closed.executed_price = fill.average_fill_price;
        closed.exit_time = fill.updated_at;
        closed.client_order_id = fill.client_order_id;
        closed.exchange_order_id = std::to_string(fill.exchange_order_id);
        closed.finalization_id = TradeHistoryRepository::new_finalization_id();
        closed.close_reason = reason;
        closed.stop_loss_triggered = reason == "stop_loss";
        closed.take_profit_triggered = reason == "take_profit";
        closed.gross_profit = (closed.exit_price - closed.entry_price) * closed.quantity;
        closed.fee_exit = closed.exit_price * closed.quantity * risk.sell_fee_percent;
        closed.net_profit = closed.gross_profit - closed.fee_entry - closed.fee_exit;
        if (!std::isfinite(closed.gross_profit) || !std::isfinite(closed.net_profit) ||
            !std::isfinite(closed.fee_entry) || !std::isfinite(closed.fee_exit))
            throw std::runtime_error("Simulated trade settlement overflowed");

        std::exception_ptr persistence_error;
        try {
            finalization.complete(closed);
        } catch (...) {
            persistence_error = std::current_exception();
            finalization_error_ = persistence_error;
        }

        // A confirmed fill remains closed even if its history projection failed.
        // Apply its complete accounting once; further evaluation is latched off.
        position = closed;
        completed_.push_back(closed);
        total_profit += closed.net_profit;
        if (closed.net_profit >= 0.0) ++win_count; else ++lose_count;
        last_exit = closed.exit_time;
        strategy->reset();
        if (persistence_error) {
            std::rethrow_exception(persistence_error);
        }

        logger.log(closed, TradeAction::SELL);
        sentum::dashboard::DashboardState::global().merge({{"last_exit_reason", reason}, {"last_trade_profit", closed.net_profit}});
        return TradeAction::SELL;
    } catch (...) {
        if (!finalization_error_) finalization_error_ = std::current_exception();
        std::rethrow_exception(finalization_error_);
    }
}

TradePosition TradeEngine::get_current_position() const { std::lock_guard<std::mutex> lock(state_mutex); return position; }
double TradeEngine::get_latest_price() const { return latest_price.load(std::memory_order_relaxed); }
double TradeEngine::get_total_profit() const { std::lock_guard<std::mutex> lock(state_mutex); return total_profit; }
int TradeEngine::get_win_count() const { std::lock_guard<std::mutex> lock(state_mutex); return win_count; }
int TradeEngine::get_lose_count() const { std::lock_guard<std::mutex> lock(state_mutex); return lose_count; }
double TradeEngine::get_winrate_percent() const { std::lock_guard<std::mutex> lock(state_mutex); const int total=win_count+lose_count; return total==0?0.0:(static_cast<double>(win_count)/total)*100.0; }
int TradeEngine::get_total_trades() const { std::lock_guard<std::mutex> lock(state_mutex); return win_count+lose_count; }
double TradeEngine::get_average_profit() const { std::lock_guard<std::mutex> lock(state_mutex); const int total=win_count+lose_count; return total==0?0.0:total_profit/static_cast<double>(total); }
std::string TradeEngine::strategy_name() const { std::lock_guard<std::mutex> lock(state_mutex); return strategy ? strategy->name() : std::string("none"); }
