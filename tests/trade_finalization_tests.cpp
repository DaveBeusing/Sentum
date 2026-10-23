#include <sentum/core/RuntimeControl.hpp>
#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/history/TradeHistoryRepository.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

struct TradeFinalizationTestAccess {
    static sqlite3* history(TradeHistoryRepository& repository) { return repository.db_; }
    static sqlite3* journal(TradeHistoryRepository& repository) { return repository.journal_; }
};

struct TradeEngineTestAccess {
    static TradeHistoryRepository& history(TradeEngine& engine) { return *engine.history; }
};

namespace {
using namespace std::chrono_literals;
using TimePoint = std::chrono::system_clock::time_point;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

class Database {
public:
    explicit Database(const std::string& path) {
        require(sqlite3_open(path.c_str(), &db_) == SQLITE_OK, "cannot open test database");
        sqlite3_busy_timeout(db_, 2000);
    }
    ~Database() { sqlite3_close(db_); }
    void execute(const char* sql) {
        char* error = nullptr;
        const auto result = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
        const std::string message = error ? error : "";
        sqlite3_free(error);
        require(result == SQLITE_OK, "test SQL failed: " + message);
    }
    sqlite3_int64 integer(const char* sql) {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) == SQLITE_OK, "test query preparation failed");
        const int result = sqlite3_step(statement);
        const auto value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        require(result == SQLITE_ROW, "test query returned no value");
        return value;
    }
    std::string text(const char* sql) {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) == SQLITE_OK, "test text query preparation failed");
        const int result = sqlite3_step(statement);
        const auto* raw = sqlite3_column_text(statement, 0);
        const std::string value = raw ? reinterpret_cast<const char*>(raw) : "";
        sqlite3_finalize(statement);
        require(result == SQLITE_ROW, "test text query returned no value");
        return value;
    }
private:
    sqlite3* db_ = nullptr;
};

class TestDirectory {
public:
    TestDirectory() : previous_(std::filesystem::current_path()) {
        auto pattern = (std::filesystem::temp_directory_path() / "sentum-finalization-XXXXXX").string();
        require(::mkdtemp(pattern.data()) != nullptr, "cannot create test directory");
        path_ = pattern;
        std::filesystem::create_directory(path_ / "log");
        std::filesystem::current_path(path_);
    }
    ~TestDirectory() {
        std::filesystem::current_path(previous_);
        std::filesystem::remove_all(path_);
    }
    std::string database(const std::string& name) const { return (path_ / (name + ".sqlite3")).string(); }
private:
    std::filesystem::path previous_;
    std::filesystem::path path_;
};

class BuyStrategy final : public IStrategy {
public:
    explicit BuyStrategy(bool fail_reset = false) : fail_reset_(fail_reset) {}
    StrategySignal on_price(double price, TimePoint now) override {
        return {TradeAction::BUY, "finalization-test", "entry", price, now, 1.0};
    }
    void reset() override { if (fail_reset_) throw std::runtime_error("strategy reset failed"); }
private:
    bool fail_reset_;
};

RiskConfig risk_config() {
    RiskConfig risk;
    risk.buy_fee_percent = 0.0;
    risk.sell_fee_percent = 0.0;
    risk.spread_percent = 0.0;
    risk.slippage_percent = 0.0;
    risk.cooldown_seconds = 0;
    return risk;
}

TradePosition closed_trade() {
    TradePosition p;
    p.open = false;
    p.simulated = true;
    p.risk_approved = true;
    p.symbol = "BTCUSDT";
    p.strategy = "finalization-test";
    p.signal_reason = "entry";
    p.risk_reason = "approved";
    p.source = "replay";
    p.finalization_id = TradeHistoryRepository::new_finalization_id();
    p.signal_time = p.entry_time = TimePoint{1710000000s};
    p.exit_time = p.entry_time + 1s;
    p.reference_price = p.entry_price = 100.0;
    p.executed_price = p.exit_price = 105.0;
    p.quantity = 5.0;
    p.gross_profit = p.net_profit = 25.0;
    p.close_reason = "take_profit";
    p.notes = "immutable complete position";
    return p;
}

template <class Function>
void expect_failure(Function operation, const std::string& message) {
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    require(failed, message);
}

void test_contention_waits_and_close_evidence_is_final(const TestDirectory& directory) {
    const auto path = directory.database("contention");
    const auto base = TimePoint{1710000000s};
    auto clock = std::make_shared<ReplayClock>(base);
    TradeEngine engine("BTCUSDT", risk_config(), clock, std::make_unique<BuyStrategy>(), path);
    require(engine.evaluate(100.0) == TradeAction::BUY, "test entry did not fill");
    clock->advance_to(base + 1s);

    Database writer(path);
    writer.execute("BEGIN IMMEDIATE;");
    auto close = std::async(std::launch::async, [&] { return engine.evaluate(105.0); });
    const bool waited = close.wait_for(100ms) == std::future_status::timeout;
    writer.execute("COMMIT;");
    require(close.wait_for(5s) == std::future_status::ready, "transient contention did not resolve within the busy budget");
    require(close.get() == TradeAction::SELL, "close failed after concurrent writer released its transaction");
    require(waited, "finalization did not wait for the concurrent writer");
    require(!engine.get_current_position().open && engine.completed_trades().size() == 1,
            "confirmed close was not fully applied");
    require(engine.get_total_trades() == 1 && std::abs(engine.get_total_profit() - 25.0) < 1e-9,
            "close PnL/counters were not applied exactly once");
    const auto& completed = engine.completed_trades().front();
    require(!completed.open && completed.holding_seconds() == 1.0 && completed.executed_price == 105.0,
            "completed historical evidence must use final state and exit timestamp");
    require(writer.integer("SELECT COUNT(*) FROM trades;") == 1, "transient contention lost or duplicated history");
    require(writer.integer("SELECT COUNT(*) FROM trade_finalization_commits;") == 1, "history identity was not committed");

    std::ifstream logs("log/trades.json");
    std::string line;
    bool saw_sell = false;
    while (std::getline(logs, line)) {
        const auto record = nlohmann::json::parse(line);
        if (record.at("action") == "SELL") {
            require(record.at("open") == false && record.at("holding_seconds") == 1.0,
                    "SELL evidence retained the former open position state");
            saw_sell = true;
        }
    }
    require(saw_sell, "finalized SELL record was not logged");
}

void test_busy_failure_precedes_fill(const TestDirectory& directory) {
    const auto path = directory.database("busy-budget");
    TradeHistoryRepository repository(path, 60);
    Database writer(path);
    writer.execute("BEGIN IMMEDIATE;");
    const auto start = std::chrono::steady_clock::now();
    bool fill_called = false;
    expect_failure([&] {
        auto transaction = repository.begin_finalization();
        fill_called = true;
        transaction.complete(closed_trade());
    }, "persistent writer contention did not fail within its configured budget");
    require(!fill_called, "failed writer reservation must not execute the fill body");
    require(std::chrono::steady_clock::now() - start < 2s, "writer reservation ignored its finite busy budget");
    writer.execute("ROLLBACK;");
    const auto p = closed_trade();
    repository.save(p);
    require(writer.integer("SELECT COUNT(*) FROM trades;") == 1, "repository did not recover after reservation failure");
}

void test_idempotency_and_legacy_rows(const TestDirectory& directory) {
    const auto path = directory.database("identity");
    TradePosition p = closed_trade();
    {
        TradeHistoryRepository repository(path);
        repository.save(p);
        repository.save(p);
        auto changed = p;
        changed.notes = "different nonprojection field";
        expect_failure([&] { repository.save(changed); }, "same identity accepted a changed full payload");
        Database db(path);
        require(db.integer("SELECT COUNT(*) FROM trades;") == 1, "repeated close was inserted twice");
        Database journal(repository.recovery_path());
        require(journal.integer("SELECT COUNT(*) FROM pending_trade_finalizations;") == 0,
                "identity mismatch poisoned the recovery journal");
        // Existing rows from versions without finalization identities remain untouched.
        db.execute("INSERT INTO trades SELECT NULL,symbol,strategy,signal_reason,risk_approved,risk_reason,signal_ts,entry_ts,exit_ts,reference_price,entry_price,exit_price,quantity,gross_profit,fees,net_profit,exit_reason,simulated FROM trades LIMIT 1;");
    }
    TradeHistoryRepository reopened(path);
    reopened.save(p);
    Database db(path);
    require(db.integer("SELECT COUNT(*) FROM trades;") == 2, "reopening changed legacy rows or duplicated the identified close");
}

void test_main_commit_failure_recovery(const TestDirectory& directory) {
    const auto path = directory.database("commit-failure");
    const auto p = closed_trade();
    std::string journal_path;
    {
        TradeHistoryRepository repository(path);
        journal_path = repository.recovery_path();
        sqlite3_commit_hook(TradeFinalizationTestAccess::history(repository), [](void*) { return 1; }, nullptr);
        bool classified = false;
        try { repository.save(p); }
        catch (const TradeHistoryRepository::FinalizationError& error) {
            classified = error.recovery_recorded() && !error.history_committed();
        }
        require(classified, "history COMMIT failure did not retain its durable close");
        require(sqlite3_get_autocommit(TradeFinalizationTestAccess::history(repository)) != 0,
                "failed commit left a live transaction");
        Database db(path), journal(journal_path);
        require(db.integer("SELECT COUNT(*) FROM trades;") == 0, "failed history commit partially persisted a trade");
        require(journal.integer("SELECT COUNT(*) FROM pending_trade_finalizations;") == 1, "failed history commit lost recovery evidence");
    }
    TradeHistoryRepository reopened(path);
    reopened.save(p);
    Database db(path), journal(journal_path);
    require(db.integer("SELECT COUNT(*) FROM trades;") == 1, "restart failed to recover exactly one committed close");
    require(journal.integer("SELECT COUNT(*) FROM pending_trade_finalizations;") == 0, "recovered close was not cleaned up");
}

void test_unrecordable_journal_is_explicit(const TestDirectory& directory) {
    const auto path = directory.database("journal-failure");
    TradeHistoryRepository repository(path);
    sqlite3_commit_hook(TradeFinalizationTestAccess::journal(repository), [](void*) { return 1; }, nullptr);
    bool classified = false;
    try { repository.save(closed_trade()); }
    catch (const TradeHistoryRepository::FinalizationError& error) {
        classified = !error.recovery_recorded() && !error.history_committed()
            && std::string(error.what()).find("durable recovery could not be confirmed") != std::string::npos;
    }
    require(classified, "journal failure incorrectly claimed durable recovery");
    Database db(path);
    require(db.integer("SELECT COUNT(*) FROM trades;") == 0, "unrecorded close leaked a partial history row");
}

void test_busy_commit_rolls_back_and_recovers(const TestDirectory& directory) {
    const auto path = directory.database("busy-commit");
    const auto p = closed_trade();
    {
        TradeHistoryRepository repository(path, 60);
        Database reader(path);
        reader.execute("BEGIN;");
        require(reader.integer("SELECT COUNT(*) FROM trades;") == 0, "unexpected history before busy commit");
        bool retained = false;
        try { repository.save(p); }
        catch (const TradeHistoryRepository::FinalizationError& error) {
            retained = error.recovery_recorded() && !error.history_committed();
        }
        reader.execute("COMMIT;");
        require(retained, "reader-blocked COMMIT lost its recovery record");
        require(sqlite3_get_autocommit(TradeFinalizationTestAccess::history(repository)) != 0,
                "COMMIT busy left the write transaction active");
        require(reader.integer("SELECT COUNT(*) FROM trades;") == 0, "COMMIT busy partially published history");
    }
    TradeHistoryRepository reopened(path);
    reopened.save(p);
    Database db(path);
    require(db.integer("SELECT COUNT(*) FROM trades;") == 1, "busy COMMIT recovery duplicated or lost the close");
}

void test_corrupt_recovery_fails_without_rewriting(const TestDirectory& directory) {
    const auto path = directory.database("corrupt-recovery");
    std::string journal_path;
    {
        TradeHistoryRepository repository(path);
        journal_path = repository.recovery_path();
        sqlite3_commit_hook(TradeFinalizationTestAccess::history(repository), [](void*) { return 1; }, nullptr);
        expect_failure([&] { repository.save(closed_trade()); }, "test did not create pending recovery evidence");
    }
    Database journal(journal_path);
    journal.execute("UPDATE pending_trade_finalizations SET payload='{}';");
    expect_failure([&] { TradeHistoryRepository invalid(path); }, "invalid recovery record was silently accepted");
    require(journal.text("SELECT payload FROM pending_trade_finalizations;") == "{}",
            "failed recovery overwrote the corrupt record");
    Database db(path);
    require(db.integer("SELECT COUNT(*) FROM trades;") == 0, "invalid recovery record produced a trade");
}

void test_original_persistence_error_survives_reset_failure(const TestDirectory& directory) {
    const auto path = directory.database("reset-failure");
    const auto base = TimePoint{1710000000s};
    auto clock = std::make_shared<ReplayClock>(base);
    TradeEngine engine("BTCUSDT", risk_config(), clock, std::make_unique<BuyStrategy>(true), path);
    require(engine.evaluate(100.0) == TradeAction::BUY, "reset-failure entry did not fill");
    Database db(path);
    db.execute("CREATE TRIGGER reject_history BEFORE INSERT ON trades BEGIN SELECT RAISE(ABORT,'original storage failure'); END;");
    clock->advance_to(base + 1s);
    bool preserved = false;
    try { engine.evaluate(105.0); }
    catch (const TradeHistoryRepository::FinalizationError& error) {
        preserved = error.recovery_recorded() && std::string(error.what()).find("original storage failure") != std::string::npos;
    }
    require(preserved, "strategy reset replaced the original persistence/recovery diagnostic");
    require(!engine.get_current_position().open && engine.get_total_trades() == 1 && engine.get_total_profit() == 25.0,
            "strategy reset failure left partial closed accounting");
}

int crash_after_failed_projection(const std::string& path, bool cleanup_failure) {
    const auto base = TimePoint{1710000000s};
    auto clock = std::make_shared<ReplayClock>(base);
    TradeEngine engine("BTCUSDT", risk_config(), clock, std::make_unique<BuyStrategy>(), path);
    require(engine.evaluate(100.0) == TradeAction::BUY, "recovery test entry did not fill");
    Database db(path);
    const auto journal_path = std::filesystem::canonical(path).string() + ".trade-finalizations.sqlite3";
    Database journal(journal_path);
    if (cleanup_failure)
        journal.execute("CREATE TRIGGER reject_cleanup BEFORE DELETE ON pending_trade_finalizations BEGIN SELECT RAISE(ABORT,'persistent cleanup failure'); END;");
    else
        db.execute("CREATE TRIGGER reject_history BEFORE INSERT ON trades BEGIN SELECT RAISE(ABORT,'persistent history failure'); END;");
    clock->advance_to(base + 1s);
    bool classified = false;
    try { engine.evaluate(105.0); }
    catch (const TradeHistoryRepository::FinalizationError& error) {
        classified = error.recovery_recorded() && error.history_committed() == cleanup_failure;
    }
    require(classified, "failed projection/cleanup was not classified");
    const auto closed = engine.get_current_position();
    require(!closed.open && closed.exchange_order_id == "2" && engine.get_total_trades() == 1,
            "persistent failure did not preserve the one confirmed simulated close");
    require(engine.completed_trades().size() == 1 && !engine.completed_trades().front().open,
            "persistent failure left incomplete completed-trade state");
    require(engine.get_total_profit() == 25.0, "persistent failure partially applied PnL");
    clock->advance_to(base + 2s);
    expect_failure([&] { engine.evaluate(100.0); }, "persistent finalization failure allowed another entry");
    require(engine.get_total_trades() == 1 && engine.get_total_profit() == 25.0
            && engine.get_current_position().finalization_id == closed.finalization_id,
            "failure retry resubmitted or accounted for a close twice");
    require(journal.integer("SELECT COUNT(*) FROM pending_trade_finalizations;") == 1,
            "persistent failure lost the durable finalization record");
    std::_Exit(0); // Recovery must work without engine/repository destructors.
}

int crash_during_history_commit(const std::string& path) {
    const auto base = TimePoint{1710000000s};
    auto clock = std::make_shared<ReplayClock>(base);
    TradeEngine engine("BTCUSDT", risk_config(), clock, std::make_unique<BuyStrategy>(), path);
    require(engine.evaluate(100.0) == TradeAction::BUY, "commit-crash entry did not fill");
    auto& history = TradeEngineTestAccess::history(engine);
    sqlite3_commit_hook(TradeFinalizationTestAccess::history(history), [](void*) -> int { std::_Exit(0); }, nullptr);
    clock->advance_to(base + 1s);
    engine.evaluate(105.0);
    throw std::runtime_error("close did not reach the history COMMIT hook");
}

void test_restart_from_confirmed_close(const TestDirectory& directory, const std::string& executable, int interruption) {
    const bool cleanup_failure = interruption == 1;
    const auto path = directory.database(cleanup_failure ? "cleanup-restart" : interruption == 2 ? "commit-restart" : "history-restart");
    const char* mode = cleanup_failure ? "--crash-cleanup" : interruption == 2 ? "--crash-commit" : "--crash-history";
    const auto child = ::fork();
    require(child >= 0, "failed to start recovery subprocess");
    if (child == 0) {
        ::execl(executable.c_str(), executable.c_str(), mode, path.c_str(), nullptr);
        std::_Exit(127);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "recovery subprocess did not preserve a consistent closed state");
    Database db(path);
    const auto journal_path = std::filesystem::canonical(path).string() + ".trade-finalizations.sqlite3";
    Database journal(journal_path);
    require(db.integer("SELECT COUNT(*) FROM trades;") == (cleanup_failure ? 1 : 0), "unexpected pre-recovery projection count");
    require(journal.integer("SELECT COUNT(*) FROM pending_trade_finalizations;") == 1, "abrupt exit lost durable recovery data");
    if (cleanup_failure) journal.execute("DROP TRIGGER reject_cleanup;");
    else if (interruption == 0) db.execute("DROP TRIGGER reject_history;");
    {
        TradeHistoryRepository reopened(path);
        require(db.integer("SELECT COUNT(*) FROM trades;") == 1, "restart did not project one confirmed close");
        require(db.integer("SELECT COUNT(*) FROM trade_finalization_commits;") == 1, "restart omitted finalization identity");
        require(db.integer("SELECT net_profit FROM trades;") == 25, "restart changed confirmed close profit");
        const auto record = nlohmann::json::parse(db.text("SELECT payload FROM trade_finalization_commits;"));
        require(record.at("open") == false && record.at("exit_price") == 105.0 && record.at("exchange_order_id") == "2",
                "recovered payload differs from the confirmed closed fill");
        require(journal.integer("SELECT COUNT(*) FROM pending_trade_finalizations;") == 0, "restart left a handled recovery record");
    }
    TradeHistoryRepository reopened_again(path);
    require(db.integer("SELECT COUNT(*) FROM trades;") == 1, "second restart duplicated the close");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3) {
            if (std::string(argv[1]) == "--crash-commit") return crash_during_history_commit(argv[2]);
            return crash_after_failed_projection(argv[2], std::string(argv[1]) == "--crash-cleanup");
        }
        const auto executable = std::filesystem::absolute(argv[0]).string();
        TestDirectory directory;
        test_contention_waits_and_close_evidence_is_final(directory);
        test_busy_failure_precedes_fill(directory);
        test_idempotency_and_legacy_rows(directory);
        test_main_commit_failure_recovery(directory);
        test_unrecordable_journal_is_explicit(directory);
        test_busy_commit_rolls_back_and_recovers(directory);
        test_corrupt_recovery_fails_without_rewriting(directory);
        test_original_persistence_error_survives_reset_failure(directory);
        test_restart_from_confirmed_close(directory, executable, 0);
        test_restart_from_confirmed_close(directory, executable, 1);
        test_restart_from_confirmed_close(directory, executable, 2);
        std::cout << "trade finalization tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "trade finalization tests failed: " << error.what() << '\n';
        return 1;
    }
}
