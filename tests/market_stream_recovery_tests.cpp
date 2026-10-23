#include "support/LocalTlsMarketServer.hpp"
#include "support/MarketStreamFixtures.hpp"

#include <sentum/core/RuntimeControl.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/ui/OperationalSafetyPolicy.hpp>
#include <sentum/ui/OperatorAlertPolicy.hpp>

#include <atomic>
#include <iostream>
#include <limits>

struct TradeEngineTestAccess {
    static void attach_live_stream(TradeEngine& engine, BinanceRestClient& api, const std::string& url) {
        engine.api = &api;
        engine.run_started_ = true;
        engine.price_stream = std::make_unique<BinanceWebsocketClient>("BTCUSDT");
        MarketStreamTestAccess::configure(*engine.price_stream, url);
        engine.price_stream->set_on_trade([&engine](double price, std::chrono::system_clock::time_point timestamp) {
            MarketEvent event;
            event.symbol = "BTCUSDT";
            event.timestamp = timestamp;
            event.price = price;
            event.close = price;
            engine.process_event(event);
        });
        engine.price_stream->start();
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
    TradeEngine engine("BTCUSDT", risk, std::make_shared<SystemClock>(), std::make_unique<BuyStrategy>(), ":memory:");
    std::atomic<bool> collector_fresh{false};
    engine.set_entry_readiness([&] { return collector_fresh.load(); });
    TradeEngineTestAccess::attach_live_stream(engine, api, server.url());
    eventually([&] { return engine.price_stream_health().connected; }, "position fixture did not connect");
    server.send(sentum::test::trade_message());
    eventually([&] { return engine.price_stream_health().fresh; }, "position fixture did not receive first price");
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
    eventually([&] { return engine.price_stream_health().fresh; }, "fresh recovery event was not admitted");
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

void operator_health_exposes_stale_transport() {
    const nlohmann::json snapshot = {{"health", "healthy"}, {"market_data_connected", true}, {"market_data_fresh", false}};
    const auto status = sentum::ui::derive_operational_status(snapshot);
    require(status.state == sentum::ui::OperationalState::Degraded, "stale connected stream appeared ready");
    const auto alerts = sentum::ui::derive_operator_alerts(snapshot);
    require(std::any_of(alerts.begin(), alerts.end(), [](const auto& alert) { return alert.id == "market.stale"; }),
            "operator alert omitted stale market data");
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
        operator_health_exposes_stale_transport();
        std::cout << "Market stream recovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Market stream recovery failure: " << error.what() << '\n';
        return 1;
    }
}
