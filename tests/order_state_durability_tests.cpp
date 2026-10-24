#include <sentum/trader/order/LiveOrderSession.hpp>
#include <sentum/trader/order/OrderEventRepository.hpp>
#include <sentum/trader/order/OrderManager.hpp>
#include <sentum/trader/order/ConfirmedPositionLedger.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using sentum::order::LiveOrderSession;
using sentum::order::OrderEventRepository;
using sentum::order::OrderManager;
using sentum::order::Snapshot;
using sentum::order::Request;
using sentum::order::State;
using sentum::order::Side;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename F> void fails(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
template <typename F> void eventually(F predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
        std::this_thread::sleep_for(1ms);
    }
}

struct Workspace {
    std::filesystem::path root;
    Workspace() {
        auto pattern = (std::filesystem::temp_directory_path() / "sentum-order-durability-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end()); buffer.push_back('\0');
        const auto path = ::mkdtemp(buffer.data());
        if (!path) throw std::runtime_error("Cannot create execution fixture directory");
        root = path;
    }
    ~Workspace() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::string database() const { return (root / "state.sqlite3").string(); }
    std::string identity() const { return OrderEventRepository::account_fingerprint(root.string()); }
};

void sql(const std::string& path, const std::string& statement) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) throw std::runtime_error("Cannot open fixture database");
    sqlite3_busy_timeout(db, 5000);
    const int status = sqlite3_exec(db, statement.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(status == SQLITE_OK, "Fixture SQL failed");
}
std::int64_t scalar(const std::string& path, const char* query) {
    sqlite3* db = nullptr;
    require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "Cannot open fixture database");
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(db, query, -1, &statement, nullptr) == SQLITE_OK, "Cannot prepare fixture query");
    require(sqlite3_step(statement) == SQLITE_ROW, "Fixture query returned no value");
    const auto result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement); sqlite3_close(db);
    return result;
}
void corrupt_payload(const std::string& path, const std::function<void(nlohmann::json&)>& change) {
    sqlite3* db = nullptr;
    require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "Cannot open payload fixture");
    sqlite3_stmt* read = nullptr;
    require(sqlite3_prepare_v2(db, "SELECT payload FROM execution_order_snapshots LIMIT 1", -1, &read, nullptr) == SQLITE_OK,
            "Cannot read payload fixture");
    require(sqlite3_step(read) == SQLITE_ROW, "Missing payload fixture");
    auto value = nlohmann::json::parse(reinterpret_cast<const char*>(sqlite3_column_text(read, 0)));
    sqlite3_finalize(read);
    change(value);
    const auto payload = value.dump();
    for (const char* query : {"UPDATE execution_order_snapshots SET payload=?", "UPDATE execution_order_history SET payload=?"}) {
        sqlite3_stmt* write = nullptr;
        require(sqlite3_prepare_v2(db, query, -1, &write, nullptr) == SQLITE_OK, "Cannot prepare payload mutation");
        sqlite3_bind_text(write, 1, payload.c_str(), -1, SQLITE_TRANSIENT);
        require(sqlite3_step(write) == SQLITE_DONE, "Cannot mutate payload fixture");
        sqlite3_finalize(write);
    }
    sqlite3_close(db);
}

Snapshot pending(std::string id = "owned-order") {
    Snapshot result;
    result.symbol = "BTCUSDT"; result.client_order_id = std::move(id);
    result.side = Side::Buy; result.state = State::Pending; result.requested_quantity = 1;
    result.locally_owned = true; result.base_asset = "BTC"; result.quote_asset = "USDT";
    result.updated_at = std::chrono::system_clock::time_point(1000ms);
    return result;
}
Snapshot filled() {
    auto result = pending();
    result.state = State::Filled; result.exchange_order_id = 77;
    result.executed_quantity = 1; result.cumulative_quote_quantity = 100; result.average_fill_price = 100;
    result.last_event_time_ms = 1001;
    result.client_order_aliases = {"cancel-request"};
    result.commissions.emplace(900, sentum::order::ExecutionCommission{"BTC", 0.001});
    result.execution_trade_quantities.emplace(900, 1.0);
    return result;
}

struct FakeClient final : BinanceSpotExecutionClient {
    FakeClient() : BinanceSpotExecutionClient("fixture-key", "fixture-secret") {}
    nlohmann::json open_orders() const override {
        if (during_open_orders) during_open_orders();
        return nlohmann::json::array();
    }
    nlohmann::json exchange_info(const std::string& symbol) const override {
        return {{"symbols", nlohmann::json::array({{{"symbol", symbol}, {"baseAsset", "BTC"}, {"quoteAsset", "USDT"},
            {"status", "TRADING"}, {"isSpotTradingAllowed", true}, {"baseAssetPrecision", 8}, {"quoteAssetPrecision", 8},
            {"orderTypes", {"MARKET"}}, {"permissionSets", {{"SPOT"}}},
            {"filters", nlohmann::json::array({{{"filterType", "LOT_SIZE"}, {"minQty", "0.000001"},
                {"maxQty", "1000"}, {"stepSize", "0.000001"}}})}}})}};
    }
    nlohmann::json account() const override {
        return {{"canTrade", true}, {"accountType", "SPOT"}, {"updateTime", 1000}, {"permissions", {"SPOT"}},
            {"balances", nlohmann::json::array({{{"asset", "BTC"}, {"free", "0"}, {"locked", "0"}},
                {{"asset", "USDT"}, {"free", "10000"}, {"locked", "0"}}})}};
    }
    nlohmann::json account_trades(const std::string&, std::int64_t, int = 1000) const override { return nlohmann::json::array(); }
    nlohmann::json ticker_price(const std::string& symbol) const override { return {{"symbol", symbol}, {"price", "100"}}; }
    nlohmann::json place_market_order(const Request& request) const override {
        ++placements;
        { std::lock_guard<std::mutex> lock(mutex); last_id = request.client_order_id; }
        if (before_place) before_place();
        if (unknown_placement) throw BinanceRequestError(BinanceRequestError::Kind::Unknown, "fixture response lost");
        return {{"orderId", 77}, {"symbol", request.symbol}, {"clientOrderId", request.client_order_id}};
    }
    nlohmann::json cancel_order(const std::string&, const std::string&) const override {
        throw BinanceRequestError(BinanceRequestError::Kind::Unknown, "fixture cancellation outcome unresolved");
    }
    nlohmann::json query_order(const std::string& symbol, const std::string& id) const override {
        ++queries;
        { std::lock_guard<std::mutex> lock(mutex); query_ids.push_back(id); }
        const int outcome = resolved.load();
        if (!outcome) throw BinanceRequestError(BinanceRequestError::Kind::NotFound, "fixture matching engine visibility lag", -2013);
        return {{"symbol", symbol}, {"clientOrderId", id}, {"orderId", 77}, {"side", "BUY"},
            {"origQty", "1"}, {"executedQty", outcome == 2 ? "1" : "0"},
            {"cummulativeQuoteQty", outcome == 2 ? "100" : "0"},
            {"status", outcome == 2 ? "FILLED" : "CANCELED"}, {"updateTime", 1001}};
    }
    bool all_queries_are(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mutex);
        return !query_ids.empty() && std::all_of(query_ids.begin(), query_ids.end(), [&](const auto& value) { return value == id; });
    }
    bool unknown_placement = true;
    std::function<void()> before_place;
    std::function<void()> during_open_orders;
    mutable std::atomic<int> placements{0}, queries{0};
    std::atomic<int> resolved{0};
    mutable std::mutex mutex;
    mutable std::string last_id;
    mutable std::vector<std::string> query_ids;
};

struct StreamState { std::atomic<bool> running{false}; };
class FakeStream final : public IUserDataStream {
public:
    explicit FakeStream(std::shared_ptr<StreamState> state, HealthHandler health)
        : state_(std::move(state)), health_(std::move(health)) {}
    void start() override { state_->running.store(true); health_(true); }
    void stop() override { state_->running.store(false); health_(false); }
    bool running() const noexcept override { return state_->running.load(); }
    bool healthy() const noexcept override { return state_->running.load(); }
    bool wait_until_healthy(std::chrono::milliseconds) override { return healthy(); }
private:
    std::shared_ptr<StreamState> state_;
    HealthHandler health_;
};
struct Streams {
    std::atomic<int> starts{0};
    LiveOrderSession::StreamFactory factory() {
        return [this](IUserDataStream::SubscriptionRequest, IUserDataStream::Handler, IUserDataStream::HealthHandler health) {
            ++starts;
            return std::make_unique<FakeStream>(std::make_shared<StreamState>(), std::move(health));
        };
    }
};
std::unique_ptr<LiveOrderSession> session(Workspace& workspace, std::unique_ptr<FakeClient> client, Streams& streams) {
    LiveOrderSession::Timing timing;
    timing.supervisor_poll_interval = 2ms;
    timing.subscription_timeout = 1s;
    auto result = LiveOrderSession::from_dependencies(std::move(client), streams.factory(), timing,
        LiveOrderSession::Persistence::durable(workspace.database(), workspace.identity()));
    result->configure_market("BTCUSDT", RiskConfig{});
    return result;
}

void roundtrip_replay_and_schema_isolation() {
    Workspace workspace;
    sql(workspace.database(), "PRAGMA user_version=37;");
    {
        OrderEventRepository journal(workspace.database(), workspace.identity());
        require(!journal.load_kill_switch(), "Fresh journal unexpectedly killed");
        journal.save_order(pending(), "intent");
        auto partial = filled(); partial.state = State::PartiallyFilled; partial.executed_quantity = .4;
        partial.cumulative_quote_quantity = 40; partial.commissions.clear();
        partial.commissions.emplace(899, sentum::order::ExecutionCommission{"", 0});
        partial.execution_trade_quantities = {{899, .4}};
        journal.save_order(partial, "stream");
        auto complete = filled(); complete.execution_trade_quantities = {{899, .4}, {900, .6}};
        complete.commissions.emplace(899, sentum::order::ExecutionCommission{"", 0});
        journal.save_order(complete, "stream");
        const auto latest = journal.load_latest();
        require(latest.size() == 1 && latest.front().locally_owned && latest.front().commissions.at(900).amount == .001 &&
            latest.front().client_order_aliases == std::vector<std::string>{"cancel-request"} &&
            latest.front().execution_trade_quantities.at(899) == .4 && latest.front().execution_trade_quantities.at(900) == .6 &&
            latest.front().cumulative_quote_quantity == 100 && latest.front().last_event_time_ms == 1001,
            "Canonical snapshot did not round-trip all execution evidence");
        require(journal.load_accepted_history().size() == 3, "Accepted history was overwritten");
        journal.finish_session();
    }
    require(scalar(workspace.database(), "PRAGMA user_version") == 37, "Execution schema changed shared database user_version");
    OrderEventRepository reopened(workspace.database(), workspace.identity());
    require(reopened.load_kill_switch(), "Stopped durable session reopened submissions");
    sentum::order::ConfirmedPositionLedger ledger;
    for (int replay = 0; replay < 2; ++replay)
        for (const auto& order : reopened.load_accepted_history()) ledger.on_order_update(order);
    const auto position = ledger.get("BTCUSDT");
    require(position && std::abs(position->quantity - .999) < 1e-12, "Restart replay duplicated execution or commission");
}

void atomic_journal_failure_and_guard() {
    Workspace workspace;
    {
        OrderEventRepository journal(workspace.database(), workspace.identity());
        journal.save_order(pending(), "intent");
        sql(workspace.database(), "CREATE TRIGGER fail_snapshot BEFORE UPDATE ON execution_order_snapshots BEGIN SELECT RAISE(ABORT,'fixture write failure'); END;");
        auto ack = pending(); ack.state = State::Acknowledged; ack.exchange_order_id = 77;
        fails([&] { journal.save_order(ack, "ack"); }, "Snapshot fault did not reject journal transaction");
        require(journal.load_latest().front().state == State::Pending && journal.load_accepted_history().size() == 1,
                "Failed snapshot save committed partial history");
        sql(workspace.database(), "DROP TRIGGER fail_snapshot; CREATE TRIGGER fail_kill BEFORE UPDATE ON execution_accounts WHEN NEW.killed=1 BEGIN SELECT RAISE(ABORT,'fixture kill failure'); END;");
        fails([&] { journal.set_kill_switch(true, "storage fault"); }, "Kill write fault was hidden");
        fails([&] { journal.finish_session(); }, "Failed shutdown cleared the crash guard");
        require(scalar(workspace.database(), "SELECT active_session FROM execution_accounts") == 1,
                "Failed durable kill lost restart guard");
    }
    sql(workspace.database(), "DROP TRIGGER fail_kill;");
    OrderEventRepository restarted(workspace.database(), workspace.identity());
    require(restarted.load_kill_switch(), "Storage-failed kill reopened after restart");
    fails([&] { restarted.set_kill_switch(false, "resume"); }, "Unresolved Pending order was resumable");
}

void process_crash_and_exclusive_lease() {
    Workspace workspace;
    const pid_t child = ::fork();
    require(child >= 0, "Cannot fork durable restart fixture");
    if (child == 0) {
        try {
            OrderEventRepository journal(workspace.database(), workspace.identity());
            journal.save_order(pending(), "intent");
            ::_exit(0);
        } catch (...) { ::_exit(1); }
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "Crash fixture failed before durable intent");
    OrderEventRepository journal(workspace.database(), workspace.identity());
    require(journal.load_kill_switch(), "Process death did not latch restart");
    fails([&] {
        OrderEventRepository competing((workspace.root / "other.sqlite3").string(), workspace.identity());
    }, "A second database bypassed the same-credential execution lease");
}

void corrupt_evidence_fails_closed() {
    const std::vector<std::function<void(nlohmann::json&)>> mutations{
        [](auto& v) { v["format"] = 2; },
        [](auto& v) { v.erase("commissions"); },
        [](auto& v) { v["state"] = "invented"; },
        [](auto& v) { v["requested_quantity"] = "1"; },
        [](auto& v) { v["executed_quantity"] = -1; },
        [](auto& v) { v["updated_at_ms"] = std::numeric_limits<std::int64_t>::max(); },
        [](auto& v) { v["updated_at_ms"] = std::numeric_limits<std::uint64_t>::max(); },
        [](auto& v) { v["state"] = "filled"; },
        [](auto& v) { v["cumulative_quote_quantity"] = 100; },
        [](auto& v) { v["quote_asset"] = "BTC"; },
        [](auto& v) { v["execution_trade_quantities"] = nlohmann::json::array({{{"trade_id", 1}, {"quantity", 0}}}); },
        [](auto& v) { v["execution_trade_quantities"] = nlohmann::json::array({{{"trade_id", 1}, {"quantity", 1}}}); },
        [](auto& v) { v["commissions"] = nlohmann::json::array({{{"trade_id", 1}, {"asset", "BTC"}, {"amount", .1}}}); }
    };
    for (const auto& mutate : mutations) {
        Workspace workspace;
        {
            OrderEventRepository journal(workspace.database(), workspace.identity());
            journal.save_order(pending(), "intent");
            journal.finish_session();
        }
        corrupt_payload(workspace.database(), mutate);
        fails([&] {
            OrderEventRepository reopened(workspace.database(), workspace.identity());
            (void)reopened.load_latest();
        }, "Malformed persisted execution was accepted");
    }
    {
        Workspace workspace;
        { OrderEventRepository journal(workspace.database(), workspace.identity()); journal.finish_session(); }
        sql(workspace.database(), "UPDATE execution_schema SET version=99;");
        fails([&] { OrderEventRepository reopened(workspace.database(), workspace.identity()); }, "Future execution schema was accepted");
    }
    {
        Workspace workspace;
        {
            OrderEventRepository journal(workspace.database(), workspace.identity());
            journal.save_order(pending(), "intent"); journal.finish_session();
        }
        sql(workspace.database(), "DELETE FROM execution_order_history;");
        fails([&] { OrderEventRepository reopened(workspace.database(), workspace.identity()); }, "Incomplete journal was accepted");
    }
}

void legacy_and_credential_change_never_auto_bind() {
    Workspace workspace;
    { OrderEventRepository legacy(workspace.database()); legacy.save(pending("old-unbound"), "exchange"); }
    {
        OrderEventRepository journal(workspace.database(), workspace.identity());
        require(journal.has_unbound_legacy_history() && journal.load_kill_switch(), "Legacy evidence was treated as bound ownership");
        auto external = filled(); external.locally_owned = false;
        journal.save_order(external, "reconciliation");
        require(journal.has_unbound_legacy_history(), "New canonical history erased unbound legacy evidence");
        fails([&] { journal.set_kill_switch(false, "resume"); }, "Unbound history was resumable");
        journal.finish_session();
    }
    OrderEventRepository other(workspace.database(), OrderEventRepository::account_fingerprint("different-fixture-credential"));
    require(other.has_unbound_legacy_history() && other.load_kill_switch(), "Credential replacement silently reused account evidence");
    Workspace no_orders;
    {
        OrderEventRepository original(no_orders.database(), no_orders.identity());
        original.set_kill_switch(true, "Operator blocked account before any placement");
        original.finish_session();
    }
    OrderEventRepository rotated(no_orders.database(), OrderEventRepository::account_fingerprint(no_orders.identity() + "rotated"));
    require(rotated.has_unbound_legacy_history() && rotated.load_kill_switch(),
            "Credential replacement bypassed a kill with no prior orders");
    fails([&] { rotated.set_kill_switch(false, "resume"); }, "New credential silently cleared another credential's kill");
}

void pending_precedes_network_and_unknown_survives_restart() {
    Workspace workspace;
    {
        OrderEventRepository journal(workspace.database(), workspace.identity());
        FakeClient client;
        client.before_place = [&] {
            const auto records = journal.load_latest();
            require(records.size() == 1 && records.front().state == State::Pending && records.front().locally_owned,
                    "Placement began before durable intent");
        };
        OrderManager manager(client, &journal);
        manager.reconcile_startup();
        const auto unknown = manager.submit_market({"BTCUSDT", Side::Buy, 1, "lost-response", "BTC", "USDT"});
        require(unknown.state == State::Unknown && manager.kill_switch_active() && client.placements == 1 &&
            client.all_queries_are("lost-response"), "Lost response did not retain blocked canonical identity");
        fails([&] { manager.submit_market({"BTCUSDT", Side::Buy, 1, "replacement", "BTC", "USDT"}); },
              "Unknown order allowed a replacement submission");
    }
    OrderEventRepository journal(workspace.database(), workspace.identity());
    FakeClient client; client.resolved.store(2);
    OrderManager restored(client, &journal);
    restored.reconcile_startup();
    const auto result = restored.get("lost-response");
    require(result && result->state == State::Filled && result->executed_quantity == 1 &&
        restored.kill_switch_active() && client.placements == 0 && client.all_queries_are("lost-response"),
        "Restart did not reconcile the original ambiguous order");
}

void session_requeries_without_resubmitting() {
    Workspace workspace; Streams streams;
    auto client = std::make_unique<FakeClient>(); auto* exchange = client.get();
    auto live = session(workspace, std::move(client), streams);
    live->start(); require(live->ready(), "Fresh fixture session was not ready");
    (void)live->submit({"BTCUSDT", Side::Buy, 1, "session-unknown", "BTC", "USDT"});
    eventually([&] { return exchange->queries.load() >= 3; }, "Supervisor did not retry canonical order query");
    require(live->killed() && !live->ready() && exchange->placements == 1 && exchange->all_queries_are("session-unknown"),
            "Unknown-outcome supervisor reopened or resubmitted");
    exchange->resolved.store(1);
    eventually([&] { const auto value = live->get("session-unknown"); return value && value->state == State::Cancelled; },
               "Supervisor did not accept authoritative terminal query");
    require(!live->ready(), "Authoritative query automatically cleared the kill latch");
    require(!live->resume_after_reconcile("wrong"), "Wrong resume confirmation cleared kill");
    require(live->resume_after_reconcile("I_CONFIRM_RECONCILED_TESTNET_RESUME") && live->ready(),
            "Valid fully resolved flat session could not resume");
    require(scalar(workspace.database(), "SELECT COUNT(*) FROM execution_kill_switch_events WHERE active=0") >= 1,
            "Resume did not leave durable audit evidence");
    live->stop();
    require(!live->ready(), "Stopped session remained ready");
}

void restored_and_late_inventory_remain_blocked() {
    for (const bool late : {false, true}) {
        Workspace workspace;
        {
            OrderEventRepository journal(workspace.database(), workspace.identity());
            auto order = late ? pending("recovered") : filled();
            if (late) order.state = State::Unknown;
            journal.save_order(order, "exchange"); journal.finish_session();
        }
        Streams streams;
        auto client = std::make_unique<FakeClient>(); auto* exchange = client.get();
        auto live = session(workspace, std::move(client), streams);
        live->start();
        require(!live->ready(), "Recovered execution state became ready without explicit reconciliation");
        if (late) {
            exchange->resolved.store(2);
            eventually([&] { return live->confirmed_position("BTCUSDT").has_value(); }, "Delayed recovered fill was not applied");
        }
        require(!live->resume_after_reconcile("I_CONFIRM_RECONCILED_TESTNET_RESUME") && !live->ready(),
                "Recovered inventory enabled a strategy with no restored position");
        require(exchange->placements == 0, "Recovery submitted a replacement order");
        live->stop();
    }
    Workspace workspace;
    {
        OrderEventRepository journal(workspace.database(), workspace.identity());
        auto external = filled(); external.locally_owned = false;
        journal.save_order(external, "exchange"); journal.finish_session();
    }
    Streams streams;
    auto live = session(workspace, std::make_unique<FakeClient>(), streams);
    live->start();
    require(!live->confirmed_position("BTCUSDT") && !live->resume_after_reconcile("I_CONFIRM_RECONCILED_TESTNET_RESUME"),
            "Unowned terminal account activity was silently resumable");
    live->stop();
}

void session_failed_kill_keeps_crash_guard() {
    Workspace workspace; Streams streams;
    {
        auto live = session(workspace, std::make_unique<FakeClient>(), streams);
        live->start();
        sql(workspace.database(), "CREATE TRIGGER fail_session_kill BEFORE UPDATE ON execution_accounts WHEN NEW.killed=1 BEGIN SELECT RAISE(ABORT,'fixture disk failure'); END;");
        live->kill(); require(live->killed() && !live->ready(), "Failed kill write left in-memory orders enabled");
        live->stop();
        require(scalar(workspace.database(), "SELECT active_session FROM execution_accounts") == 1,
                "Failed stop write cleared durable crash guard");
    }
    sql(workspace.database(), "DROP TRIGGER fail_session_kill;");
    OrderEventRepository restarted(workspace.database(), workspace.identity());
    require(restarted.load_kill_switch(), "Failed session kill was forgotten after restart");
}

void incomplete_evidence_blocks_even_flat_recovery() {
    Workspace workspace;
    {
        OrderEventRepository journal(workspace.database(), workspace.identity());
        auto buy = filled(); buy.commissions.clear(); buy.requires_reconciliation = true;
        journal.save_order(buy, "order_query");
        auto sell = filled(); sell.client_order_id = "exit-order"; sell.client_order_aliases.clear();
        sell.exchange_order_id = 78; sell.side = Side::Sell; sell.commissions.clear();
        sell.commissions.emplace(900, sentum::order::ExecutionCommission{"", 0});
        journal.save_order(sell, "stream");
        fails([&] { journal.set_kill_switch(false, "resume"); }, "Incomplete terminal execution evidence was resumable");
        journal.finish_session();
    }
    Streams streams;
    auto live = session(workspace, std::make_unique<FakeClient>(), streams);
    live->start();
    require(!live->confirmed_position("BTCUSDT") && !live->ready() &&
        !live->resume_after_reconcile("I_CONFIRM_RECONCILED_TESTNET_RESUME"),
        "Flat inventory hid incomplete execution or commission evidence");
    live->stop();
}

void newer_kill_wins_over_in_progress_resume() {
    Workspace workspace; Streams streams;
    auto client = std::make_unique<FakeClient>(); auto* exchange = client.get();
    auto live = session(workspace, std::move(client), streams);
    live->start(); live->kill();
    std::promise<void> entered, release;
    auto reached = entered.get_future(); auto released = release.get_future().share();
    exchange->during_open_orders = [&] { entered.set_value(); released.wait(); };
    auto resume = std::async(std::launch::async, [&] { return live->resume_after_reconcile("I_CONFIRM_RECONCILED_TESTNET_RESUME"); });
    require(reached.wait_for(3s) == std::future_status::ready, "Resume did not enter reconciliation");
    live->kill();
    release.set_value();
    require(resume.wait_for(3s) == std::future_status::ready && !resume.get() && live->killed() && !live->ready(),
            "In-progress resume cleared a newer operator kill");
    exchange->during_open_orders = {};
    live->stop();
}

void stop_cannot_be_undone_by_concurrent_resume() {
    Workspace workspace; Streams streams;
    auto live = session(workspace, std::make_unique<FakeClient>(), streams);
    live->start(); live->kill();
    std::promise<void> start;
    auto ready = start.get_future().share();
    auto resume = std::async(std::launch::async, [&] { ready.wait(); return live->resume_after_reconcile("I_CONFIRM_RECONCILED_TESTNET_RESUME"); });
    auto stop = std::async(std::launch::async, [&] { ready.wait(); live->stop(); });
    start.set_value();
    require(resume.wait_for(3s) == std::future_status::ready && stop.wait_for(3s) == std::future_status::ready,
            "Concurrent stop/resume did not terminate");
    (void)resume.get(); stop.get();
    require(!live->ready() && !live->resume_after_reconcile("I_CONFIRM_RECONCILED_TESTNET_RESUME"),
            "Resume recreated readiness after shutdown");
    const int count = streams.starts.load();
    fails([&] { live->start(); }, "Stopped session instance restarted its stream");
    require(streams.starts == count, "Stopped session created another stream");
}
}

int main() {
    try {
        roundtrip_replay_and_schema_isolation();
        atomic_journal_failure_and_guard();
        process_crash_and_exclusive_lease();
        corrupt_evidence_fails_closed();
        legacy_and_credential_change_never_auto_bind();
        pending_precedes_network_and_unknown_survives_restart();
        session_requeries_without_resubmitting();
        restored_and_late_inventory_remain_blocked();
        session_failed_kill_keeps_crash_guard();
        incomplete_evidence_blocks_even_flat_recovery();
        newer_kill_wins_over_in_progress_resume();
        stop_cannot_be_undone_by_concurrent_resume();
        std::cout << "order_state_durability_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "order_state_durability_tests: " << error.what() << '\n';
        return 1;
    }
}
