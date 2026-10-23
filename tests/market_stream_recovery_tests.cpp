#include "support/LocalTlsMarketServer.hpp"
#include "support/MarketStreamFixtures.hpp"

#include <sentum/core/RuntimeControl.hpp>
#include <sentum/core/ExecutionEngine.hpp>
#include <sentum/dashboard/DashboardState.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/ui/OperationalSafetyPolicy.hpp>
#include <sentum/ui/OperatorAlertPolicy.hpp>

#include <atomic>
#include <future>
#include <iostream>
#include <limits>

struct TradeEngineTestAccess {
    static void attach_live_stream(TradeEngine& engine, BinanceRestClient& api, const std::string& url,
                                   std::atomic<int>& processed) {
        engine.api = &api;
        engine.run_started_ = true;
        engine.price_stream = std::make_unique<BinanceWebsocketClient>("BTCUSDT");
        MarketStreamTestAccess::configure(*engine.price_stream, url);
        engine.price_stream->set_on_trade([&engine, &processed](double price, std::chrono::system_clock::time_point timestamp) {
            MarketEvent event;
            event.symbol = "BTCUSDT";
            event.timestamp = timestamp;
            event.price = price;
            event.close = price;
            engine.process_event(event);
            processed.fetch_add(1, std::memory_order_release);
        });
        engine.price_stream->start();
    }
};

struct DatabaseTestAccess {
    static void trace(Database& database, int (*callback)(unsigned, void*, void*, void*), void* state) {
        sentum::test::require(sqlite3_trace_v2(database.db, SQLITE_TRACE_STMT, callback, state) == SQLITE_OK,
                              "cannot install SQLite write gate");
    }
};

struct ExecutionEngineTestAccess {
    static Collector& prepare(ExecutionEngine& engine, const std::string& path, const std::string& url) {
        auto& control = sentum::runtime::RuntimeControl::global();
        control.configure({{"type", "momentum"}}, true, "");
        engine.applied_control_generation_ = control.generation();
        engine.db_path = path;
        engine.db = std::make_unique<Database>(path);
        engine.market_store = std::make_unique<MarketDataStore>(600);
        engine.collector = std::make_unique<Collector>(*engine.db, *engine.market_store,
            std::vector<MarketInfo>{{"BTCUSDT", "BTC", 8, "USDT", 8}});
        MarketStreamTestAccess::configure(*engine.collector, url);
        return *engine.collector;
    }

    static Database& database(ExecutionEngine& engine) { return *engine.db; }

    static void start(ExecutionEngine& engine, bool other_failures = false) {
        if (other_failures) {
            engine.trader_error_ = std::make_exception_ptr(std::runtime_error("fixture trader finalization failed"));
            engine.paper_account_error_ = std::make_exception_ptr(std::runtime_error("fixture account settlement failed"));
        }
        sentum::dashboard::DashboardState::global().merge({{"health", "healthy"}, {"last_error", nullptr}});
        engine.collector->start();
        engine.running.store(true);
        engine.collector_active.store(true);
        engine.main_thread = std::thread(&ExecutionEngine::run_main_loop, &engine);
    }

    static bool joined(ExecutionEngine& engine) {
        return !engine.main_thread.joinable() && !engine.scanner_thread.joinable() &&
               !engine.trader_thread.joinable() && MarketStreamTestAccess::joined(*engine.collector);
    }
};

namespace {
using namespace std::chrono_literals;
using sentum::test::require;
using sentum::test::eventually;

class BuyStrategy final : public IStrategy {
public:
    StrategySignal on_price(double price, std::chrono::system_clock::time_point now) override {
        return {TradeAction::BUY, "fixture", "entry", price, now, 1.0};
    }
    void reset() override {}
};

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        std::string pattern = (std::filesystem::temp_directory_path() / "sentum-market-storage-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        require(created != nullptr, "cannot create storage fixture directory");
        directory_ = created;
        path = directory_ / "candles.sqlite3";
    }
    ~TemporaryDatabase() { std::error_code ignored; std::filesystem::remove_all(directory_, ignored); }
    std::filesystem::path path;
private:
    std::filesystem::path directory_;
};

class SqliteControl {
public:
    explicit SqliteControl(const std::filesystem::path& path) {
        require(sqlite3_open(path.c_str(), &db_) == SQLITE_OK, "cannot open fixture SQLite connection");
    }
    ~SqliteControl() { sqlite3_close(db_); }
    void execute(const char* sql) {
        require(sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "fixture SQL failed");
    }
private:
    sqlite3* db_ = nullptr;
};

// The SQLite statement trace holds a real writer at a chosen transaction
// boundary. Tests release it after the competing lifecycle call is waiting.
class WriteAttemptGate {
public:
    explicit WriteAttemptGate(int attempt) : attempt_(attempt) {}
    static int trace(unsigned, void* state, void*, void* statement) {
        auto& gate = *static_cast<WriteAttemptGate*>(state);
        if (std::string_view(static_cast<const char*>(statement)) != "BEGIN IMMEDIATE;") return 0;
        if (++gate.observed_ != gate.attempt_) return 0;
        std::unique_lock<std::mutex> lock(gate.mutex_);
        gate.blocked_ = true;
        gate.cv_.notify_all();
        gate.cv_.wait(lock, [&] { return gate.released_; });
        return 0;
    }
    bool wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 5s, [&] { return blocked_; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }
private:
    const int attempt_;
    int observed_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool blocked_ = false, released_ = false;
};

struct ReleaseWriteGate {
    WriteAttemptGate& gate;
    ~ReleaseWriteGate() { gate.release(); }
};

void client_reconnect_and_freshness(const sentum::test::LocalTlsIdentity& identity) {
    sentum::test::LocalTlsMarketServer server(identity);
    BinanceWebsocketClient client("BTCUSDT");
    MarketStreamTestAccess::configure(client, server.url());
    std::atomic<int> delivered{0};
    client.set_on_price([&](double) { ++delivered; });
    client.start();
    eventually([&] { return client.stream_health().connected; }, "price stream did not connect");
    require(!client.stream_health().fresh, "connection became fresh before a valid event");
    server.send(sentum::test::trade_message());
    eventually([&] { return delivered.load() == 1 && client.stream_health().fresh; }, "valid price did not establish freshness");
    const auto valid_timestamp = client.stream_health().last_event_time_ms;
    eventually([&] { const auto h = client.stream_health(); return h.connected && !h.fresh; }, "silent stream never became stale");
    server.send(R"({"p":"not-a-price","T":1})");
    server.send(R"({"p":"100","T":9223372036854775807})");
    server.send(sentum::test::trade_message(90.0, sentum::test::unix_milliseconds() - 10000));
    require(client.stream_health().last_event_time_ms == valid_timestamp, "invalid event refreshed price health");
    eventually([&] { return client.stream_health().reconnects >= 1; }, "silent socket was not reconnected");
    require(!client.stream_health().fresh, "reconnected socket reused old freshness");
    server.send(sentum::test::trade_message(101.0));
    eventually([&] { return delivered.load() == 2 && client.stream_health().fresh; }, "price delivery did not resume after idle reconnect");

    const auto previous = client.stream_health().successful_connections;
    server.drop_connections();
    eventually([&] { return !client.stream_health().connected; }, "abrupt socket loss remained connected");
    const auto down = client.stream_health();
    require(down.worker_running && !down.fresh, "worker liveness masked a disconnected socket");
    eventually([&] { return client.stream_health().successful_connections > previous; }, "abruptly dropped socket was not reconnected");
    server.send(sentum::test::trade_message(102.0));
    eventually([&] { return delivered.load() == 3 && client.stream_health().fresh; }, "no data after abrupt reconnect");
    const auto before_stop = std::chrono::steady_clock::now();
    client.stop();
    require(std::chrono::steady_clock::now() - before_stop < 1s, "connected stream stop exceeded one second");
    require(!client.stream_health().connected && !client.stream_health().worker_running, "stopped stream retained liveness");
}

void collector_reconnect_and_drain(const sentum::test::LocalTlsIdentity& identity) {
    sentum::test::LocalTlsMarketServer server(identity);
    Database database(":memory:");
    MarketDataStore store(600);
    Collector collector(database, store, {MarketInfo{"BTCUSDT", "BTC", 8, "USDT", 8}});
    MarketStreamTestAccess::configure(collector, server.url());
    collector.start();
    eventually([&] { return collector.stream_health().connected; }, "collector did not connect");
    server.send(sentum::test::kline_message());
    eventually([&] { return collector.enqueued_count() == 1; }, "collector did not accept initial kline");
    server.drop_connections();
    eventually([&] { return !collector.stream_health().connected; }, "collector socket loss was invisible");
    eventually([&] { return collector.stream_health().reconnects == 1; }, "collector did not reconnect");
    require(!collector.stream_health().fresh, "collector reused old connection freshness");
    server.send(sentum::test::kline_message());
    eventually([&] { return collector.enqueued_count() == 2; }, "collector did not accept post-reconnect kline");
    collector.stop();
    require(database.load_klines("btcusdt", 10).size() == 2, "collector did not drain both connections into SQLite");
    require(!collector.stream_health().worker_running && !collector.stream_health().connected,
            "collector stop retained transport health");
}

void initial_failure_and_cancellation(const sentum::test::LocalTlsIdentity& identity) {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reservation(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto port = reservation.local_endpoint().port();
    reservation.close();
    const auto url = "wss://localhost:" + std::to_string(port) + "/market";
    BinanceWebsocketClient client("BTCUSDT");
    MarketStreamTestAccess::configure(client, url);
    client.start();
    eventually([&] { return client.stream_health().connection_attempts >= 2; }, "initial refusal was not retried");
    require(!client.stream_health().connected && !client.stream_health().fresh, "refused connection reported readiness");
    sentum::test::LocalTlsMarketServer server(identity, port);
    eventually([&] { return client.stream_health().connected; }, "client did not recover when endpoint became available");
    client.stop();

    BinanceWebsocketClient backing_off("BTCUSDT");
    auto settings = MarketStreamTestAccess::settings();
    settings.initial_backoff = settings.maximum_backoff = 10s;
    MarketStreamTestAccess::configure(backing_off, "wss://localhost:1/market", "localhost", settings);
    backing_off.start();
    eventually([&] { return backing_off.stream_health().state == "backoff"; }, "retry fixture never entered backoff");
    const auto before_stop = std::chrono::steady_clock::now();
    backing_off.stop();
    require(std::chrono::steady_clock::now() - before_stop < 1s, "stop waited for the reconnect backoff");

    boost::asio::ip::tcp::acceptor silent(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    BinanceWebsocketClient handshake("BTCUSDT");
    MarketStreamTestAccess::configure(handshake, "wss://localhost:" + std::to_string(silent.local_endpoint().port()) + "/market");
    handshake.start();
    eventually([&] { return handshake.stream_health().connection_attempts == 1; }, "handshake fixture did not start");
    const auto before_handshake_stop = std::chrono::steady_clock::now();
    handshake.stop();
    require(std::chrono::steady_clock::now() - before_handshake_stop < 1s, "handshake cancellation exceeded one second");
}

void tls_identity_stays_required(const sentum::test::LocalTlsIdentity& identity) {
    sentum::test::LocalTlsMarketServer server(identity);
    BinanceWebsocketClient client("BTCUSDT");
    MarketStreamTestAccess::configure(client, server.url(), "wrong.invalid");
    std::atomic<int> delivered{0};
    client.set_on_price([&](double) { ++delivered; });
    client.start();
    eventually([&] { return client.stream_health().connection_attempts >= 2; }, "TLS rejection was not retried");
    require(client.stream_health().successful_connections == 0 && delivered.load() == 0,
            "retry weakened TLS hostname validation");
    client.stop();
}

void position_waits_for_valid_recovery_price(const sentum::test::LocalTlsIdentity& identity) {
    sentum::test::LocalTlsMarketServer server(identity);
    BinanceRestClient api("", "");
    RiskConfig risk;
    risk.buy_fee_percent = risk.sell_fee_percent = risk.slippage_percent = risk.spread_percent = 0.0;
    risk.max_data_age_ms = 500;
    sentum::runtime::RuntimeControl::global().configure({{"type", "momentum"}}, false, "BTCUSDT");
    sentum::runtime::RuntimeControl::global().pause_entries(false);
    std::atomic<bool> collector_fresh{false};
    std::atomic<int> processed{0};
    TradeEngine engine("BTCUSDT", risk, std::make_shared<SystemClock>(), std::make_unique<BuyStrategy>(), ":memory:");
    engine.set_entry_readiness([&] { return collector_fresh.load(); });
    TradeEngineTestAccess::attach_live_stream(engine, api, server.url(), processed);
    eventually([&] { return engine.price_stream_health().connected; }, "position fixture did not connect");
    server.send(sentum::test::trade_message());
    eventually([&] { return processed.load(std::memory_order_acquire) == 1; }, "position fixture did not process first price");
    require(!engine.get_current_position().open, "stale collector allowed a new position");
    collector_fresh.store(true);
    server.send(sentum::test::trade_message());
    eventually([&] { return engine.get_current_position().open; }, "fresh streams did not allow the fixture entry");
    server.drop_connections();
    eventually([&] { return !engine.price_stream_health().connected; }, "position fixture did not observe socket loss");
    collector_fresh.store(false);
    require(engine.evaluate(90.0) == TradeAction::NONE && engine.get_current_position().open,
            "disconnected position was synthetically closed using an unverified price");
    eventually([&] { return engine.price_stream_health().reconnects >= 1; }, "position stream did not reconnect");
    server.send(sentum::test::trade_message(90.0, sentum::test::unix_milliseconds() - 10000));
    server.send(sentum::test::trade_message(100.0));
    eventually([&] { return processed.load(std::memory_order_acquire) == 3; }, "fresh recovery event was not processed");
    require(engine.get_current_position().open, "old buffered event closed the position on reconnect");
    server.send(sentum::test::trade_message(90.0));
    eventually([&] { return engine.get_total_trades() == 1; }, "fresh gap price did not manage the existing stop");
    require(!engine.get_current_position().open && engine.get_total_profit() < 0.0,
            "risk management did not resume for an open position while collector entries remained blocked");
    engine.stop();
}

void invalid_freshness_configuration() {
    for (const auto age : {std::int64_t{0}, std::int64_t{-1}, std::numeric_limits<std::int64_t>::max()}) {
        bool rejected = false;
        try { BinanceWebsocketClient invalid("BTCUSDT", std::chrono::milliseconds(age)); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid market freshness budget was accepted");
    }
}

void collector_lifecycle_cancellation() {
    Database database(":memory:");
    MarketDataStore store(600);
    Collector collector(database, store, {MarketInfo{"BTCUSDT", "BTC", 8, "USDT", 8}});
    collector.stop();
    MarketStreamTestAccess::before_connect(collector, [&] { collector.stop(); });
    std::vector<std::thread> callers;
    for (int i = 0; i < 4; ++i) {
        callers.emplace_back([&] {
            for (int cycle = 0; cycle < 8; ++cycle) { collector.start(); collector.stop(); }
        });
    }
    for (auto& caller : callers) caller.join();
    collector.stop();
    require(!collector.stream_health().worker_running && !collector.stream_health().connected,
            "concurrent collector stop/start retained a worker");
}

void failed_storage_cancels_transport_and_runtime(const sentum::test::LocalTlsIdentity& identity) {
    TemporaryDatabase temporary;
    sentum::test::LocalTlsMarketServer server(identity);
    ExecutionEngine engine;
    auto& collector = ExecutionEngineTestAccess::prepare(engine, temporary.path.string(), server.url());
    SqliteControl control(temporary.path);
    control.execute("CREATE TRIGGER reject_writes BEFORE INSERT ON klines "
                    "BEGIN SELECT RAISE(ABORT, 'fixture permanent write failure'); END;");
    ExecutionEngineTestAccess::start(engine);
    eventually([&] { return collector.stream_health().connected; }, "storage failure fixture did not connect");
    server.send(sentum::test::kline_message());
    eventually([&] { return collector.persistence_failed(); }, "actual TLS candle did not expose SQLite failure");
    eventually([&] { return !collector.stream_health().worker_running; }, "failed writer did not cancel actual transport worker");
    eventually([&] { return !engine.is_running(); }, "coordinator kept running after collector storage failure");
    const auto attempts = collector.stream_health().connection_attempts;
    const auto before_stop = std::chrono::steady_clock::now();
    bool propagated = false;
    try { engine.stop(); }
    catch (const std::runtime_error& error) {
        propagated = std::string(error.what()).find("unrecovered=1") != std::string::npos;
    }
    require(propagated, "runtime stop swallowed the collector failure or its accounting");
    require(std::chrono::steady_clock::now() - before_stop < 1s, "storage failure cleanup exceeded one second");
    require(ExecutionEngineTestAccess::joined(engine), "runtime error escaped before every worker joined");
    const auto status = collector.persistence_snapshot();
    require(status.at("accepted") == 1 && status.at("persisted") == 0 && status.at("unrecovered") == 1 &&
            status.at("drained") == false && status.at("writer_active") == false,
            "fatal TLS ingestion did not preserve exact failed-write accounting");
    require(MarketStreamTestAccess::retained_canonical_batch(collector, 1), "fatal writer lost its canonical pending batch");
    bool restart_rejected = false;
    try { collector.start(); } catch (const std::runtime_error&) { restart_rejected = true; }
    require(restart_rejected && collector.stream_health().connection_attempts == attempts && !collector.stream_health().connected,
            "terminal storage failure restarted market intake");
    const auto dashboard = sentum::dashboard::DashboardState::global().snapshot();
    require(dashboard.at("health") == "error" && dashboard.at("collector_persistence") == status &&
            dashboard.at("persistence_healthy") == false && dashboard.at("entries_blocked_by_persistence") == true,
            "dashboard lost the final persistence error after cleanup");
    require(ExecutionEngineTestAccess::database(engine).load_klines("btcusdt").empty(), "rejected TLS candle appeared durable");
}

void storage_failure_during_shutdown_preserves_other_errors(const sentum::test::LocalTlsIdentity& identity) {
    TemporaryDatabase temporary;
    WriteAttemptGate gate(1);
    sentum::test::LocalTlsMarketServer server(identity);
    ExecutionEngine engine;
    auto& collector = ExecutionEngineTestAccess::prepare(engine, temporary.path.string(), server.url());
    SqliteControl control(temporary.path);
    control.execute("CREATE TRIGGER reject_writes BEFORE INSERT ON klines "
                    "BEGIN SELECT RAISE(ABORT, 'fixture shutdown write failure'); END;");
    DatabaseTestAccess::trace(ExecutionEngineTestAccess::database(engine), WriteAttemptGate::trace, &gate);
    ReleaseWriteGate release{gate};
    ExecutionEngineTestAccess::start(engine, true);
    eventually([&] { return collector.stream_health().connected; }, "shutdown failure fixture did not connect");
    server.send(sentum::test::kline_message());
    require(gate.wait(), "writer did not reach controlled shutdown boundary");
    auto stopping = std::async(std::launch::async, [&] {
        try { engine.stop(); return std::string{}; }
        catch (const std::exception& error) { return std::string(error.what()); }
    });
    try {
        eventually([&] { return MarketStreamTestAccess::joining(collector); }, "shutdown did not join the gated writer");
        require(!engine.is_running() && !collector.persistence_failed(), "storage failure was not isolated to shutdown drain");
    } catch (...) { gate.release(); if (stopping.valid()) stopping.wait(); throw; }
    gate.release();
    const auto primary = stopping.get();
    require(primary == "fixture trader finalization failed", "collector failure replaced the primary trader diagnostic");
    require(collector.persistence_failed() && ExecutionEngineTestAccess::joined(engine), "shutdown did not retain storage failure after joining");
    const auto dashboard = sentum::dashboard::DashboardState::global().snapshot();
    const auto diagnostic = dashboard.at("last_error").get<std::string>();
    require(dashboard.at("health") == "error" && diagnostic.find("fixture account settlement failed") != std::string::npos &&
            diagnostic.find("collector persistence also failed") != std::string::npos && diagnostic.find("unrecovered=1") != std::string::npos,
            "combined finalization lost an account or collector failure diagnostic");
}

void retrying_writer_rejects_restart_after_join(const sentum::test::LocalTlsIdentity& identity) {
    TemporaryDatabase temporary;
    WriteAttemptGate gate(2);
    sentum::test::LocalTlsMarketServer server(identity);
    Database database(temporary.path.string());
    MarketDataStore store;
    Collector collector(database, store, {{"BTCUSDT", "BTC", 8, "USDT", 8}});
    SqliteControl locker(temporary.path);
    locker.execute("BEGIN IMMEDIATE;");
    DatabaseTestAccess::trace(database, WriteAttemptGate::trace, &gate);
    MarketStreamTestAccess::configure(collector, server.url());
    ReleaseWriteGate release{gate};
    collector.start();
    eventually([&] { return collector.stream_health().connected; }, "restart fixture did not connect");
    server.send(sentum::test::kline_message());
    require(gate.wait(), "retrying writer did not reach controlled restart boundary");
    require(!collector.persistence_healthy() && !collector.persistence_failed(), "restart fixture was not retrying");
    auto stopping = std::async(std::launch::async, [&] { collector.stop(); });
    try {
        eventually([&] { return MarketStreamTestAccess::joining(collector); }, "stop did not begin joining the retrying writer");
        std::promise<void> attempted;
        auto attempted_future = attempted.get_future();
        auto restarting = std::async(std::launch::async, [&] {
            attempted.set_value();
            try { collector.start(); return false; }
            catch (const std::runtime_error&) { return true; }
        });
        attempted_future.wait();
        const bool waited_for_join = restarting.wait_for(50ms) != std::future_status::ready;
        gate.release();
        const bool rejected = restarting.get();
        stopping.get();
        require(waited_for_join && rejected, "concurrent start bypassed the failure latched by the old writer");
    } catch (...) { gate.release(); if (stopping.valid()) stopping.wait(); throw; }
    require(collector.persistence_failed() && MarketStreamTestAccess::joined(collector), "restart replaced a failed collector worker");
    require(collector.persistence_snapshot().at("unrecovered") == 1 &&
            MarketStreamTestAccess::retained_canonical_batch(collector, 1), "restart lost retained failed candle state");
    locker.execute("ROLLBACK;");
}

void operator_health_exposes_stale_transport() {
    const nlohmann::json snapshot = {{"health", "healthy"}, {"market_data_connected", true}, {"market_data_fresh", false}};
    const auto status = sentum::ui::derive_operational_status(snapshot);
    require(status.state == sentum::ui::OperationalState::Degraded, "stale connected stream appeared ready");
    const auto alerts = sentum::ui::derive_operator_alerts(snapshot);
    require(std::any_of(alerts.begin(), alerts.end(), [](const auto& alert) { return alert.id == "market.stale"; }),
            "operator alert omitted stale market data");
}

void operator_health_exposes_storage_failure() {
    nlohmann::json snapshot = {{"health", "healthy"}, {"market_data_connected", true}, {"market_data_fresh", true},
                               {"collector_persistence", {{"status", "RETRYING"}}}};
    require(sentum::ui::derive_operational_status(snapshot).state == sentum::ui::OperationalState::Degraded,
            "retrying persistence appeared ready");
    auto alerts = sentum::ui::derive_operator_alerts(snapshot);
    const auto contains = [&](sentum::ui::OperatorAlertSeverity severity) {
        return std::any_of(alerts.begin(), alerts.end(), [&](const auto& alert) {
            return alert.id == "persistence.write_failure" && alert.severity == severity;
        });
    };
    require(contains(sentum::ui::OperatorAlertSeverity::Warning), "retrying persistence omitted its warning");
    snapshot["collector_persistence"]["status"] = "FAILED";
    require(sentum::ui::derive_operational_status(snapshot).state == sentum::ui::OperationalState::Halted,
            "failed persistence did not halt operator readiness");
    alerts = sentum::ui::derive_operator_alerts(snapshot);
    require(contains(sentum::ui::OperatorAlertSeverity::Critical), "failed persistence omitted its critical alert");
}
}

int main() {
    try {
        sentum::test::LocalTlsIdentity identity;
        client_reconnect_and_freshness(identity);
        collector_reconnect_and_drain(identity);
        initial_failure_and_cancellation(identity);
        tls_identity_stays_required(identity);
        position_waits_for_valid_recovery_price(identity);
        invalid_freshness_configuration();
        collector_lifecycle_cancellation();
        failed_storage_cancels_transport_and_runtime(identity);
        storage_failure_during_shutdown_preserves_other_errors(identity);
        retrying_writer_rejects_restart_after_join(identity);
        operator_health_exposes_stale_transport();
        operator_health_exposes_storage_failure();
        std::cout << "Market stream recovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Market stream recovery failure: " << error.what() << '\n';
        return 1;
    }
}
