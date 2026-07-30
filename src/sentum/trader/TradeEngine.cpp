#include <chrono>
#include <stdexcept>

#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/strategy/MomentumStrategy.hpp>

TradeEngine::TradeEngine(const std::string& symbol_, BinanceRestClient& api_, bool paper_)
    : symbol(symbol_), api(api_), running(false), isPaperTrading(paper_), engine_logger("log/engine.log") {}

TradeEngine::~TradeEngine() { stop(); }

void TradeEngine::enqueue_price(double price) {
    latest_price.store(price, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (price_queue.size() >= max_queue_size) price_queue.pop_front();
        price_queue.push_back({price, std::chrono::system_clock::now()});
    }
    queue_cv.notify_one();
}

void TradeEngine::stop() {
    if (!running.exchange(false)) return;
    if (price_stream) price_stream->stop();
    queue_cv.notify_all();
}

void TradeEngine::run() {
    if (!isPaperTrading) throw std::runtime_error("Live trading is disabled until order execution is production-ready");
    if (running.exchange(true)) return;
    engine_logger.start();
    try {
        risk = load_risk_config("config/risk.json");
        strategy = std::make_unique<MomentumStrategy>();
        risk_manager = std::make_unique<RiskManager>(risk);
        history = std::make_unique<TradeHistoryRepository>("log/klines.sqlite3");
        price_stream = std::make_unique<BinanceWebsocketClient>(symbol);
        price_stream->set_on_price([this](double price) { enqueue_price(price); });
        price_stream->start();

        while (running.load()) {
            PriceEvent event{};
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [this] { return !running.load() || !price_queue.empty(); });
                if (!running.load() && price_queue.empty()) break;
                event = price_queue.front();
                price_queue.pop_front();
            }
            const auto age = std::chrono::system_clock::now() - event.observed_at;
            if (age > std::chrono::milliseconds(risk.max_data_age_ms)) {
                engine_logger.log("[RISK] stale price event rejected");
                continue;
            }
            evaluate(event.price);
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
    const auto now = std::chrono::system_clock::now();

    if (!position.open) {
        const StrategySignal signal = strategy->on_price(price, now);
        if (signal.action != TradeAction::BUY) return TradeAction::NONE;
        const RiskDecision decision = risk_manager->approve_entry(signal, price, now, last_exit);
        engine_logger.log("[SIGNAL] strategy=" + signal.strategy + " reason=" + signal.reason);
        engine_logger.log("[RISK] approved=" + std::string(decision.approved ? "true" : "false") + " reason=" + decision.reason);
        if (!decision.approved) return TradeAction::NONE;

        const double ask = price * (1.0 + risk.spread_percent * 0.5);
        const double fill = ask * (1.0 + risk.slippage_percent);
        position = TradePosition{};
        position.open = true;
        position.simulated = true;
        position.risk_approved = true;
        position.symbol = symbol;
        position.source = "binance-websocket";
        position.strategy = signal.strategy;
        position.signal_reason = signal.reason;
        position.risk_reason = decision.reason;
        position.reference_price = signal.reference_price;
        position.entry_price = fill;
        position.executed_price = fill;
        position.entry_time = now;
        position.signal_time = signal.created_at;
        position.quantity = decision.quantity;
        position.highest_price = fill;
        position.lowest_price = fill;
        position.stop_loss_price = fill * (1.0 - risk.stop_loss_percent);
        position.take_profit_price = fill * (1.0 + risk.take_profit_percent);
        position.risk_per_trade = risk.risk_per_trade;
        position.capital_at_risk = risk.max_total_capital * risk.risk_per_trade;
        position.stop_loss_percent = risk.stop_loss_percent;
        position.take_profit_percent = risk.take_profit_percent;
        position.trailing_sl_enabled = risk.trailing_sl_enabled;
        position.trailing_sl_percent = risk.trailing_sl_percent;
        position.trailing_tp_enabled = risk.trailing_tp_enabled;
        position.trailing_tp_percent = risk.trailing_tp_percent;
        position.buy_fee_percent = risk.buy_fee_percent;
        position.sell_fee_percent = risk.sell_fee_percent;
        position.fee_entry = fill * position.quantity * risk.buy_fee_percent;
        logger.log(position, TradeAction::BUY);
        engine_logger.log("[FILL] side=BUY reference=" + std::to_string(price) + " executed=" + std::to_string(fill));
        return TradeAction::BUY;
    }

    if (price > position.highest_price) {
        position.highest_price = price;
        if (position.trailing_sl_enabled) position.stop_loss_price = price * (1.0 - position.trailing_sl_percent);
    }
    if (price < position.lowest_price) position.lowest_price = price;
    if (price <= position.stop_loss_price) return close_position(price, "stop_loss");
    if (price >= position.take_profit_price) return close_position(price, "take_profit");
    if (now - position.entry_time >= std::chrono::seconds(risk.max_holding_seconds)) return close_position(price, "maximum_holding_time");
    return TradeAction::NONE;
}

TradeAction TradeEngine::close_position(double market_price, const std::string& reason) {
    const double bid = market_price * (1.0 - risk.spread_percent * 0.5);
    const double fill = bid * (1.0 - risk.slippage_percent);
    position.exit_price = fill;
    position.exit_time = std::chrono::system_clock::now();
    position.close_reason = reason;
    position.stop_loss_triggered = reason == "stop_loss";
    position.take_profit_triggered = reason == "take_profit";
    position.gross_profit = (fill - position.entry_price) * position.quantity;
    position.fee_exit = fill * position.quantity * risk.sell_fee_percent;
    position.net_profit = position.gross_profit - position.fee_entry - position.fee_exit;
    total_profit += position.net_profit;
    if (position.net_profit >= 0.0) ++win_count; else ++lose_count;
    logger.log(position, TradeAction::SELL);
    history->save(position);
    engine_logger.log("[EXIT] reason=" + reason + " executed=" + std::to_string(fill) + " net=" + std::to_string(position.net_profit));
    last_exit = position.exit_time;
    position.open = false;
    strategy->reset();
    return TradeAction::SELL;
}

TradePosition TradeEngine::get_current_position() const { std::lock_guard<std::mutex> lock(state_mutex); return position; }
double TradeEngine::get_latest_price() const { return latest_price.load(std::memory_order_relaxed); }
double TradeEngine::get_total_profit() const { std::lock_guard<std::mutex> lock(state_mutex); return total_profit; }
int TradeEngine::get_win_count() const { std::lock_guard<std::mutex> lock(state_mutex); return win_count; }
int TradeEngine::get_lose_count() const { std::lock_guard<std::mutex> lock(state_mutex); return lose_count; }
double TradeEngine::get_winrate_percent() const { std::lock_guard<std::mutex> lock(state_mutex); const int total=win_count+lose_count; return total==0?0.0:(static_cast<double>(win_count)/total)*100.0; }
int TradeEngine::get_total_trades() const { std::lock_guard<std::mutex> lock(state_mutex); return win_count+lose_count; }
double TradeEngine::get_average_profit() const { std::lock_guard<std::mutex> lock(state_mutex); const int total=win_count+lose_count; return total==0?0.0:total_profit/static_cast<double>(total); }
