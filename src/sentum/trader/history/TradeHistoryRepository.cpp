#include <sentum/trader/history/TradeHistoryRepository.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;

void checked(sqlite3* db, int result, const char* operation) {
    if (result != SQLITE_OK) throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(db));
}

void execute(sqlite3* db, const char* sql, const char* operation) {
    checked(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr), operation);
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        checked(db_, sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr), "Prepare trade finalization statement");
    }
    ~Statement() { if (statement_) sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bind(int index, const std::string& value) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::length_error("Trade finalization text exceeds SQLite binding limits");
        checked(db_, sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT),
                "Bind trade finalization text");
    }
    void bind(int index, double value) {
        checked(db_, sqlite3_bind_double(statement_, index, value), "Bind trade finalization number");
    }
    void bind(int index, sqlite3_int64 value) {
        checked(db_, sqlite3_bind_int64(statement_, index, value), "Bind trade finalization integer");
    }
    int step() {
        const int result = sqlite3_step(statement_);
        if (result != SQLITE_DONE && result != SQLITE_ROW)
            throw std::runtime_error(std::string("Persist trade finalization: ") + sqlite3_errmsg(db_));
        return result;
    }
    std::string text(int column) const {
        const auto* value = sqlite3_column_text(statement_, column);
        if (!value) throw std::runtime_error("Trade finalization contains null text");
        return {reinterpret_cast<const char*>(value), static_cast<std::size_t>(sqlite3_column_bytes(statement_, column))};
    }
private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};

sqlite3* open_database(const std::string& path, int timeout) {
    sqlite3* db = nullptr;
    const int result = sqlite3_open(path.c_str(), &db);
    if (result != SQLITE_OK) {
        const std::string reason = db ? sqlite3_errmsg(db) : "no database handle";
        if (db) sqlite3_close(db);
        throw std::runtime_error("Open trade finalization database: " + reason);
    }
    try {
        checked(db, sqlite3_busy_timeout(db, timeout), "Configure trade finalization busy timeout");
        execute(db, "PRAGMA synchronous=EXTRA;", "Configure durable trade finalization");
    } catch (...) {
        sqlite3_close(db);
        throw;
    }
    return db;
}

template <class Position, class Visitor>
void visit_fields(Position& p, Visitor visit) {
    visit("open", p.open);
    visit("simulated", p.simulated);
    visit("risk_approved", p.risk_approved);
    visit("symbol", p.symbol);
    visit("source", p.source);
    visit("strategy", p.strategy);
    visit("signal_reason", p.signal_reason);
    visit("risk_reason", p.risk_reason);
    visit("notes", p.notes);
    visit("order_type", p.order_type);
    visit("client_order_id", p.client_order_id);
    visit("exchange_order_id", p.exchange_order_id);
    visit("finalization_id", p.finalization_id);
    visit("reference_price", p.reference_price);
    visit("entry_price", p.entry_price);
    visit("quantity", p.quantity);
    visit("executed_price", p.executed_price);
    visit("execution_latency_ms", p.execution_latency_ms);
    visit("highest_price", p.highest_price);
    visit("lowest_price", p.lowest_price);
    visit("stop_loss_price", p.stop_loss_price);
    visit("take_profit_price", p.take_profit_price);
    visit("exit_price", p.exit_price);
    visit("gross_profit", p.gross_profit);
    visit("net_profit", p.net_profit);
    visit("fee_entry", p.fee_entry);
    visit("fee_exit", p.fee_exit);
    visit("initial_balance", p.initial_balance);
    visit("closing_balance", p.closing_balance);
    visit("capital_at_risk", p.capital_at_risk);
    visit("leverage", p.leverage);
    visit("risk_per_trade", p.risk_per_trade);
    visit("stop_loss_percent", p.stop_loss_percent);
    visit("trailing_sl_enabled", p.trailing_sl_enabled);
    visit("trailing_sl_percent", p.trailing_sl_percent);
    visit("take_profit_percent", p.take_profit_percent);
    visit("trailing_tp_enabled", p.trailing_tp_enabled);
    visit("trailing_tp_percent", p.trailing_tp_percent);
    visit("buy_fee_percent", p.buy_fee_percent);
    visit("sell_fee_percent", p.sell_fee_percent);
    visit("close_reason", p.close_reason);
    visit("stop_loss_triggered", p.stop_loss_triggered);
    visit("take_profit_triggered", p.take_profit_triggered);
}

std::string encode(const TradePosition& position) {
    if (position.open || !position.simulated || position.finalization_id.empty() || position.symbol.empty())
        throw std::invalid_argument("Trade finalization requires a closed simulated fill and a stable identity");
    Json record{{"version", 1}};
    visit_fields(position, [&](const char* name, const auto& value) {
        if constexpr (std::is_floating_point_v<std::decay_t<decltype(value)>>) {
            if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite trade finalization field: " + std::string(name));
        }
        record[name] = value;
    });
    const auto ns = [](auto value) { return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count(); };
    record["signal_ns"] = ns(position.signal_time);
    record["entry_ns"] = ns(position.entry_time);
    record["exit_ns"] = ns(position.exit_time);
    return record.dump();
}

TradePosition decode(const std::string& payload) {
    const auto record = Json::parse(payload);
    if (!record.is_object() || record.at("version") != 1)
        throw std::runtime_error("Unsupported trade finalization recovery record");
    TradePosition position;
    visit_fields(position, [&](const char* name, auto& value) {
        value = record.at(name).template get<std::decay_t<decltype(value)>>();
    });
    const auto timestamp = [&](const char* name) {
        return std::chrono::system_clock::time_point{
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::nanoseconds{record.at(name).get<std::int64_t>()})};
    };
    position.signal_time = timestamp("signal_ns");
    position.entry_time = timestamp("entry_ns");
    position.exit_time = timestamp("exit_ns");
    if (encode(position) != payload) throw std::runtime_error("Noncanonical or corrupt trade finalization recovery record");
    return position;
}
} // namespace

TradeHistoryRepository::TradeHistoryRepository(const std::string& path, int busy_timeout_ms) {
    if (busy_timeout_ms < 0 || busy_timeout_ms > 60000)
        throw std::invalid_argument("Trade finalization busy timeout must be between zero and sixty seconds");
    try {
        db_ = open_database(path, busy_timeout_ms);
        execute(db_, "CREATE TABLE IF NOT EXISTS trades (id INTEGER PRIMARY KEY AUTOINCREMENT, symbol TEXT NOT NULL, strategy TEXT NOT NULL, signal_reason TEXT NOT NULL, risk_approved INTEGER NOT NULL, risk_reason TEXT NOT NULL, signal_ts INTEGER NOT NULL, entry_ts INTEGER NOT NULL, exit_ts INTEGER NOT NULL, reference_price REAL NOT NULL, entry_price REAL NOT NULL, exit_price REAL NOT NULL, quantity REAL NOT NULL, gross_profit REAL NOT NULL, fees REAL NOT NULL, net_profit REAL NOT NULL, exit_reason TEXT NOT NULL, simulated INTEGER NOT NULL);"
                     "CREATE TABLE IF NOT EXISTS trade_finalization_commits (finalization_id TEXT PRIMARY KEY NOT NULL, payload TEXT NOT NULL, trade_id INTEGER NOT NULL UNIQUE);",
                "Create trade history schema");
        const char* filename = sqlite3_db_filename(db_, "main");
        recovery_path_ = filename && *filename ? std::filesystem::canonical(filename).string() + ".trade-finalizations.sqlite3" : ":memory:";
        journal_ = open_database(recovery_path_, busy_timeout_ms);
        execute(journal_, "CREATE TABLE IF NOT EXISTS pending_trade_finalizations (finalization_id TEXT PRIMARY KEY NOT NULL, payload TEXT NOT NULL);",
                "Create trade finalization recovery journal");
        recover_pending();
    } catch (...) {
        if (journal_) sqlite3_close(journal_);
        if (db_) sqlite3_close(db_);
        throw;
    }
}

TradeHistoryRepository::~TradeHistoryRepository() {
    if (journal_) sqlite3_close(journal_);
    if (db_) sqlite3_close(db_);
}

std::string TradeHistoryRepository::new_finalization_id() {
    unsigned char bytes[16];
    sqlite3_randomness(sizeof(bytes), bytes);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (unsigned char value : bytes) {
        result.push_back(hex[value >> 4]);
        result.push_back(hex[value & 0x0f]);
    }
    return result;
}

TradeHistoryRepository::Finalization::Finalization(TradeHistoryRepository& owner)
    : owner_(owner), lock_(owner.mutex_) {
    try {
        execute(owner_.db_, "BEGIN IMMEDIATE;", "Reserve trade history writer before exit");
    } catch (...) {
        owner_.rollback();
        throw;
    }
}

TradeHistoryRepository::Finalization::~Finalization() {
    if (active_) owner_.rollback();
}

void TradeHistoryRepository::rollback() noexcept {
    // Some I/O errors roll back automatically; COMMIT busy leaves a live transaction.
    if (!sqlite3_get_autocommit(db_)) sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
}

void TradeHistoryRepository::Finalization::complete(const TradePosition& position) {
    if (!active_) throw std::logic_error("Trade finalization transaction already completed");
    const auto payload = encode(position);
    bool recovery_recorded = false;
    bool history_committed = false;
    try {
        if (!owner_.already_committed(position.finalization_id, payload)) {
            owner_.record_pending(position, payload);
            recovery_recorded = true;
            owner_.insert_trade(position, payload);
        } else {
            // A repeated delivery may follow a crash after the history COMMIT.
            history_committed = true;
            recovery_recorded = true;
        }
        execute(owner_.db_, "COMMIT;", "Commit trade history and finalization identity");
        active_ = false;
        history_committed = true;
        owner_.remove_pending(position.finalization_id);
    } catch (const std::exception& error) {
        owner_.rollback();
        active_ = false;
        const std::string state = history_committed ? "history committed; recovery cleanup failed"
            : recovery_recorded ? "confirmed close retained for recovery"
            : "durable recovery could not be confirmed; inspect the journal before restart";
        throw FinalizationError("Trade finalization failed (" + state + "; recovery journal " + owner_.recovery_path_ + "): " + error.what(),
                                recovery_recorded, history_committed);
    }
}

void TradeHistoryRepository::save(const TradePosition& position) {
    auto finalization = begin_finalization();
    finalization.complete(position);
}

bool TradeHistoryRepository::already_committed(const std::string& id, const std::string& payload) {
    Statement query(db_, "SELECT payload FROM trade_finalization_commits WHERE finalization_id=?;");
    query.bind(1, id);
    if (query.step() == SQLITE_DONE) return false;
    if (query.text(0) != payload) throw std::runtime_error("Trade finalization identity has a different immutable payload");
    return true;
}

void TradeHistoryRepository::record_pending(const TradePosition& position, const std::string& payload) {
    {
        Statement insert(journal_, "INSERT INTO pending_trade_finalizations(finalization_id,payload) VALUES(?,?) ON CONFLICT(finalization_id) DO NOTHING;");
        insert.bind(1, position.finalization_id);
        insert.bind(2, payload);
        insert.step();
    }
    Statement query(journal_, "SELECT payload FROM pending_trade_finalizations WHERE finalization_id=?;");
    query.bind(1, position.finalization_id);
    if (query.step() != SQLITE_ROW || query.text(0) != payload)
        throw std::runtime_error("Trade finalization recovery identity has a different immutable payload");
}

void TradeHistoryRepository::insert_trade(const TradePosition& p, const std::string& payload) {
    Statement insert(db_, "INSERT INTO trades(symbol,strategy,signal_reason,risk_approved,risk_reason,signal_ts,entry_ts,exit_ts,reference_price,entry_price,exit_price,quantity,gross_profit,fees,net_profit,exit_reason,simulated) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    const auto ms = [](auto tp) { return static_cast<sqlite3_int64>(std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count()); };
    insert.bind(1, p.symbol);
    insert.bind(2, p.strategy);
    insert.bind(3, p.signal_reason);
    insert.bind(4, static_cast<sqlite3_int64>(p.risk_approved));
    insert.bind(5, p.risk_reason);
    insert.bind(6, ms(p.signal_time));
    insert.bind(7, ms(p.entry_time));
    insert.bind(8, ms(p.exit_time));
    insert.bind(9, p.reference_price);
    insert.bind(10, p.entry_price);
    insert.bind(11, p.exit_price);
    insert.bind(12, p.quantity);
    insert.bind(13, p.gross_profit);
    insert.bind(14, p.fee_entry + p.fee_exit);
    insert.bind(15, p.net_profit);
    insert.bind(16, p.close_reason);
    insert.bind(17, static_cast<sqlite3_int64>(p.simulated));
    insert.step();
    const auto trade_id = sqlite3_last_insert_rowid(db_);
    Statement marker(db_, "INSERT INTO trade_finalization_commits(finalization_id,payload,trade_id) VALUES(?,?,?);");
    marker.bind(1, p.finalization_id);
    marker.bind(2, payload);
    marker.bind(3, trade_id);
    marker.step();
}

void TradeHistoryRepository::remove_pending(const std::string& id) {
    Statement remove(journal_, "DELETE FROM pending_trade_finalizations WHERE finalization_id=?;");
    remove.bind(1, id);
    remove.step();
}

void TradeHistoryRepository::recover_pending() {
    std::vector<TradePosition> pending;
    {
        Statement query(journal_, "SELECT finalization_id,payload FROM pending_trade_finalizations ORDER BY finalization_id;");
        while (query.step() == SQLITE_ROW) {
            auto position = decode(query.text(1));
            if (position.finalization_id != query.text(0)) throw std::runtime_error("Trade finalization recovery identity does not match its payload");
            pending.push_back(std::move(position));
        }
    }
    for (const auto& position : pending) save(position);
}
