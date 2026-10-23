#include <sentum/collector/Collector.hpp>
#include <sentum/utils/Database.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct DatabaseTestAccess {
    static void use_rollback_journal(Database& db) { db.exec_or_throw("PRAGMA journal_mode=DELETE;"); }
};

struct CollectorTestAccess {
    static void prepare(Collector& collector, bool producer_active = false) {
        collector.running.store(true, std::memory_order_release);
        collector.producer_stopped.store(!producer_active, std::memory_order_release);
        collector.logger.start();
    }
    static void start_writer(Collector& collector) {
        collector.writer_thread = std::thread(&Collector::writer_loop, &collector);
    }
    static bool enqueue(Collector& collector, Kline value) {
        return collector.try_enqueue(&collector.canonical_symbols.front(), std::move(value));
    }
    static void producer_done(Collector& collector) {
        collector.producer_stopped.store(true, std::memory_order_release);
        collector.queue_cv.notify_all();
    }
    static std::size_t retained_batch_size(const Collector& collector) {
        return collector.pending_batch.size();
    }
    static constexpr std::size_t capacity() { return Collector::queue_capacity; }
};

namespace {
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate>
void eventually(Predicate predicate, const std::string& message) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
    require(predicate(), message);
}

struct TemporaryDatabase {
    std::filesystem::path path;
    explicit TemporaryDatabase(const std::string& name) {
        std::filesystem::create_directories("log");
        path = std::filesystem::path("log") / ("collector_persistence_" + name + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite3");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        for (const auto* suffix : {"", "-wal", "-shm", "-journal"}) {
            std::filesystem::remove(path.string() + suffix, ignored);
        }
    }
};

class SqliteConnection {
public:
    explicit SqliteConnection(const std::filesystem::path& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) throw std::runtime_error("cannot open test database");
        sqlite3_busy_timeout(db_, 1000);
    }
    ~SqliteConnection() { sqlite3_close(db_); }
    void execute(const char* sql) {
        char* error = nullptr;
        const int code = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
        std::string message = error ? error : "";
        sqlite3_free(error);
        require(code == SQLITE_OK, std::string("test SQL failed: ") + sql + ": " + message);
    }
private:
    sqlite3* db_ = nullptr;
};

std::vector<MarketInfo> markets() { return {MarketInfo{"BTCUSDT", "BTC", 8, "USDT", 8}}; }
Kline candle(std::int64_t timestamp, double close = 100.0) {
    return Kline{timestamp, 100.0, 110.0, 90.0, close, 1.0};
}

void check_accounting(const Collector& collector, std::uint64_t accepted, std::uint64_t persisted,
                      std::uint64_t unrecovered, std::uint64_t drops) {
    const auto status = collector.persistence_snapshot();
    require(status.at("accepted") == accepted, "accepted write accounting mismatch");
    require(status.at("persisted") == persisted, "committed write accounting mismatch");
    require(status.at("unpersisted") == accepted - persisted, "unpersisted write accounting mismatch");
    require(status.at("unrecovered") == unrecovered, "unrecovered writes were not explicitly reported");
    require(status.at("queue_overflow_drops") == drops, "overflow and persistence failure counters were conflated");
}

void failed_statement_rolls_back_complete_batch() {
    TemporaryDatabase temporary("statement");
    Database db(temporary.path.string());
    SqliteConnection control(temporary.path);
    control.execute("CREATE TRIGGER reject_second BEFORE INSERT ON klines WHEN NEW.timestamp=2 "
                    "BEGIN SELECT RAISE(ABORT, 'injected write failure'); END;");
    const std::string symbol = "btcusdt";
    const std::vector<KlineBatchItem> batch{{&symbol, candle(1)}, {&symbol, candle(2)}};
    const auto failed = db.save_kline_batch_result(batch, 0);
    require(!failed.committed && !failed.retryable(), "constraint failure was considered committed or retryable");
    require((failed.sqlite_code & 0xff) == SQLITE_CONSTRAINT && failed.rollback_code == SQLITE_OK,
            "constraint failure lost its SQLite classification or rollback result");
    require(db.load_klines(symbol).empty(), "a failed batch left a partially committed candle");
    control.execute("DROP TRIGGER reject_second;");
    require(db.save_kline_batch_result(batch, 0).committed, "failed write left the connection transaction open");
    require(db.save_kline_batch_result(batch, 0).committed, "idempotent replay was rejected");
    require(db.load_klines(symbol).size() == 2, "batch replay created duplicate candle rows");

    const auto invalid = db.save_kline_batch_result({{nullptr, candle(3)}}, 0);
    require(!invalid.committed && !invalid.retryable() && invalid.sqlite_code == SQLITE_MISUSE,
            "invalid batch item was silently accepted");
    require(db.save_kline_batch_result({{&symbol, candle(3)}}, 0).committed,
            "invalid batch item leaked a transaction");
}

void failed_commit_rolls_back_and_can_retry() {
    TemporaryDatabase temporary("commit");
    Database db(temporary.path.string());
    DatabaseTestAccess::use_rollback_journal(db);
    SqliteConnection reader(temporary.path);
    reader.execute("BEGIN; SELECT COUNT(*) FROM klines;");
    const std::string symbol = "btcusdt";
    const std::vector<KlineBatchItem> batch{{&symbol, candle(1)}};
    const auto failed = db.save_kline_batch_result(batch, 0);
    require(!failed.committed && failed.retryable() && failed.rollback_code == SQLITE_OK,
            "blocked COMMIT did not produce a retryable rolled-back result");
    require(db.load_klines(symbol).empty(), "failed COMMIT left a pending transaction visible to the writer");
    reader.execute("COMMIT;");
    require(db.save_kline_batch_result(batch, 0).committed, "retry after COMMIT contention failed");
    require(db.load_klines(symbol).size() == 1, "recovered COMMIT did not persist exactly one row");
}

void transient_lock_recovers_without_losing_accepted_candles() {
    TemporaryDatabase temporary("transient");
    Database db(temporary.path.string());
    MarketDataStore store;
    Collector collector(db, store, markets());
    SqliteConnection locker(temporary.path);
    CollectorTestAccess::prepare(collector);
    locker.execute("BEGIN IMMEDIATE;");
    for (std::int64_t i = 0; i < 64; ++i) {
        require(CollectorTestAccess::enqueue(collector, candle(i % 32 + 1, 100.0 + i)), "unexpected enqueue rejection");
    }
    CollectorTestAccess::start_writer(collector);
    eventually([&] { return collector.persistence_snapshot().at("status") == "RETRYING"; },
               "SQLite contention was not observed");
    require(!collector.persistence_healthy() && !collector.persistence_failed(), "transient write failure was not degraded");
    require(collector.persistence_snapshot().at("persisted") == 0, "uncommitted batch counted as durable");
    locker.execute("ROLLBACK;");
    eventually([&] { return collector.persistence_snapshot().at("persisted") == 64; }, "retained batch did not recover");
    require(collector.persistence_snapshot().at("drained") == false,
            "a running collector claimed shutdown drain completion");
    collector.stop();
    require(collector.persistence_healthy() && !collector.persistence_failed(), "successful recovery did not restore health");
    check_accounting(collector, 64, 64, 0, 0);
    require(collector.persistence_snapshot().at("drained") == true, "successful stop was not recorded as drained");
    const auto rows = db.load_klines("btcusdt");
    require(rows.size() == 32 && rows.front().close == 132.0 && rows.back().close == 163.0,
            "retry or duplicate UPSERT changed the final candle sequence");
}

void persistent_lock_has_bounded_stop_and_explicit_unrecovered_count() {
    TemporaryDatabase temporary("persistent");
    Database db(temporary.path.string());
    MarketDataStore store;
    Collector collector(db, store, markets());
    SqliteConnection locker(temporary.path);
    CollectorTestAccess::prepare(collector);
    locker.execute("BEGIN IMMEDIATE;");
    const auto capacity = CollectorTestAccess::capacity();
    for (std::size_t i = 0; i < capacity; ++i) {
        require(CollectorTestAccess::enqueue(collector, candle(static_cast<std::int64_t>(i + 1))), "backlog filled prematurely");
    }
    require(!CollectorTestAccess::enqueue(collector, candle(9000)), "full queue accepted an overflow candle");
    const auto started = std::chrono::steady_clock::now();
    CollectorTestAccess::start_writer(collector);
    eventually([&] { return collector.persistence_failed(); }, "permanent SQLite contention did not fail closed");
    collector.stop();
    require(std::chrono::steady_clock::now() - started < 4s, "failed shutdown retried the entire backlog without a bound");
    check_accounting(collector, capacity, 0, capacity, 1);
    require(collector.queue_depth() + CollectorTestAccess::retained_batch_size(collector) == capacity,
            "accepted failed batch or queue backlog was discarded");
    require(!collector.persistence_healthy() && collector.persistence_snapshot().at("drained") == false,
            "failed persistence reported a healthy drained stop");
    require(!CollectorTestAccess::enqueue(collector, candle(9001)), "terminal storage failure still accepted intake");
    require(collector.persistence_snapshot().at("rejected_after_failure") == 1, "post-failure rejection was not counted");
    bool propagated = false;
    try { collector.rethrow_persistence_failure(); }
    catch (const std::runtime_error& error) {
        propagated = std::string(error.what()).find("unrecovered=" + std::to_string(capacity)) != std::string::npos;
    }
    require(propagated, "terminal persistence error lost its unrecovered count");
    bool restart_rejected = false;
    try { collector.start(); } catch (const std::runtime_error&) { restart_rejected = true; }
    require(restart_rejected, "failed collector was restarted without resolving its unrecovered batch");
    locker.execute("ROLLBACK;");
    collector.stop();
    require(collector.persistence_failed(), "stop cleared the terminal failure latch");
    require(db.load_klines("btcusdt").empty(), "uncommitted failed batch appeared in history");
}

void permanent_write_error_preserves_producer_tail_accounting() {
    TemporaryDatabase temporary("producer_tail");
    Database db(temporary.path.string());
    MarketDataStore store;
    Collector collector(db, store, markets());
    SqliteConnection control(temporary.path);
    control.execute("CREATE TRIGGER reject_writes BEFORE INSERT ON klines "
                    "BEGIN SELECT RAISE(ABORT, 'injected durable failure'); END;");
    CollectorTestAccess::prepare(collector, true);
    require(CollectorTestAccess::enqueue(collector, candle(1)), "initial candle rejected");
    std::atomic<std::uint64_t> accepted{1};
    std::thread producer([&] {
        for (std::int64_t i = 2; i < 2000 && !collector.persistence_failed(); ++i) {
            if (CollectorTestAccess::enqueue(collector, candle(i))) accepted.fetch_add(1);
        }
        CollectorTestAccess::producer_done(collector);
    });
    CollectorTestAccess::start_writer(collector);
    producer.join();
    eventually([&] { return collector.persistence_failed(); }, "permanent write error was not visible");
    collector.stop();
    check_accounting(collector, accepted.load(), 0, accepted.load(), 0);
    const auto snapshot = collector.persistence_snapshot();
    require(snapshot.at("write_failures") == 1 && snapshot.at("retries") == 0,
            "permanent write error was retried as lock contention");
    require(collector.queue_depth() + CollectorTestAccess::retained_batch_size(collector) == accepted.load(),
            "concurrent producer tail escaped unrecovered accounting");
    require(db.load_klines("btcusdt").empty(), "rejected writes changed the durable history");
}
} // namespace

int main() {
    try {
        failed_statement_rolls_back_complete_batch();
        failed_commit_rolls_back_and_can_retry();
        transient_lock_recovers_without_losing_accepted_candles();
        persistent_lock_has_bounded_stop_and_explicit_unrecovered_count();
        permanent_write_error_preserves_producer_tail_accounting();
        std::cout << nlohmann::json{{"scenario", "collector-persistence"}, {"pass", true},
                                  {"validated_faults", 5}, {"unrecovered_accounting_verified", true}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cout << nlohmann::json{{"scenario", "collector-persistence"}, {"pass", false},
                                  {"failure", error.what()}}.dump() << '\n';
        return 1;
    }
}
