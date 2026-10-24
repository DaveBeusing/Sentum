#include <sentum/trader/order/LiveOrderSession.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using nlohmann::json;
using sentum::order::LiveOrderSession;
using sentum::order::OrderEventRepository;
using sentum::order::Side;
using sentum::order::Snapshot;
using sentum::order::State;
using sentum::execution::ExecutionRecoveryState;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename F> void rejects(F operation, const std::string& message) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
void near(double actual, double expected, const std::string& message) {
    require(std::abs(actual - expected) < 1e-10 * std::max(1.0, std::abs(expected)), message);
}
struct Workspace {
    std::filesystem::path root;
    Workspace() {
        auto pattern = (std::filesystem::temp_directory_path() / "sentum-session-recovery-XXXXXX").string();
        std::vector<char> chars(pattern.begin(), pattern.end());
        chars.push_back('\0');
        const char* directory = ::mkdtemp(chars.data());
        if (!directory) throw std::runtime_error("Cannot create recovery fixture directory");
        root = directory;
    }
    ~Workspace() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::string database() const { return (root / "execution.sqlite3").string(); }
    std::string identity() const { return OrderEventRepository::account_fingerprint(root.string()); }
};
void sql(const std::string& path, const char* statement) {
    sqlite3* db = nullptr;
    require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "Cannot open recovery fixture database");
    const int status = sqlite3_exec(db, statement, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(status == SQLITE_OK, "Recovery fixture SQL failed");
}
json read_json(const std::string& path, const char* query) {
    sqlite3* db = nullptr;
    require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "Cannot open recovery proof database");
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(db, query, -1, &statement, nullptr) == SQLITE_OK, "Cannot prepare recovery proof query");
    require(sqlite3_step(statement) == SQLITE_ROW, "Recovery proof query returned no row");
    const std::string payload(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return json::parse(payload);
}
std::int64_t scalar(const std::string& path, const char* query) {
    sqlite3* db = nullptr;
    require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "Cannot open recovery counter database");
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(db, query, -1, &statement, nullptr) == SQLITE_OK, "Cannot prepare recovery counter query");
    require(sqlite3_step(statement) == SQLITE_ROW, "Recovery counter query returned no row");
    const auto value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return value;
}

json account(std::string quantity = "0") {
    return {{"canTrade", true}, {"accountType", "SPOT"}, {"permissions", {"SPOT"}},
        {"updateTime", 2000}, {"uid", 123}, {"balances", json::array({
            {{"asset", "BTC"}, {"free", quantity}, {"locked", "0"}},
            {{"asset", "USDT"}, {"free", "10000"}, {"locked", "0"}}
        })}};
}
Snapshot owned(double executed = 1.0, bool incomplete = false) {
    Snapshot value;
    value.symbol = "BTCUSDT";
    value.client_order_id = "owned-buy";
    value.exchange_order_id = 71;
    value.side = Side::Buy;
    value.state = executed == 1.0 ? State::Filled : State::PartiallyFilled;
    value.requested_quantity = 1.0;
    value.executed_quantity = executed;
    value.cumulative_quote_quantity = executed * 100.0;
    value.average_fill_price = 100.0;
    value.base_asset = "BTC";
    value.quote_asset = "USDT";
    value.locally_owned = true;
    value.requires_reconciliation = incomplete;
    value.last_event_time_ms = executed == 1.0 ? 2000 : 1000;
    value.updated_at = std::chrono::system_clock::time_point(std::chrono::milliseconds(value.last_event_time_ms));
    if (!incomplete) {
        value.execution_trade_quantities.emplace(900, executed);
        value.commissions.emplace(900, sentum::order::ExecutionCommission{"BTC", 0.001});
    }
    return value;
}
json order_summary() {
    return {{"symbol", "BTCUSDT"}, {"clientOrderId", "owned-buy"}, {"orderId", 71},
        {"side", "BUY"}, {"status", "FILLED"}, {"origQty", "1"}, {"executedQty", "1"},
        {"cummulativeQuoteQty", "100"}, {"updateTime", 2000}};
}
json trade(std::int64_t id = 900, std::string quantity = "1", std::string quote = "100",
           std::int64_t time = 2000, std::string fee = "0.001") {
    return {{"symbol", "BTCUSDT"}, {"id", id}, {"orderId", 71}, {"price", "100"},
        {"qty", quantity}, {"quoteQty", quote}, {"commission", fee}, {"commissionAsset", "BTC"},
        {"time", time}, {"isBuyer", true}};
}
json execution_event(const std::string& symbol, const std::string& client = "external-order", std::int64_t order_id = 999) {
    return {{"e", "executionReport"}, {"s", symbol}, {"c", client}, {"i", order_id},
        {"S", "BUY"}, {"X", "FILLED"}, {"q", "1"}, {"z", "1"}, {"Z", "100"},
        {"l", "1"}, {"t", 1001}, {"n", "0"}, {"N", "USDT"}, {"T", 3000}, {"E", 3000}};
}
void seed(Workspace& workspace, const Snapshot& snapshot) {
    OrderEventRepository repository(workspace.database(), workspace.identity());
    repository.save_order(snapshot, "exchange");
    repository.finish_session();
}

struct StreamState {
    IUserDataStream::Handler event;
    IUserDataStream::HealthHandler health;
    std::atomic<bool> running{false}, healthy{false};
    void emit(const json& value) { if (event) event(value); }
    void disconnect() {
        // The worker intentionally remains active, exercising the distinction
        // between a live thread and an authenticated subscription.
        healthy.store(false);
        if (health) health(false);
    }
};
class Stream final : public IUserDataStream {
public:
    Stream(std::shared_ptr<StreamState> state, bool acknowledge)
        : state_(std::move(state)), acknowledge_(acknowledge) {}
    void start() override {
        state_->running.store(true);
        state_->healthy.store(acknowledge_);
        if (acknowledge_ && state_->health) state_->health(true);
    }
    void stop() override { state_->running.store(false); state_->healthy.store(false); }
    bool running() const noexcept override { return state_->running.load(); }
    bool healthy() const noexcept override { return state_->healthy.load(); }
    bool wait_until_healthy(std::chrono::milliseconds) override { return healthy(); }
private:
    std::shared_ptr<StreamState> state_;
    bool acknowledge_;
};
struct Streams {
    std::mutex mutex;
    std::vector<std::shared_ptr<StreamState>> values;
    std::atomic<bool> acknowledge{true};
    LiveOrderSession::StreamFactory factory() {
        return [this](IUserDataStream::SubscriptionRequest, IUserDataStream::Handler handler,
                      IUserDataStream::HealthHandler health) {
            auto state = std::make_shared<StreamState>();
            state->event = std::move(handler);
            state->health = std::move(health);
            { std::lock_guard<std::mutex> lock(mutex); values.push_back(state); }
            return std::make_unique<Stream>(std::move(state), acknowledge.load());
        };
    }
    std::shared_ptr<StreamState> latest() {
        std::lock_guard<std::mutex> lock(mutex);
        require(!values.empty(), "Missing fixture subscription");
        return values.back();
    }
};
class Client final : public BinanceSpotExecutionClient {
public:
    Client() : BinanceSpotExecutionClient("fixture-key", "fixture-secret") {}
    json account() const override {
        const int call = ++account_calls;
        if (account_hook) account_hook(call);
        return account_response;
    }
    json open_orders() const override { return json::array(); }
    json account_trades(const std::string& symbol, std::int64_t from_id, int limit) const override {
        require(symbol == "BTCUSDT", "Unexpected reconciliation market");
        json result = json::array();
        for (const auto& value : trades) {
            if (value.at("id").get<std::int64_t>() >= from_id) result.push_back(value);
            if (result.size() == static_cast<std::size_t>(limit)) break;
        }
        return result;
    }
    json query_order(const std::string& symbol, const std::string& client) const override {
        require(symbol == "BTCUSDT" && client == "owned-buy", "Recovery queried an unrelated order");
        return order_summary();
    }
    json exchange_info(const std::string& symbol) const override {
        return {{"symbols", json::array({{{"symbol", symbol}, {"baseAsset", "BTC"}, {"quoteAsset", "USDT"},
            {"status", "TRADING"}, {"isSpotTradingAllowed", true}, {"baseAssetPrecision", 8}, {"quoteAssetPrecision", 8},
            {"orderTypes", {"MARKET"}}, {"permissionSets", {{"SPOT"}}},
            {"filters", json::array({{{"filterType", "LOT_SIZE"}, {"minQty", "0.000001"},
                {"maxQty", "1000"}, {"stepSize", "0.000001"}}})}}})}};
    }
    json place_market_order(const sentum::order::Request&) const override {
        ++placements;
        throw std::runtime_error("Recovery must not place an order");
    }
    json ticker_price(const std::string& symbol) const override { return {{"symbol", symbol}, {"price", "100"}}; }
    json account_response = ::account();
    json trades = json::array();
    std::function<void(int)> account_hook;
    mutable std::atomic<int> account_calls{0}, placements{0};
};
std::unique_ptr<LiveOrderSession> make_session(Workspace& workspace, Streams& streams, std::unique_ptr<Client> client) {
    LiveOrderSession::Timing timing;
    timing.supervisor_poll_interval = 1s;
    timing.subscription_timeout = 10ms;
    auto session = LiveOrderSession::from_dependencies(std::move(client), streams.factory(), timing,
        LiveOrderSession::Persistence::durable(workspace.database(), workspace.identity()));
    session->configure_market("BTCUSDT", RiskConfig{});
    return session;
}
json proof() {
    return {{"format", 1}, {"started_ms", 1000}, {"finished_ms", 2000}, {"account_uid", 123},
        {"account_permissions", {"SPOT"}}, {"details", "Complete fixture account proof"}};
}

void restart_restores_ledger_and_strategy_before_readiness() {
    Workspace workspace;
    seed(workspace, owned());
    Streams streams;
    auto client = std::make_unique<Client>();
    auto* observed = client.get();
    client->account_response = account("0.999");
    client->trades = json::array({trade()});
    auto session = make_session(workspace, streams, std::move(client));
    int restores = 0;
    session->set_recovery_handler([&](const ExecutionRecoveryState& state) {
        ++restores;
        require(!session->ready(), "Session became ready before strategy restoration");
        require(state.position && state.orders.size() == 1 && state.replay.size() == 1, "Strategy received incomplete restoration");
        near(state.position->quantity, 0.999, "Strategy quantity differs from confirmed base fees");
        near(session->confirmed_position("BTCUSDT")->quantity, state.position->quantity, "Session and strategy ledgers differ");
        require(state.entry_time == std::chrono::system_clock::time_point(2000ms), "Confirmed entry time not restored");
    });
    session->start();
    require(restores == 1 && session->reconciliation_complete() && session->killed() && !session->ready(),
            "Restart cleared the durable latch or failed restoration");
    require(session->resume_after_reconcile("I_CONFIRM_RECONCILED_TESTNET_RESUME"), "Verified explicit resume failed");
    require(session->ready() && restores == 2 && observed->placements == 0, "Resume failed to preserve restored inventory");
    const auto evidence = read_json(workspace.database(), "SELECT proof FROM execution_reconciliations ORDER BY id DESC LIMIT 1");
    require(evidence.at("account_uid") == 123 && evidence.at("finished_ms") >= evidence.at("started_ms"),
            "Durable reconciliation omitted account identity or collection interval");
    session->stop();
}

void missed_outage_fills_and_fees_replace_incomplete_history_once() {
    Workspace workspace;
    auto partial = owned(0.4, true);
    partial.execution_trade_quantities.emplace(900, 0.4);
    partial.commissions.emplace(900, sentum::order::ExecutionCommission{"BTC", 0.001});
    seed(workspace, partial);
    Streams streams;
    auto client = std::make_unique<Client>();
    client->account_response = account("0.997");
    client->trades = json::array({trade(900, "0.4", "40", 1000), trade(901, "0.6", "60", 2000, "0.002")});
    auto session = make_session(workspace, streams, std::move(client));
    std::optional<ExecutionRecoveryState> restored;
    session->set_recovery_handler([&](const ExecutionRecoveryState& state) { restored = state; });
    session->start();
    require(restored && restored->position && restored->replay.size() == 2, "Missed outage fill was not restored");
    near(restored->position->quantity, 0.997, "Outage commission history did not adjust owned inventory");
    require(!session->get("owned-buy")->requires_reconciliation && session->get("owned-buy")->commissions.size() == 2,
            "Complete proof did not durably repair the incomplete marker");
    require(session->resume_after_reconcile("I_CONFIRM_RECONCILED_TESTNET_RESUME"), "Repaired outage proof could not resume");
    near(session->confirmed_position("BTCUSDT")->quantity, 0.997, "Repeated recovery duplicated inventory");
    session->stop();
}

void concurrent_events_invalidate_proof_and_retry_is_bounded() {
    Workspace workspace;
    Streams streams;
    auto client = std::make_unique<Client>();
    auto* observed = client.get();
    client->account_hook = [&](int) {
        streams.latest()->emit({{"e", "outboundAccountPosition"}, {"B", json::array({{{"a", "USDT"}, {"f", "10000"}, {"l", "0"}}})}});
    };
    auto session = make_session(workspace, streams, std::move(client));
    int restores = 0;
    session->set_recovery_handler([&](const ExecutionRecoveryState&) { ++restores; });
    session->start();
    require(!session->ready() && session->killed() && !session->reconciliation_complete() && restores == 0,
            "Moving stream evidence authorized a restoration");
    require(observed->account_calls == 6, "Reconciliation retried outside its three complete-attempt bound");
    require(scalar(workspace.database(), "SELECT COUNT(*) FROM execution_reconciliations") == 0,
            "Changed stream proof was committed");
    session->stop();
}

void disconnect_before_or_after_recovery_revokes_admission_immediately() {
    {
        Workspace workspace;
        Streams streams;
        auto client = std::make_unique<Client>();
        client->account_hook = [&](int call) { if (call == 1) streams.latest()->disconnect(); };
        auto session = make_session(workspace, streams, std::move(client));
        rejects([&] { session->start(); }, "Subscription loss during REST collection was accepted");
        require(!session->ready() && session->killed() && !session->reconciliation_complete(),
                "Lost subscription retained reconciliation readiness");
        session->stop();
    }
    {
        Workspace workspace;
        Streams streams;
        auto session = make_session(workspace, streams, std::make_unique<Client>());
        session->start();
        require(session->ready(), "Fresh flat session failed to become ready");
        auto previous = streams.latest();
        streams.acknowledge.store(false);
        previous->disconnect();
        require(previous->running.load(), "Fixture did not preserve worker state after disconnect");
        require(!session->ready() && session->killed() && !session->user_stream_healthy(),
                "Worker liveness disguised authenticated subscription loss");
        rejects([&] { session->submit({"BTCUSDT", Side::Buy, 1.0, "blocked-buy"}); },
                "Disconnected session accepted a new order");
        session->stop();
    }
}

void unrelated_account_activity_never_mutates_the_strategy_position() {
    for (const std::string symbol : {"BTCUSDT", "ETHUSDT"}) {
        Workspace workspace;
        Streams streams;
        auto session = make_session(workspace, streams, std::make_unique<Client>());
        session->start();
        require(session->ready(), "Fresh session unexpectedly blocked");
        streams.latest()->emit(execution_event(symbol));
        require(!session->ready() && session->killed(), "Unowned account activity did not block admission");
        require(!session->confirmed_position("BTCUSDT"), "Unrelated order became the strategy inventory");
        require(!session->reconciliation_complete(), "Unowned activity left stale reconciliation success");
        session->stop();
    }
}

void audit_failure_rolls_back_every_replacement_and_blocks_publication() {
    Workspace workspace;
    seed(workspace, owned(1.0, true));
    Streams streams;
    auto client = std::make_unique<Client>();
    client->account_response = account("0.999");
    client->trades = json::array({trade()});
    auto session = make_session(workspace, streams, std::move(client));
    sql(workspace.database(), "CREATE TRIGGER fail_recovery BEFORE INSERT ON execution_reconciliations BEGIN SELECT RAISE(ABORT, 'fixture recovery write failure'); END;");
    const auto count = scalar(workspace.database(), "SELECT COUNT(*) FROM execution_order_history");
    int published = 0;
    session->set_recovery_handler([&](const ExecutionRecoveryState&) { ++published; });
    rejects([&] { session->start(); }, "Failed reconciliation transaction was accepted");
    require(!session->ready() && session->killed() && !session->reconciliation_complete() && published == 0,
            "Failed durable proof published strategy restoration");
    require(scalar(workspace.database(), "SELECT COUNT(*) FROM execution_order_history") == count,
            "Failed audit left partial replacement history");
    const auto stored = read_json(workspace.database(), "SELECT payload FROM execution_order_snapshots LIMIT 1");
    require(stored.at("requires_reconciliation").get<bool>() && stored.at("commissions").empty(),
            "Failed transaction cleared incomplete execution proof");
    session->stop();
}

void journal_rejects_forged_and_incomplete_replay_before_mutating_state() {
    Workspace workspace;
    OrderEventRepository repository(workspace.database(), workspace.identity());
    repository.save_order(owned(1.0, true), "order_query");
    const auto final = owned();
    auto replay = final;
    replay.state = State::PartiallyFilled;
    rejects([&] { repository.save_reconciled_orders({final}, {}, "BTCUSDT", proof()); },
            "Empty replay cleared positive execution evidence");
    auto foreign = replay;
    foreign.exchange_order_id = 888;
    rejects([&] { repository.save_reconciled_orders({final}, {foreign}, "BTCUSDT", proof()); },
            "Wrong exchange identity passed durable replay validation");
    foreign = replay;
    foreign.side = Side::Sell;
    rejects([&] { repository.save_reconciled_orders({final}, {foreign}, "BTCUSDT", proof()); },
            "Wrong side passed durable replay validation");
    foreign = replay;
    foreign.commissions.at(900).amount = 0.002;
    rejects([&] { repository.save_reconciled_orders({final}, {foreign}, "BTCUSDT", proof()); },
            "Changed replay commission passed validation");
    rejects([&] { repository.save_reconciled_orders({final}, {replay, replay}, "BTCUSDT", proof()); },
            "Duplicate replay trade passed durable validation");
    rejects([&] { repository.save_reconciled_orders({final}, {replay}, "BTCUSDT", json::object()); },
            "Missing account proof passed durable validation");
    require(repository.load_latest()[0].requires_reconciliation && repository.load_accepted_history().size() == 1,
            "Rejected recovery proof changed the journal");
    repository.save_reconciled_orders({final}, {replay}, "BTCUSDT", proof());
    require(!repository.load_latest()[0].requires_reconciliation && repository.load_accepted_history().size() == 2,
            "Verified replay failed durable adoption");
    require(scalar(workspace.database(), "SELECT version FROM execution_schema") == 2,
            "Recovery schema version was not explicit");
    repository.finish_session();
}

void position_policy_checkpoint_is_owned_monotonic_and_atomic() {
    Workspace workspace;
    OrderEventRepository repository(workspace.database(), workspace.identity());
    repository.save_order(owned(), "exchange");
    RiskConfig risk;
    risk.trailing_sl_enabled = true;
    risk.trailing_sl_percent = 0.02;
    sentum::risk::PositionPolicyCheckpoint checkpoint;
    checkpoint.symbol = "BTCUSDT";
    checkpoint.entry_client_order_id = "owned-buy";
    checkpoint.entry_exchange_order_id = 71;
    checkpoint.entry_time_ms = 2000;
    checkpoint.risk_policy = sentum::risk::position_policy_identity(risk);
    checkpoint.quantity = 0.999;
    checkpoint.entry_price = 100.0 / 0.999;
    checkpoint.highest_price = 103.0;
    checkpoint.lowest_price = 100.0;
    checkpoint.stop_loss_price = 100.94;
    checkpoint.take_profit_price = checkpoint.entry_price * 1.04;
    checkpoint.updated_at_ms = 3000;
    const auto original = checkpoint.to_json();
    require(!repository.load_position_policy("BTCUSDT"), "New journal invented a trailing checkpoint");
    repository.save_position_policy("BTCUSDT", original);
    require(repository.load_position_policy("BTCUSDT") == original, "Trailing checkpoint did not survive a durable roundtrip");
    auto weakened = original;
    weakened["stop_loss_price"] = 99.0;
    rejects([&] { repository.save_position_policy("BTCUSDT", weakened); }, "Journal accepted a lower same-position stop");
    weakened = original;
    weakened["highest_price"] = 102.0;
    rejects([&] { repository.save_position_policy("BTCUSDT", weakened); }, "Journal forgot an established market peak");
    weakened = original;
    weakened["risk_policy"]["trailing_sl_percent"] = 0.03;
    rejects([&] { repository.save_position_policy("BTCUSDT", weakened); }, "Same position changed risk policy without recovery");
    weakened = original;
    weakened["entry_time_ms"] = 2001;
    rejects([&] { repository.save_position_policy("BTCUSDT", weakened); }, "Checkpoint moved the holding clock forward");
    weakened = original;
    weakened["updated_at_ms"] = 2999;
    rejects([&] { repository.save_position_policy("BTCUSDT", weakened); }, "Older checkpoint replaced newer evidence");
    weakened = original;
    weakened["entry_client_order_id"] = "unowned";
    rejects([&] { repository.save_position_policy("BTCUSDT", weakened); }, "Unknown opening order gained a policy checkpoint");
    rejects([&] { repository.save_position_policy("ETHUSDT", original); }, "Checkpoint crossed symbol namespaces");
    require(!repository.load_position_policy("ETHUSDT"), "Different market inherited the trailing checkpoint");
    {
        OrderEventRepository other(workspace.database(), workspace.identity() + "-other");
        require(!other.load_position_policy("BTCUSDT"), "Different credential namespace inherited trailing protection");
        rejects([&] { other.save_position_policy("BTCUSDT", original); }, "Different namespace adopted another account entry");
    }
    auto advanced = original;
    advanced["highest_price"] = 104.0;
    advanced["stop_loss_price"] = 101.92;
    advanced["entry_time_ms"] = 1999;
    advanced["updated_at_ms"] = 4000;
    sql(workspace.database(), "CREATE TRIGGER fail_position_policy BEFORE UPDATE ON execution_position_policies BEGIN SELECT RAISE(ABORT, 'fixture checkpoint failure'); END;");
    rejects([&] { repository.save_position_policy("BTCUSDT", advanced); }, "Failed policy write was accepted");
    require(repository.load_position_policy("BTCUSDT") == original, "Failed policy transaction lost earlier protection");
    sql(workspace.database(), "DROP TRIGGER fail_position_policy;");
    repository.save_position_policy("BTCUSDT", advanced);
    require(repository.load_position_policy("BTCUSDT") == advanced, "Stronger checkpoint with earlier authoritative entry time was rejected");
    auto next_opening = owned();
    next_opening.client_order_id = "next-buy";
    next_opening.exchange_order_id = 72;
    next_opening.execution_trade_quantities.clear();
    next_opening.execution_trade_quantities.emplace(901, 1.0);
    next_opening.commissions.clear();
    next_opening.commissions.emplace(901, sentum::order::ExecutionCommission{"BTC", 0.001});
    repository.save_order(next_opening, "exchange");
    auto next = original;
    next["entry_client_order_id"] = "next-buy";
    next["entry_exchange_order_id"] = 72;
    next["entry_time_ms"] = 5000;
    next["updated_at_ms"] = 5000;
    repository.save_position_policy("BTCUSDT", next);
    require(repository.load_position_policy("BTCUSDT") == next, "A new opening order inherited the previous position's stop");
    sql(workspace.database(), "UPDATE execution_position_policies SET payload='{}';");
    rejects([&] { (void)repository.load_position_policy("BTCUSDT"); }, "Corrupted policy checkpoint was silently ignored");
    repository.finish_session();
}
} // namespace

int main() {
    try {
        restart_restores_ledger_and_strategy_before_readiness();
        missed_outage_fills_and_fees_replace_incomplete_history_once();
        concurrent_events_invalidate_proof_and_retry_is_bounded();
        disconnect_before_or_after_recovery_revokes_admission_immediately();
        unrelated_account_activity_never_mutates_the_strategy_position();
        audit_failure_rolls_back_every_replacement_and_blocks_publication();
        journal_rejects_forged_and_incomplete_replay_before_mutating_state();
        position_policy_checkpoint_is_owned_monotonic_and_atomic();
        std::cout << "Testnet session recovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
