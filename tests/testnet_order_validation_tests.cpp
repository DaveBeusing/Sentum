#include <sentum/trader/order/LiveOrderSession.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using nlohmann::json;
using sentum::order::LiveOrderSession;
using sentum::order::Request;
using sentum::order::Side;
using namespace std::chrono_literals;
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action, const std::string& message) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
json lot(const std::string& type, const std::string& step, const std::string& minimum = "0.00001", const std::string& maximum = "100") {
    return {{"filterType", type}, {"minQty", minimum}, {"maxQty", maximum}, {"stepSize", step}};
}
json metadata() {
    return {{"symbols", json::array({{
        {"symbol", "BTCUSDT"}, {"status", "TRADING"}, {"isSpotTradingAllowed", true},
        {"baseAsset", "BTC"}, {"quoteAsset", "USDT"}, {"baseAssetPrecision", 8}, {"quoteAssetPrecision", 8},
        {"orderTypes", json::array({"MARKET", "LIMIT"})}, {"permissionSets", json::array({json::array({"SPOT"})})},
        {"filters", json::array({lot("LOT_SIZE", "0.00001"), lot("MARKET_LOT_SIZE", "0", "0", "0")})}}})}};
}
RiskConfig risk_config() {
    RiskConfig risk;
    risk.max_total_capital = 1000000.0;
    risk.risk_per_trade = 0.1;
    risk.min_notional = 0.01;
    risk.cooldown_seconds = 0;
    return risk;
}
class HealthyStream final : public IUserDataStream {
public:
    explicit HealthyStream(HealthHandler callback) : callback_(std::move(callback)) {}
    void start() override { running_.store(true); callback_(true); }
    void stop() override { running_.store(false); }
    bool running() const noexcept override { return running_.load(); }
    bool healthy() const noexcept override { return running(); }
    bool wait_until_healthy(std::chrono::milliseconds) override { return healthy(); }
private:
    HealthHandler callback_;
    std::atomic<bool> running_{false};
};
class Exchange final : public BinanceSpotExecutionClient {
public:
    Exchange() : BinanceSpotExecutionClient("fixture", "fixture") {}
    json exchange_info(const std::string& symbol) const override {
        require(symbol == "BTCUSDT", "metadata requested for wrong market");
        ++metadata_calls;
        return information;
    }
    json account() const override {
        return {{"canTrade", true}, {"accountType", "SPOT"}, {"permissions", permissions}, {"updateTime", 1000}, {"uid", 42},
            {"balances", json::array({{{"asset", "BTC"}, {"free", "0"}, {"locked", "0"}},
                                     {{"asset", "USDT"}, {"free", "1000000"}, {"locked", "0"}}})}};
    }
    json open_orders() const override { return json::array(); }
    json account_trades(const std::string&, std::int64_t, int) const override { return json::array(); }
    json ticker_price(const std::string&) const override { ++ticker_calls; return ticker; }
    json reference_price(const std::string&) const override {
        ++reference_calls;
        if (reference_error) throw BinanceRequestError(BinanceRequestError::Kind::Unknown, "fixture reference response", reference_error);
        return reference;
    }
    json average_price(const std::string&) const override { ++average_calls; return average; }
    json place_market_order(const Request& request) const override {
        ++placements;
        placed.push_back(request);
        ids[request.client_order_id] = placements;
        return {{"symbol", request.symbol}, {"clientOrderId", request.client_order_id}, {"orderId", placements}};
    }
    json cancel_order(const std::string& symbol, const std::string& client) const override {
        return {{"symbol", symbol}, {"origClientOrderId", client}, {"clientOrderId", "cancel-" + client},
            {"orderId", ids.at(client)}, {"side", "BUY"}, {"origQty", placed.back().quantity_text},
            {"executedQty", "0"}, {"cummulativeQuoteQty", "0"}, {"status", "CANCELED"}, {"updateTime", now_ms()}};
    }
    json query_order(const std::string&, const std::string&) const override { throw std::runtime_error("unexpected unresolved fixture order"); }
    json information = metadata();
    std::vector<std::string> permissions{"SPOT"};
    json ticker{{"symbol", "BTCUSDT"}, {"price", "100"}};
    json reference{{"symbol", "BTCUSDT"}, {"referencePrice", nullptr}, {"timestamp", now_ms()}};
    json average{{"mins", 5}, {"price", "100"}, {"closeTime", now_ms()}};
    int reference_error = 0;
    mutable int placements = 0, metadata_calls = 0, ticker_calls = 0, reference_calls = 0, average_calls = 0;
    mutable std::vector<Request> placed;
    mutable std::map<std::string, std::int64_t> ids;
};
class Session {
public:
    explicit Session(RiskConfig risk = risk_config()) {
        auto exchange = std::make_unique<Exchange>();
        client = exchange.get();
        LiveOrderSession::Timing timing;
        timing.supervisor_poll_interval = 1h;
        timing.subscription_timeout = 100ms;
        session = LiveOrderSession::from_dependencies(std::move(exchange),
            [](IUserDataStream::SubscriptionRequest, IUserDataStream::Handler, IUserDataStream::HealthHandler health) {
                return std::make_unique<HealthyStream>(std::move(health));
            }, timing, LiveOrderSession::Persistence::ephemeral());
        session->configure_market("BTCUSDT", risk);
        session->set_update_handler([this](const auto&) { ++updates; });
    }
    ~Session() { session->stop(); session.reset(); }
    void start() { session->start(); require(session->ready(), "valid flat Testnet fixture failed to become ready"); }
    void approve(double quantity, const std::string& expected) {
        const auto before = client->placements;
        const auto id = "order-" + std::to_string(++sequence);
        const auto result = session->submit(Request{"BTCUSDT", Side::Buy, quantity, id});
        require(client->placements == before + 1 && result.state == sentum::order::State::Acknowledged,
                "validated fixture request did not reach placement exactly once");
        const auto& actual = client->placed.back();
        require(actual.quantity_text == expected && actual.quantity <= quantity && actual.quantity > 0.0,
                "session sent incorrect or increased decimal quantity");
        require(actual.base_asset == "BTC" && actual.quote_asset == "USDT", "placement lacks verified asset provenance");
        session->cancel(id);
    }
    void reject(double quantity, const std::string& reason) {
        const auto before_placements = client->placements, before_updates = updates;
        rejects([&] { session->submit(Request{"BTCUSDT", Side::Buy, quantity, "blocked-" + std::to_string(++sequence)}); }, reason);
        require(client->placements == before_placements && updates == before_updates,
                "failed validation crossed intent or placement boundary: " + reason);
    }
    Exchange* client;
    std::unique_ptr<LiveOrderSession> session;
    int updates = 0, sequence = 0;
};
json minimum_notional(int minutes = 5) {
    return {{"filterType", "MIN_NOTIONAL"}, {"minNotional", "10"}, {"applyToMarket", true}, {"avgPriceMins", minutes}};
}
void add_filter(Session& fixture, json filter) { fixture.client->information["symbols"][0]["filters"].push_back(std::move(filter)); }

void fresh_metadata_and_joint_quantities() {
    Session fixture;
    fixture.start();
    require(fixture.client->metadata_calls > 0, "session readiness omitted symbol metadata");
    const auto startup_calls = fixture.client->metadata_calls;
    fixture.approve(1.23456789, "1.23456");
    require(fixture.client->metadata_calls > startup_calls, "submission reused startup metadata without refreshing");
    fixture.client->information["symbols"][0]["filters"][0]["stepSize"] = "0.1";
    fixture.approve(0.29, "0.2");
    fixture.client->information["symbols"][0]["filters"][0]["minQty"] = "0.5";
    fixture.reject(0.29, "below-minimum order was rounded upward");
    fixture.client->information["symbols"][0]["filters"][0]["minQty"] = "0.00001";
    fixture.client->information["symbols"][0]["filters"][0]["maxQty"] = "0.1";
    fixture.reject(0.29, "exchange maximum quantity ignored");

    auto risk = risk_config(); risk.step_size = 0.03;
    Session intersection(risk);
    intersection.client->information["symbols"][0]["filters"][0]["stepSize"] = "0.02";
    intersection.start();
    intersection.approve(0.29, "0.24");
    intersection.client->information["symbols"][0]["filters"][0]["stepSize"] = "0.025";
    intersection.approve(0.29, "0.15");
}

void schema_permissions_and_local_bounds() {
    Session fixture;
    fixture.start();
    const auto original = fixture.client->information;
    const std::vector<std::function<void(json&)>> invalid_metadata{
        [](json& value) { value["symbols"][0]["status"] = "HALT"; },
        [](json& value) { value["symbols"][0]["isSpotTradingAllowed"] = false; },
        [](json& value) { value["symbols"][0]["orderTypes"] = json::array({"LIMIT"}); },
        [](json& value) { value["symbols"][0]["filters"][0]["stepSize"] = "nan"; },
        [](json& value) { value["symbols"][0]["permissionSets"] = json::array({json::array({"SPOT"}), json::array({"TRD_GRP_004"})}); },
        [](json& value) { value = json::object(); }
    };
    for (const auto& change : invalid_metadata) {
        fixture.client->information = original;
        change(fixture.client->information);
        fixture.reject(1.0, "invalid fresh metadata or unavailable permissions accepted");
    }
    fixture.client->information = original;
    fixture.approve(1.0, "1");
    Session unsupported;
    unsupported.client->information["symbols"][0].erase("filters");
    rejects([&] { unsupported.session->start(); }, "startup without metadata succeeded");
    require(!unsupported.session->ready() && unsupported.client->placements == 0, "invalid startup authorized orders");
    Session permission;
    permission.client->information["symbols"][0]["permissionSets"] = json::array({json::array({"SPOT"}), json::array({"TRD_GRP_004", "TRD_GRP_005"})});
    permission.client->permissions.push_back("TRD_GRP_005");
    permission.start(); permission.approve(1.0, "1");
    auto local = risk_config(); local.max_total_capital = 100.0; local.risk_per_trade = 0.01;
    Session budget(local); budget.start(); budget.reject(2.0, "local capital/stop-risk limit ignored");
}

void reference_average_and_last_prices() {
    Session average;
    add_filter(average, minimum_notional());
    average.start();
    average.client->average["price"] = "20";
    average.reject(0.4, "last trade substituted for the required average price");
    average.client->average["price"] = "25";
    average.approve(0.4, "0.4");
    Session reference;
    add_filter(reference, minimum_notional()); reference.start();
    reference.client->reference["referencePrice"] = "100";
    reference.client->average["price"] = "1";
    reference.approve(0.4, "0.4");
    require(reference.client->average_calls == 0, "active reference price did not override average window");
    reference.client->reference["referencePrice"] = "20";
    reference.reject(0.4, "active reference-price minimum ignored");
    Session last;
    add_filter(last, minimum_notional(0)); last.start();
    last.client->reference_error = -2043;
    last.approve(0.1, "0.1");
    require(last.client->average_calls == 0 && last.client->ticker_calls >= 2, "zero-minute filter did not use last Testnet price");
    Session flags;
    add_filter(flags, {{"filterType", "NOTIONAL"}, {"minNotional", "20"}, {"maxNotional", "100"},
        {"applyMinToMarket", false}, {"applyMaxToMarket", true}, {"avgPriceMins", 0}});
    flags.start(); flags.approve(0.15, "0.15"); flags.reject(1.1, "market maximum notional ignored");
    Session disabled;
    add_filter(disabled, {{"filterType", "NOTIONAL"}, {"minNotional", "20"}, {"maxNotional", "100"},
        {"applyMinToMarket", false}, {"applyMaxToMarket", false}, {"avgPriceMins", 5}});
    disabled.start(); disabled.approve(0.1, "0.1");
    require(disabled.client->reference_calls == 0 && disabled.client->average_calls == 0,
            "disabled market filters demanded unrelated price evidence");
}

void malformed_and_stale_market_evidence() {
    Session fixture;
    add_filter(fixture, minimum_notional()); fixture.start();
    const auto average = fixture.client->average, reference = fixture.client->reference, ticker = fixture.client->ticker;
    for (const auto& bad : {json("nan"), json("inf"), json("1tail"), json("0"), json("-1"), json(nullptr)}) {
        fixture.client->average = average; fixture.client->average["price"] = bad;
        fixture.reject(1.0, "invalid average market price accepted");
    }
    fixture.client->average = average; fixture.client->average["mins"] = 6;
    fixture.reject(1.0, "mismatched average window accepted");
    fixture.client->average = average; fixture.client->average["closeTime"] = now_ms() - 60000;
    fixture.reject(1.0, "stale average price accepted");
    fixture.client->average = average; fixture.client->average["closeTime"] = now_ms() + 60000;
    fixture.reject(1.0, "future average price accepted");
    fixture.client->average = average; fixture.client->average.erase("closeTime");
    fixture.reject(1.0, "missing average evidence timestamp accepted");
    fixture.client->average = average;
    fixture.client->reference["referencePrice"] = "100"; fixture.client->reference["timestamp"] = now_ms() - 60000;
    fixture.reject(1.0, "stale reference price accepted");
    fixture.client->reference = reference; fixture.client->reference["symbol"] = "ETHUSDT";
    fixture.reject(1.0, "wrong-market reference price accepted");
    fixture.client->reference = reference; fixture.client->reference.erase("timestamp");
    fixture.reject(1.0, "incomplete null reference-price schema accepted");
    fixture.client->reference = reference; fixture.client->reference_error = -1007;
    fixture.reject(1.0, "reference-price failure was treated as confirmed absence");
    fixture.client->reference_error = 0;
    fixture.client->ticker["symbol"] = "ETHUSDT";
    fixture.reject(1.0, "wrong-market local risk ticker accepted");
    fixture.client->ticker = ticker; fixture.client->ticker["price"] = "nan";
    fixture.reject(1.0, "invalid local risk ticker accepted");
    fixture.client->ticker = ticker; fixture.client->reference = reference; fixture.client->average = average;
    fixture.approve(1.0, "1");
}
} // namespace

int main() {
    try {
        fresh_metadata_and_joint_quantities(); schema_permissions_and_local_bounds();
        reference_average_and_last_prices(); malformed_and_stale_market_evidence();
        std::cout << "Testnet order validation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Testnet order validation tests failed: " << error.what() << '\n';
        return 1;
    }
}
