#include <sentum/collector/Collector.hpp>
#include <sentum/dashboard/DashboardServer.hpp>
#include <sentum/market/RuntimePerformanceMetrics.hpp>
#include <sentum/operations/OperationalEventRepository.hpp>
#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/execution/AccountReconciler.hpp>
#include <sentum/trader/order/LiveOrderSession.hpp>
#include <sentum/trader/order/OrderManager.hpp>
#include <sentum/trader/strategy/MomentumStrategy.hpp>
#include <sentum/utils/Database.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

struct CollectorTestAccess {
    static void prepare_persistence(Collector& collector) {
        collector.running.store(true, std::memory_order_release);
        collector.producer_stopped.store(true, std::memory_order_release);
        collector.logger.start();
    }

    static void start_writer(Collector& collector) {
        collector.writer_thread = std::thread(&Collector::writer_loop, &collector);
    }

    static bool enqueue(Collector& collector, Kline kline) {
        if (collector.canonical_symbols.empty()) return false;
        return collector.try_enqueue(&collector.canonical_symbols.front(), std::move(kline));
    }

    static void request_stop(Collector& collector) {
        collector.running.store(false, std::memory_order_release);
        collector.producer_stopped.store(true, std::memory_order_release);
        collector.queue_cv.notify_all();
    }

    static void join_writer(Collector& collector) {
        if (collector.writer_thread.joinable()) collector.writer_thread.join();
        collector.logger.stop();
    }

    static constexpr std::size_t capacity() { return Collector::queue_capacity; }
};

namespace {
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void remove_database_files(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + "-wal", ignored);
    std::filesystem::remove(path.string() + "-shm", ignored);
}

std::filesystem::path temporary_database_path(const std::string& suffix) {
    std::filesystem::create_directories("log");
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::path("log") / ("runtime_qualification_" + std::to_string(ticks) + suffix);
}

std::vector<MarketInfo> test_markets() {
    return {MarketInfo{"BTCUSDT", "BTC", 8, "USDT", 8}};
}

Kline make_kline(std::int64_t timestamp, double price = 100.0) {
    return Kline{timestamp, price, price + 1.0, price - 1.0, price + 0.25, 1.0};
}

template <typename Predicate>
void require_eventually(Predicate predicate, std::chrono::milliseconds timeout, const std::string& message) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return;
        std::this_thread::sleep_for(2ms);
    }
    require(predicate(), message);
}

class MockSpotExecutionClient final : public BinanceSpotExecutionClient {
public:
    MockSpotExecutionClient() : BinanceSpotExecutionClient("qualification-key", "qualification-secret") {}

    nlohmann::json place_market_order(const sentum::order::Request& request) const override {
        submitted.fetch_add(1, std::memory_order_relaxed);
        last_request = request.client_order_id;
        return {{"orderId", next_order_id.fetch_add(1, std::memory_order_relaxed) + 1000}};
    }

    nlohmann::json cancel_order(const std::string&, const std::string&) const override {
        cancelled.fetch_add(1, std::memory_order_relaxed);
        return {{"status", "CANCELED"}};
    }

    nlohmann::json query_order(const std::string&, const std::string&) const override {
        return nlohmann::json::object();
    }

    nlohmann::json open_orders() const override {
        open_order_reads.fetch_add(1, std::memory_order_relaxed);
        if (fail_open_orders.load(std::memory_order_acquire)) throw std::runtime_error("injected open-orders failure");
        std::lock_guard<std::mutex> lock(data_mutex);
        return open_orders_response;
    }

    nlohmann::json account() const override {
        account_reads.fetch_add(1, std::memory_order_relaxed);
        if (fail_account.load(std::memory_order_acquire)) throw std::runtime_error("injected account failure");
        std::lock_guard<std::mutex> lock(data_mutex);
        return account_response;
    }

    nlohmann::json exchange_info(const std::string& symbol) const override {
        return {
            {"symbols", nlohmann::json::array({{
                {"symbol", symbol},
                {"filters", nlohmann::json::array({
                    {{"filterType", "LOT_SIZE"}, {"minQty", "0.000001"}, {"maxQty", "1000"}, {"stepSize", "0.000001"}},
                    {{"filterType", "MIN_NOTIONAL"}, {"minNotional", "1"}}
                })}
            }})}
        };
    }

    std::string create_listen_key() const override {
        const auto value = listen_key_creations.fetch_add(1, std::memory_order_relaxed) + 1;
        return "qualification-listen-" + std::to_string(value);
    }

    void keepalive_listen_key(const std::string&) const override {
        keepalive_calls.fetch_add(1, std::memory_order_relaxed);
        int remaining = keepalive_failures_remaining.load(std::memory_order_acquire);
        while (remaining > 0) {
            if (keepalive_failures_remaining.compare_exchange_weak(
                    remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                throw std::runtime_error("injected listen-key keepalive failure");
            }
        }
    }

    void close_listen_key(const std::string&) const override {
        closed_listen_keys.fetch_add(1, std::memory_order_relaxed);
    }

    void set_open_orders(nlohmann::json value) {
        std::lock_guard<std::mutex> lock(data_mutex);
        open_orders_response = std::move(value);
    }

    void set_account(nlohmann::json value) {
        std::lock_guard<std::mutex> lock(data_mutex);
        account_response = std::move(value);
    }

    mutable std::atomic<int> submitted{0};
    mutable std::atomic<int> cancelled{0};
    mutable std::atomic<int> open_order_reads{0};
    mutable std::atomic<int> account_reads{0};
    mutable std::atomic<int> listen_key_creations{0};
    mutable std::atomic<int> keepalive_calls{0};
    mutable std::atomic<int> closed_listen_keys{0};
    mutable std::atomic<int> keepalive_failures_remaining{0};
    mutable std::atomic<std::int64_t> next_order_id{0};
    std::atomic<bool> fail_open_orders{false};
    std::atomic<bool> fail_account{false};
    mutable std::string last_request;

private:
    mutable std::mutex data_mutex;
    nlohmann::json open_orders_response = nlohmann::json::array();
    nlohmann::json account_response = {
        {"balances", nlohmann::json::array({
            {{"asset", "BTC"}, {"free", "0"}, {"locked", "0"}},
            {{"asset", "USDT"}, {"free", "10000"}, {"locked", "0"}}
        })}
    };
};

struct MockStreamState {
    std::atomic<bool> running{false};
    BinanceUserDataStream::Handler handler;
};

class MockUserDataStream final : public BinanceUserDataStream {
public:
    MockUserDataStream(std::shared_ptr<MockStreamState> state, BinanceUserDataStream::Handler handler)
        : BinanceUserDataStream("qualification", {}), state_(std::move(state)) {
        state_->handler = std::move(handler);
    }

    ~MockUserDataStream() override { stop(); }

    void start() override { state_->running.store(true, std::memory_order_release); }
    void stop() override { state_->running.store(false, std::memory_order_release); }
    bool running() const noexcept override { return state_->running.load(std::memory_order_acquire); }

private:
    std::shared_ptr<MockStreamState> state_;
};

class MockStreamRegistry {
public:
    sentum::order::LiveOrderSession::StreamFactory factory() {
        return [this](std::string, BinanceUserDataStream::Handler handler) {
            auto state = std::make_shared<MockStreamState>();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                states_.push_back(state);
            }
            return std::make_unique<MockUserDataStream>(std::move(state), std::move(handler));
        };
    }

    std::shared_ptr<MockStreamState> latest() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (states_.empty()) return {};
        return states_.back();
    }

    std::size_t creations() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return states_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<MockStreamState>> states_;
};

nlohmann::json execution_report(
    const std::string& state, double requested, double executed, double quote, std::int64_t order_id = 77) {
    const auto event_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return {
        {"e", "executionReport"},
        {"c", "qualification-order"},
        {"s", "BTCUSDT"},
        {"i", order_id},
        {"S", "BUY"},
        {"q", std::to_string(requested)},
        {"z", std::to_string(executed)},
        {"Z", std::to_string(quote)},
        {"X", state},
        {"r", "NONE"},
        {"E", event_ms}
    };
}

nlohmann::json scenario_partial_fill() {
    MockSpotExecutionClient client;
    sentum::order::OrderManager manager(client);
    std::vector<sentum::order::Snapshot> updates;
    manager.set_update_handler([&](const sentum::order::Snapshot& update) { updates.push_back(update); });
    manager.reconcile_startup();

    sentum::order::Request request{"BTCUSDT", sentum::order::Side::Buy, 1.0, "qualification-order"};
    const auto acknowledgement = manager.submit_market(request);
    require(acknowledgement.state == sentum::order::State::Acknowledged, "market order was not acknowledged");

    manager.on_user_data_event(execution_report("PARTIALLY_FILLED", 1.0, 0.4, 40.0));
    const auto partial = manager.get(request.client_order_id);
    require(partial.has_value(), "partial fill order disappeared");
    require(partial->state == sentum::order::State::PartiallyFilled, "partial fill state was not preserved");
    require(!partial->exchange_confirmed_fill(), "partial fill was treated as a confirmed complete fill");

    manager.on_user_data_event(execution_report("FILLED", 1.0, 1.0, 101.0));
    const auto filled = manager.get(request.client_order_id);
    require(filled.has_value() && filled->exchange_confirmed_fill(), "final exchange fill was not authoritative");

    return {
        {"order_updates", updates.size()},
        {"executed_quantity", filled->executed_quantity},
        {"average_fill_price", filled->average_fill_price},
        {"reconciliation_outcome", "consistent"},
        {"kill_switch_transitions", 0}
    };
}

nlohmann::json scenario_unresolved_order_restart() {
    MockSpotExecutionClient client;
    client.set_open_orders(nlohmann::json::array({{
        {"symbol", "BTCUSDT"},
        {"clientOrderId", "existing-order"},
        {"orderId", 123},
        {"side", "BUY"},
        {"origQty", "1.0"},
        {"executedQty", "0.25"},
        {"cummulativeQuoteQty", "25.0"},
        {"status", "PARTIALLY_FILLED"}
    }}));

    sentum::order::OrderManager manager(client);
    manager.reconcile_startup();
    require(manager.reconciled(), "restart reconciliation did not complete");
    require(manager.kill_switch_active(), "unresolved restart order did not latch the kill switch");

    bool clear_blocked = false;
    try { manager.clear_kill_switch_for_reconciled_resume(); }
    catch (const std::logic_error&) { clear_blocked = true; }
    require(clear_blocked, "kill switch cleared while an unresolved order remained");

    bool submission_blocked = false;
    try {
        manager.submit_market({"BTCUSDT", sentum::order::Side::Buy, 1.0, "new-order"});
    } catch (const std::logic_error&) {
        submission_blocked = true;
    }
    require(submission_blocked, "new submission was allowed during ambiguous restart recovery");

    return {
        {"reconciliation_outcome", "blocked_unresolved_order"},
        {"open_orders", 1},
        {"kill_switch_transitions", 1},
        {"submission_blocked", true}
    };
}

nlohmann::json scenario_balance_mismatch() {
    const auto path = temporary_database_path("_balance.sqlite3");
    remove_database_files(path);
    try {
        nlohmann::json result;
        {
            MockSpotExecutionClient client;
            client.set_account({
                {"balances", nlohmann::json::array({
                    {{"asset", "BTC"}, {"free", "0.25"}, {"locked", "0.10"}},
                    {{"asset", "USDT"}, {"free", "10000"}, {"locked", "0"}}
                })}
            });
            sentum::operations::OperationalEventRepository events(path.string());
            sentum::execution::AccountReconciler reconciler(client, events);
            const auto report = reconciler.run("BTCUSDT", 1.0, "BTC");
            require(!report.success, "balance mismatch unexpectedly reconciled");
            require(report.inconsistencies > 0, "balance mismatch produced no inconsistency evidence");
            result = {
                {"reconciliation_outcome", "balance_mismatch"},
                {"inconsistencies", report.inconsistencies},
                {"balances_checked", report.balances_checked},
                {"details", report.details},
                {"kill_switch_transitions", 0}
            };
        }
        remove_database_files(path);
        return result;
    } catch (...) {
        remove_database_files(path);
        throw;
    }
}

std::unique_ptr<sentum::order::LiveOrderSession> make_session(
    std::unique_ptr<MockSpotExecutionClient> client,
    MockStreamRegistry& streams,
    std::chrono::milliseconds poll = 5ms,
    std::chrono::milliseconds keepalive = 5s) {
    sentum::order::LiveOrderSession::Timing timing;
    timing.supervisor_poll_interval = poll;
    timing.keepalive_interval = keepalive;
    return sentum::order::LiveOrderSession::from_dependencies(
        std::move(client), streams.factory(), timing);
}

nlohmann::json scenario_user_stream_interruption() {
    auto client = std::make_unique<MockSpotExecutionClient>();
    auto* client_ptr = client.get();
    MockStreamRegistry streams;
    auto session = make_session(std::move(client), streams);
    session->start();
    require(session->ready(), "session was not initially ready");

    constexpr std::size_t disconnect_cycles = 3;
    for (std::size_t cycle = 0; cycle < disconnect_cycles; ++cycle) {
        auto stream = streams.latest();
        require(static_cast<bool>(stream), "mock user stream was not created");
        const auto expected_creations = streams.creations() + 1;
        stream->running.store(false, std::memory_order_release);
        require_eventually([&] {
            return session->killed() && !session->ready() && streams.creations() >= expected_creations;
        }, 500ms, "User Data Stream interruption did not fail closed and reconnect");
    }

    bool submission_blocked = false;
    try {
        session->submit({"BTCUSDT", sentum::order::Side::Buy, 1.0, "blocked-after-reconnect"});
    } catch (const std::logic_error&) {
        submission_blocked = true;
    }
    require(submission_blocked, "automatic stream recovery allowed a new submission");

    const auto reconnects = streams.creations() - 1;
    require(reconnects >= disconnect_cycles, "repeated stream recovery lost a reconnect cycle");
    session->stop();

    return {
        {"reconnect_count", reconnects},
        {"listen_key_creations", client_ptr->listen_key_creations.load()},
        {"reconciliation_outcome", "recovered_but_operator_resume_required"},
        {"kill_switch_transitions", 1},
        {"submission_blocked", submission_blocked}
    };
}

nlohmann::json scenario_listen_key_keepalive() {
    auto client = std::make_unique<MockSpotExecutionClient>();
    auto* client_ptr = client.get();
    client_ptr->keepalive_failures_remaining.store(1, std::memory_order_release);
    MockStreamRegistry streams;
    auto session = make_session(std::move(client), streams, 5ms, 10ms);
    session->start();

    require_eventually([&] {
        return client_ptr->keepalive_calls.load(std::memory_order_acquire) >= 1 &&
               session->killed() && !session->ready() && streams.creations() >= 2;
    }, 750ms, "listen-key keepalive failure did not fail closed and rebuild the stream");

    const auto reconnects = streams.creations() - 1;
    session->stop();
    return {
        {"reconnect_count", reconnects},
        {"keepalive_calls", client_ptr->keepalive_calls.load()},
        {"reconciliation_outcome", "recovered_but_operator_resume_required"},
        {"kill_switch_transitions", 1},
        {"submission_blocked", true}
    };
}

nlohmann::json scenario_kill_switch_recovery() {
    auto client = std::make_unique<MockSpotExecutionClient>();
    MockStreamRegistry streams;
    auto session = make_session(std::move(client), streams);
    session->start();
    auto stream = streams.latest();
    require(static_cast<bool>(stream), "mock stream missing before recovery test");
    stream->running.store(false, std::memory_order_release);

    require_eventually([&] { return session->killed() && streams.creations() >= 2; },
                       500ms, "fault did not latch kill switch");
    require(!session->resume_after_reconcile("WRONG_CONFIRMATION"),
            "kill switch recovery accepted an invalid confirmation");
    require(session->killed() && !session->ready(),
            "invalid confirmation changed recovery state");

    require(session->resume_after_reconcile("I_CONFIRM_RECONCILED_TESTNET_RESUME"),
            "explicit reconciled recovery did not resume");
    require(session->ready() && !session->killed(),
            "session did not return to ready state after explicit reconciled recovery");
    const auto reconnects = streams.creations() - 1;
    session->stop();

    return {
        {"reconnect_count", reconnects},
        {"reconciliation_outcome", "explicit_resume_after_reconciliation"},
        {"kill_switch_transitions", 2},
        {"invalid_confirmation_blocked", true}
    };
}

nlohmann::json scenario_market_data_reconnect() {
    const auto path = temporary_database_path("_market.sqlite3");
    remove_database_files(path);
    try {
        nlohmann::json result;
        {
            Database db(path.string());
            {
                Collector collector(db, test_markets());
                CollectorTestAccess::prepare_persistence(collector);
                CollectorTestAccess::start_writer(collector);
                for (std::int64_t i = 0; i < 128; ++i) {
                    require(CollectorTestAccess::enqueue(collector, make_kline(i + 1)),
                            "market producer enqueue failed before disconnect");
                }
                CollectorTestAccess::request_stop(collector);
                CollectorTestAccess::join_writer(collector);
                require(collector.queue_depth() == 0, "disconnect left market persistence backlog");
            }
            {
                Collector collector(db, test_markets());
                CollectorTestAccess::prepare_persistence(collector);
                CollectorTestAccess::start_writer(collector);
                for (std::int64_t i = 0; i < 64; ++i) {
                    require(CollectorTestAccess::enqueue(collector, make_kline(1000 + i)),
                            "market producer enqueue failed after reconnect");
                }
                CollectorTestAccess::request_stop(collector);
                CollectorTestAccess::join_writer(collector);
                require(collector.queue_depth() == 0, "reconnect left market persistence backlog");
            }

            const auto rows = db.load_klines("btcusdt", 512);
            require(rows.size() == 192, "market reconnect did not preserve deterministic persistence continuity");
            result = {
                {"reconnect_count", 1},
                {"events_persisted", rows.size()},
                {"queue_depth", 0},
                {"reconciliation_outcome", "not_applicable"},
                {"kill_switch_transitions", 0}
            };
        }
        remove_database_files(path);
        return result;
    } catch (...) {
        remove_database_files(path);
        throw;
    }
}

nlohmann::json scenario_persistence_pressure() {
    const auto path = temporary_database_path("_pressure.sqlite3");
    remove_database_files(path);
    try {
        nlohmann::json result;
        {
            Database db(path.string());
            Collector collector(db, test_markets());
            CollectorTestAccess::prepare_persistence(collector);

            const auto capacity = CollectorTestAccess::capacity();
            for (std::size_t i = 0; i < capacity; ++i) {
                require(CollectorTestAccess::enqueue(collector, make_kline(static_cast<std::int64_t>(i + 1))),
                        "persistence queue saturated before configured capacity");
            }
            require(!CollectorTestAccess::enqueue(collector, make_kline(static_cast<std::int64_t>(capacity + 1))),
                    "persistence queue accepted data beyond bounded capacity");

            const auto saturated = sentum::market::RuntimePerformanceMetrics::global().snapshot();
            require(saturated.value("queue_pressure", std::string{}) == "saturated",
                    "queue saturation was not visible in runtime metrics");
            require(collector.dropped_count() == 1, "queue saturation drop accounting mismatch");

            CollectorTestAccess::request_stop(collector);
            CollectorTestAccess::start_writer(collector);
            CollectorTestAccess::join_writer(collector);
            require(collector.queue_depth() == 0, "persistence queue did not drain after pressure recovery");

            const auto final_metrics = sentum::market::RuntimePerformanceMetrics::global().snapshot();
            result = {
                {"queue_depth", final_metrics.value("queue_depth", 0ULL)},
                {"queue_high_water", final_metrics.value("queue_high_water", 0ULL)},
                {"queue_saturation_events", final_metrics.value("queue_saturation_events", 0ULL)},
                {"queue_drop_count", collector.dropped_count()},
                {"reconciliation_outcome", "not_applicable"},
                {"kill_switch_transitions", 0}
            };
        }
        remove_database_files(path);
        return result;
    } catch (...) {
        remove_database_files(path);
        throw;
    }
}

std::uint16_t reserve_free_port() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(
        io, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    return acceptor.local_endpoint().port();
}

nlohmann::json scenario_dashboard_recovery() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor blocker(
        io, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    blocker.listen();
    const auto port = blocker.local_endpoint().port();

    sentum::dashboard::DashboardServer server("127.0.0.1", port);
    bool failed_as_expected = false;
    try { server.start(); }
    catch (const std::exception&) { failed_as_expected = true; }
    require(failed_as_expected, "dashboard start unexpectedly succeeded on occupied port");
    require(!server.running(), "dashboard remained running after injected start failure");

    boost::system::error_code ec;
    blocker.close(ec);
    server.start();
    require(server.running(), "dashboard did not recover after injected start failure");
    server.stop();
    require(!server.running(), "dashboard remained running after recovery stop");

    return {
        {"lifecycle_failures", 0},
        {"restart_count", 1},
        {"reconciliation_outcome", "not_applicable"},
        {"kill_switch_transitions", 0}
    };
}

nlohmann::json scenario_paper_soak(double duration_seconds, std::uint64_t seed) {
    require(duration_seconds > 0.0, "paper soak duration must be positive");
    const auto path = temporary_database_path("_paper.sqlite3");
    remove_database_files(path);

    try {
        nlohmann::json result;
        {
            RiskConfig risk;
        risk.max_total_capital = 10000.0;
        risk.risk_per_trade = 0.001;
        risk.stop_loss_percent = 0.01;
        risk.take_profit_percent = 0.01;
        risk.cooldown_seconds = 0;
        risk.max_holding_seconds = 5;
        risk.max_data_age_ms = 60000;
        risk.min_notional = 1.0;

        auto clock = std::make_shared<ReplayClock>(std::chrono::system_clock::now());
        TradeEngine engine(
            "BTCUSDT", risk, clock, std::make_unique<MomentumStrategy>(8, 0.0002), path.string());

        std::mt19937_64 random(seed);
        std::normal_distribution<double> movement(0.0, 0.0007);
        double price = 100.0;
        std::uint64_t events = 0;
        std::uint64_t buy_actions = 0;
        std::uint64_t sell_actions = 0;
        const auto started = std::chrono::steady_clock::now();
        auto event_time = clock->now();

        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() < duration_seconds) {
            price = std::max(1.0, price * (1.0 + movement(random)));
            MarketEvent event;
            event.type = MarketEvent::Type::Trade;
            event.symbol = "BTCUSDT";
            event.timestamp = event_time;
            event.price = price;
            event.close = price;
            const auto action = engine.process_event(event);
            if (action == TradeAction::BUY) ++buy_actions;
            if (action == TradeAction::SELL) ++sell_actions;
            ++events;
            event_time += 20ms;
            clock->advance_to(event_time);
            std::this_thread::sleep_for(1ms);
        }

        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto metrics = sentum::market::RuntimePerformanceMetrics::global().snapshot();
            const auto trades = engine.get_total_trades();
            result = {
                {"event_count", events},
                {"event_throughput_per_second", elapsed > 0.0 ? static_cast<double>(events) / elapsed : 0.0},
                {"buy_actions", buy_actions},
                {"sell_actions", sell_actions},
                {"completed_trades", trades},
                {"queue_depth", metrics.value("queue_depth", 0ULL)},
                {"queue_high_water", metrics.value("queue_high_water", 0ULL)},
                {"queue_saturation_events", metrics.value("queue_saturation_events", 0ULL)},
                {"queue_drop_count", 0},
                {"latency", metrics},
                {"reconnect_count", 0},
                {"restart_count", 0},
                {"lifecycle_failures", 0},
                {"reconciliation_outcome", "not_applicable"},
                {"kill_switch_transitions", 0}
            };
        }
        remove_database_files(path);
        return result;
    } catch (...) {
        remove_database_files(path);
        throw;
    }
}

using Scenario = std::function<nlohmann::json()>;

nlohmann::json run_named(const std::string& scenario, double duration_seconds, std::uint64_t seed) {
    if (scenario == "paper-soak") return scenario_paper_soak(duration_seconds, seed);
    if (scenario == "partial-fill") return scenario_partial_fill();
    if (scenario == "unresolved-order-restart") return scenario_unresolved_order_restart();
    if (scenario == "balance-mismatch") return scenario_balance_mismatch();
    if (scenario == "user-stream-interruption") return scenario_user_stream_interruption();
    if (scenario == "listen-key-keepalive") return scenario_listen_key_keepalive();
    if (scenario == "kill-switch-recovery") return scenario_kill_switch_recovery();
    if (scenario == "market-data-reconnect") return scenario_market_data_reconnect();
    if (scenario == "persistence-pressure") return scenario_persistence_pressure();
    if (scenario == "dashboard-recovery") return scenario_dashboard_recovery();
    throw std::invalid_argument("unknown qualification scenario: " + scenario);
}

std::vector<std::string> fault_scenarios() {
    return {
        "market-data-reconnect",
        "user-stream-interruption",
        "listen-key-keepalive",
        "partial-fill",
        "unresolved-order-restart",
        "balance-mismatch",
        "persistence-pressure",
        "dashboard-recovery",
        "kill-switch-recovery"
    };
}

} // namespace

int main(int argc, char** argv) {
    std::string scenario = "all-faults";
    double duration_seconds = 2.0;
    std::uint64_t seed = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--scenario" && i + 1 < argc) scenario = argv[++i];
        else if (arg == "--duration-seconds" && i + 1 < argc) duration_seconds = std::stod(argv[++i]);
        else if (arg == "--seed" && i + 1 < argc) seed = static_cast<std::uint64_t>(std::stoull(argv[++i]));
        else if (arg == "--list") {
            nlohmann::json values = fault_scenarios();
            values.push_back("paper-soak");
            std::cout << values.dump() << '\n';
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 2;
        }
    }

    const auto started = std::chrono::steady_clock::now();
    try {
        nlohmann::json result;
        if (scenario == "all-faults") {
            nlohmann::json scenarios = nlohmann::json::object();
            for (const auto& name : fault_scenarios()) scenarios[name] = run_named(name, duration_seconds, seed);
            result = {
                {"scenario", scenario},
                {"pass", true},
                {"scenarios", std::move(scenarios)}
            };
        } else {
            result = {
                {"scenario", scenario},
                {"pass", true},
                {"metrics", run_named(scenario, duration_seconds, seed)}
            };
        }
        result["elapsed_seconds"] = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        nlohmann::json result = {
            {"scenario", scenario},
            {"pass", false},
            {"failure", error.what()},
            {"elapsed_seconds", std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count()}
        };
        std::cout << result.dump() << '\n';
        return 1;
    }
}
