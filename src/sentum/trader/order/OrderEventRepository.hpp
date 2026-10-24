#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <sentum/trader/order/OrderTypes.hpp>

namespace sentum::order {

// The one-argument form is the historical diagnostic log. Account-scoped
// journals own the authoritative snapshots, audit history and execution lease.
class OrderEventRepository {
public:
    explicit OrderEventRepository(const std::string& path, std::string account = {})
        : account_(std::move(account)) {
        try {
            if (durable()) {
                if (path.empty() || path == ":memory:")
                    throw std::invalid_argument("Durable execution requires a file database");
                acquire_lease();
            }
            if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
                throw std::runtime_error("Failed to open order-event database");
            sqlite3_busy_timeout(db_, 5000);
            exec("PRAGMA foreign_keys=ON;");
            if (durable()) exec("PRAGMA synchronous=FULL;");
            exec("CREATE TABLE IF NOT EXISTS order_events ("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT, client_order_id TEXT NOT NULL, exchange_order_id INTEGER NOT NULL, "
                 "symbol TEXT NOT NULL, side TEXT NOT NULL, state TEXT NOT NULL, source TEXT NOT NULL, "
                 "requested_qty REAL NOT NULL, executed_qty REAL NOT NULL, average_fill_price REAL NOT NULL, "
                 "rejection_reason TEXT NOT NULL, exchange_ts INTEGER NOT NULL, local_ts INTEGER NOT NULL);"
                 "CREATE INDEX IF NOT EXISTS idx_order_events_client_ts ON order_events(client_order_id, local_ts);");
            if (durable()) { initialize_schema(); begin_session(); }
        } catch (...) { close(); throw; }
    }
    ~OrderEventRepository() { close(); }
    OrderEventRepository(const OrderEventRepository&) = delete;
    OrderEventRepository& operator=(const OrderEventRepository&) = delete;

    bool durable() const noexcept { return !account_.empty(); }
    static std::string account_fingerprint(const std::string& api_key) {
        if (api_key.empty()) throw std::invalid_argument("Execution credential identity is empty");
        return digest("binance-spot-testnet:" + api_key);
    }

    void save(const Snapshot& snapshot, const std::string& source) {
        if (durable()) { save_order(snapshot, source); return; }
        std::lock_guard<std::mutex> lock(mutex_);
        Statement s(db_, "INSERT INTO order_events(client_order_id,exchange_order_id,symbol,side,state,source,requested_qty,executed_qty,average_fill_price,rejection_reason,exchange_ts,local_ts) VALUES(?,?,?,?,?,?,?,?,?,?,?,?);");
        s.text(1, snapshot.client_order_id); s.integer(2, snapshot.exchange_order_id);
        s.text(3, snapshot.symbol); s.text(4, snapshot.side == Side::Buy ? "BUY" : "SELL");
        s.text(5, to_string(snapshot.state)); s.text(6, source);
        s.real(7, snapshot.requested_quantity); s.real(8, snapshot.executed_quantity);
        s.real(9, snapshot.average_fill_price); s.text(10, snapshot.rejection_reason);
        s.integer(11, milliseconds(snapshot.updated_at)); s.integer(12, now_ms()); s.done();
    }

    void save_order(const Snapshot& snapshot, const std::string& source) {
        require_durable(); validate(snapshot);
        if (source.empty()) throw std::invalid_argument("Execution event source is empty");
        const auto payload = encode(snapshot).dump();
        std::lock_guard<std::mutex> lock(mutex_);
        transaction([&] {
            require_active_session();
            Statement prior(db_, "SELECT payload FROM execution_order_snapshots WHERE account=? AND client_order_id=?;");
            prior.text(1, account_); prior.text(2, snapshot.client_order_id);
            if (prior.row() && decode(prior.string(0)).requires_reconciliation && !snapshot.requires_reconciliation)
                throw std::logic_error("Execution reconciliation requirement cannot be cleared by an order update");
            Statement history(db_, "INSERT INTO execution_order_history(account,client_order_id,payload,source,local_ts) VALUES(?,?,?,?,?);");
            history.text(1, account_); history.text(2, snapshot.client_order_id);
            history.text(3, payload); history.text(4, source); history.integer(5, now_ms()); history.done();
            const auto history_id = sqlite3_last_insert_rowid(db_);
            Statement current(db_, "INSERT INTO execution_order_snapshots(account,client_order_id,symbol,exchange_order_id,history_id,payload) VALUES(?,?,?,?,?,?) ON CONFLICT(account,client_order_id) DO UPDATE SET symbol=excluded.symbol,exchange_order_id=excluded.exchange_order_id,history_id=excluded.history_id,payload=excluded.payload;");
            current.text(1, account_); current.text(2, snapshot.client_order_id); current.text(3, snapshot.symbol);
            current.integer(4, snapshot.exchange_order_id); current.integer(5, history_id);
            current.text(6, payload); current.done();
            Statement count(db_, "UPDATE execution_accounts SET history_count=history_count+1 WHERE account=?;");
            count.text(1, account_); count.done();
        });
    }

    std::vector<Snapshot> load_latest() const {
        require_durable();
        std::lock_guard<std::mutex> lock(mutex_);
        validate_journal();
        Statement s(db_, "SELECT client_order_id,payload,symbol,exchange_order_id FROM execution_order_snapshots WHERE account=? ORDER BY history_id;");
        s.text(1, account_); return read_snapshots(s, true);
    }
    std::vector<Snapshot> load_accepted_history() const {
        require_durable();
        std::lock_guard<std::mutex> lock(mutex_);
        validate_journal();
        Statement s(db_, "SELECT client_order_id,payload FROM execution_order_history WHERE account=? ORDER BY id;");
        s.text(1, account_); return read_snapshots(s);
    }
    bool load_kill_switch() const {
        require_durable(); std::lock_guard<std::mutex> lock(mutex_); return read_account().killed;
    }
    std::string kill_switch_reason() const {
        require_durable(); std::lock_guard<std::mutex> lock(mutex_); return read_account().reason;
    }
    void set_kill_switch(bool active, const std::string& reason) {
        require_durable();
        if (reason.empty()) throw std::invalid_argument("Kill-switch audit reason is required");
        std::lock_guard<std::mutex> lock(mutex_);
        transaction([&] {
            require_active_session();
            if (!active) {
                if (read_account().unbound_history)
                    throw std::logic_error("Unbound execution history requires explicit account reconciliation");
                validate_journal();
                Statement s(db_, "SELECT client_order_id,payload,symbol,exchange_order_id FROM execution_order_snapshots WHERE account=? ORDER BY history_id;");
                s.text(1, account_);
                for (const auto& order : read_snapshots(s, true)) {
                    if (order.requires_reconciliation)
                        throw std::logic_error("Incomplete execution evidence requires full reconciliation");
                    if (!order.locally_owned)
                        throw std::logic_error("Unowned account activity requires explicit account reconciliation");
                    if (unresolved(order.state))
                        throw std::logic_error("Cannot durably resume with unresolved orders");
                }
            }
            update_latch(active, reason);
        });
    }

    // If shutdown persistence fails, the durable active-session guard remains.
    // Merely destroying this object never clears that guard.
    void finish_session() {
        require_durable();
        std::lock_guard<std::mutex> lock(mutex_);
        transaction([&] {
            require_active_session();
            update_latch(true, "Execution session stopped; explicit reconciled resume required");
            Statement s(db_, "UPDATE execution_accounts SET active_session=0 WHERE account=?;");
            s.text(1, account_); s.done();
        });
        finished_ = true;
    }
    bool has_unbound_legacy_history() const {
        require_durable(); std::lock_guard<std::mutex> lock(mutex_);
        return read_account().unbound_history;
    }

private:
    class Statement {
    public:
        Statement(sqlite3* db, const char* sql) {
            if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK)
                throw std::runtime_error("Cannot prepare execution journal statement");
        }
        ~Statement() { sqlite3_finalize(statement_); }
        void text(int i, const std::string& v) {
            if (sqlite3_bind_text(statement_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT) != SQLITE_OK) fail();
        }
        void integer(int i, std::int64_t v) { if (sqlite3_bind_int64(statement_, i, v) != SQLITE_OK) fail(); }
        void real(int i, double v) { if (sqlite3_bind_double(statement_, i, v) != SQLITE_OK) fail(); }
        bool row() {
            const int r = sqlite3_step(statement_);
            if (r == SQLITE_ROW) return true;
            if (r == SQLITE_DONE) return false;
            fail();
        }
        void done() { if (sqlite3_step(statement_) != SQLITE_DONE) fail(); }
        std::string string(int c) const {
            if (sqlite3_column_type(statement_, c) != SQLITE_TEXT) fail();
            return std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement_, c)), sqlite3_column_bytes(statement_, c));
        }
        std::int64_t number(int c) const {
            if (sqlite3_column_type(statement_, c) != SQLITE_INTEGER) fail();
            return sqlite3_column_int64(statement_, c);
        }
    private:
        [[noreturn]] static void fail() { throw std::runtime_error("Execution journal operation failed"); }
        sqlite3_stmt* statement_ = nullptr;
    };
    struct AccountState { bool killed; bool active; std::string reason; std::int64_t count; bool unbound_history; };
    static std::int64_t milliseconds(std::chrono::system_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    }
    static std::int64_t now_ms() { return milliseconds(std::chrono::system_clock::now()); }
    static bool unresolved(State s) {
        return s == State::Pending || s == State::Acknowledged || s == State::PartiallyFilled ||
               s == State::Cancelling || s == State::Unknown;
    }
    static std::string digest(const std::string& value) {
        unsigned char output[EVP_MAX_MD_SIZE]; unsigned int length = 0;
        if (EVP_Digest(value.data(), value.size(), output, &length, EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("Cannot derive execution credential identity");
        std::ostringstream result;
        for (unsigned int i = 0; i < length; ++i)
            result << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(output[i]);
        return result.str();
    }
    void acquire_lease() {
        const auto directory = std::filesystem::path("/tmp") /
            ("sentum-execution-" + std::to_string(static_cast<unsigned long>(geteuid())));
        if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
            throw std::runtime_error("Cannot create execution lease directory");
        struct stat ds{};
        if (::lstat(directory.c_str(), &ds) != 0 || !S_ISDIR(ds.st_mode) ||
            ds.st_uid != geteuid() || (ds.st_mode & 0077) != 0)
            throw std::runtime_error("Execution lease directory is not private");
        const auto path = directory / (digest(account_) + ".lock");
        lease_fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        struct stat fs{};
        if (lease_fd_ < 0 || ::fstat(lease_fd_, &fs) != 0 || !S_ISREG(fs.st_mode) ||
            fs.st_uid != geteuid() || ::flock(lease_fd_, LOCK_EX | LOCK_NB) != 0)
            throw std::runtime_error("Another execution session owns this credential identity");
    }
    void close() noexcept {
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        if (lease_fd_ >= 0) { ::close(lease_fd_); lease_fd_ = -1; }
    }
    void require_durable() const {
        if (!durable()) throw std::logic_error("An account-scoped execution journal is required");
    }
    void require_active_session() const {
        if (finished_ || !read_account().active) throw std::logic_error("Execution journal session has ended");
    }
    void exec(const char* sql) const {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Execution journal database operation failed");
    }
    template <typename Operation> void transaction(Operation operation) {
        exec("BEGIN IMMEDIATE;");
        try { operation(); exec("COMMIT;"); }
        catch (...) { sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); throw; }
    }
    void initialize_schema() {
        exec("CREATE TABLE IF NOT EXISTS execution_schema(id INTEGER PRIMARY KEY CHECK(id=1), version INTEGER NOT NULL);");
        exec("INSERT OR IGNORE INTO execution_schema(id,version) VALUES(1,1);");
        Statement version(db_, "SELECT version FROM execution_schema WHERE id=1;");
        if (!version.row() || version.number(0) != 1 || version.row())
            throw std::runtime_error("Unsupported execution journal schema");
        exec("CREATE TABLE IF NOT EXISTS execution_accounts(account TEXT PRIMARY KEY,killed INTEGER NOT NULL CHECK(killed IN (0,1)),reason TEXT NOT NULL,active_session INTEGER NOT NULL CHECK(active_session IN (0,1)),history_count INTEGER NOT NULL CHECK(history_count>=0),legacy_unbound INTEGER NOT NULL CHECK(legacy_unbound IN (0,1)));"
             "CREATE TABLE IF NOT EXISTS execution_order_history(id INTEGER PRIMARY KEY AUTOINCREMENT,account TEXT NOT NULL,client_order_id TEXT NOT NULL,payload TEXT NOT NULL,source TEXT NOT NULL,local_ts INTEGER NOT NULL,FOREIGN KEY(account) REFERENCES execution_accounts(account));"
             "CREATE INDEX IF NOT EXISTS execution_history_account ON execution_order_history(account,id);"
             "CREATE INDEX IF NOT EXISTS execution_history_order ON execution_order_history(account,client_order_id,id);"
             "CREATE TABLE IF NOT EXISTS execution_order_snapshots(account TEXT NOT NULL,client_order_id TEXT NOT NULL,symbol TEXT NOT NULL,exchange_order_id INTEGER NOT NULL,history_id INTEGER NOT NULL,payload TEXT NOT NULL,PRIMARY KEY(account,client_order_id),FOREIGN KEY(account) REFERENCES execution_accounts(account),FOREIGN KEY(history_id) REFERENCES execution_order_history(id));"
             "CREATE UNIQUE INDEX IF NOT EXISTS execution_exchange_identity ON execution_order_snapshots(account,symbol,exchange_order_id) WHERE exchange_order_id>0;"
             "CREATE TABLE IF NOT EXISTS execution_kill_switch_events(id INTEGER PRIMARY KEY AUTOINCREMENT,account TEXT NOT NULL,active INTEGER NOT NULL CHECK(active IN (0,1)),reason TEXT NOT NULL,local_ts INTEGER NOT NULL,FOREIGN KEY(account) REFERENCES execution_accounts(account));");
    }
    void begin_session() {
        transaction([&] {
            Statement create(db_, "INSERT OR IGNORE INTO execution_accounts(account,killed,reason,active_session,history_count,legacy_unbound) VALUES(?,0,'New credential journal',0,0,EXISTS(SELECT 1 FROM order_events) OR EXISTS(SELECT 1 FROM execution_accounts));");
            create.text(1, account_); create.done();
            const auto previous = read_account();
            if (previous.active)
                update_latch(true, "Previous execution session did not complete durable shutdown");
            if (previous.unbound_history)
                update_latch(true, "Unbound execution history requires full account reconciliation");
            Statement guard(db_, "UPDATE execution_accounts SET active_session=1 WHERE account=?;");
            guard.text(1, account_); guard.done();
            validate_journal();
        });
    }
    AccountState read_account() const {
        Statement s(db_, "SELECT killed,active_session,reason,history_count,legacy_unbound FROM execution_accounts WHERE account=?;");
        s.text(1, account_);
        if (!s.row()) throw std::runtime_error("Execution credential state is missing");
        const auto killed = s.number(0), active = s.number(1), count = s.number(3);
        const auto unbound = s.number(4);
        const auto reason = s.string(2);
        if ((killed != 0 && killed != 1) || (active != 0 && active != 1) || (unbound != 0 && unbound != 1) || count < 0 || reason.empty())
            throw std::runtime_error("Execution credential state is invalid");
        return {killed != 0, active != 0, reason, count, unbound != 0};
    }
    void update_latch(bool active, const std::string& reason) {
        Statement update(db_, "UPDATE execution_accounts SET killed=?,reason=? WHERE account=?;");
        update.integer(1, active ? 1 : 0); update.text(2, reason); update.text(3, account_); update.done();
        if (sqlite3_changes(db_) != 1) throw std::runtime_error("Execution latch was not persisted");
        Statement audit(db_, "INSERT INTO execution_kill_switch_events(account,active,reason,local_ts) VALUES(?,?,?,?);");
        audit.text(1, account_); audit.integer(2, active ? 1 : 0);
        audit.text(3, reason); audit.integer(4, now_ms()); audit.done();
    }
    void validate_journal() const {
        const auto expected = read_account().count;
        Statement count(db_, "SELECT COUNT(*) FROM execution_order_history WHERE account=?;");
        count.text(1, account_);
        if (!count.row() || count.number(0) != expected) throw std::runtime_error("Execution history is incomplete");
        Statement missing(db_, "SELECT COUNT(*) FROM execution_order_snapshots s LEFT JOIN execution_order_history h ON h.id=s.history_id WHERE s.account=? AND (h.id IS NULL OR h.account!=s.account OR h.client_order_id!=s.client_order_id OR h.payload!=s.payload OR s.history_id!=(SELECT MAX(t.id) FROM execution_order_history t WHERE t.account=s.account AND t.client_order_id=s.client_order_id));");
        missing.text(1, account_);
        if (!missing.row() || missing.number(0) != 0) throw std::runtime_error("Execution snapshot contradicts its journal");
        Statement absent(db_, "SELECT COUNT(*) FROM (SELECT DISTINCT client_order_id FROM execution_order_history WHERE account=?) h WHERE NOT EXISTS(SELECT 1 FROM execution_order_snapshots s WHERE s.account=? AND s.client_order_id=h.client_order_id);");
        absent.text(1, account_); absent.text(2, account_);
        if (!absent.row() || absent.number(0) != 0) throw std::runtime_error("Execution snapshot is missing");
    }
    static void validate(const Snapshot& s) {
        if (s.symbol.empty() || s.client_order_id.empty() || s.exchange_order_id < 0 || s.last_event_time_ms < 0 ||
            milliseconds(s.updated_at) < 0 || !std::isfinite(s.requested_quantity) || s.requested_quantity <= 0 ||
            !std::isfinite(s.executed_quantity) || s.executed_quantity < 0 ||
            !std::isfinite(s.cumulative_quote_quantity) || s.cumulative_quote_quantity < 0 ||
            !std::isfinite(s.average_fill_price) || s.average_fill_price < 0 ||
            s.executed_quantity > s.requested_quantity + std::max(1e-12, s.requested_quantity * 1e-10))
            throw std::runtime_error("Invalid persisted order quantities or identity");
        if (s.side != Side::Buy && s.side != Side::Sell) throw std::runtime_error("Invalid persisted order side");
        switch (s.state) {
            case State::Pending: case State::Acknowledged: case State::PartiallyFilled: case State::Filled:
            case State::Cancelling: case State::Cancelled: case State::Rejected: case State::Unknown: break;
            default: throw std::runtime_error("Invalid persisted order state");
        }
        if (s.executed_quantity > 0 && (!s.exchange_confirmed_execution() ||
            (s.locally_owned && (s.base_asset.empty() || s.quote_asset.empty()))))
            throw std::runtime_error("Incomplete persisted execution evidence");
        if ((s.executed_quantity == 0 && (s.cumulative_quote_quantity != 0 || s.average_fill_price != 0 || !s.commissions.empty())) ||
            ((s.state == State::Filled || s.state == State::PartiallyFilled) && s.executed_quantity <= 0) ||
            (!s.base_asset.empty() && s.base_asset == s.quote_asset))
            throw std::runtime_error("Contradictory persisted execution evidence");
        if (s.executed_quantity > 0 && std::abs(s.average_fill_price - s.cumulative_quote_quantity / s.executed_quantity) >
            std::max(1e-10, s.average_fill_price * 1e-10))
            throw std::runtime_error("Persisted execution price differs from cumulative quote");
        std::set<std::string> aliases;
        for (const auto& alias : s.client_order_aliases)
            if (alias.empty() || !aliases.insert(alias).second) throw std::runtime_error("Invalid persisted order alias");
        double base_fees = 0, quote_fees = 0;
        for (const auto& [trade, commission] : s.commissions) {
            if (trade < 0 || (commission.asset.empty() && commission.amount != 0.0) ||
                !std::isfinite(commission.amount) || commission.amount < 0)
                throw std::runtime_error("Invalid persisted commission evidence");
            if (commission.asset == s.base_asset) base_fees += commission.amount;
            if (commission.asset == s.quote_asset) quote_fees += commission.amount;
        }
        if (!std::isfinite(base_fees) || !std::isfinite(quote_fees) || base_fees > s.executed_quantity || quote_fees > s.cumulative_quote_quantity)
            throw std::runtime_error("Persisted commission exceeds executed quantity");
        double proven_quantity = 0.0;
        for (const auto& [trade, quantity] : s.execution_trade_quantities) {
            if (trade < 0 || !std::isfinite(quantity) || quantity <= 0.0)
                throw std::runtime_error("Invalid persisted execution trade quantity");
            proven_quantity += quantity;
        }
        const double tolerance = 64 * std::numeric_limits<double>::epsilon() *
            std::max(proven_quantity, s.executed_quantity);
        if (!std::isfinite(proven_quantity) || proven_quantity > s.executed_quantity + tolerance ||
            (s.locally_owned && !s.requires_reconciliation && std::abs(proven_quantity - s.executed_quantity) > tolerance))
            throw std::runtime_error("Persisted execution lacks complete unique-trade evidence");
        if (!s.requires_reconciliation)
            for (const auto& [trade, commission] : s.commissions) {
                (void)commission;
                if (!s.execution_trade_quantities.count(trade))
                    throw std::runtime_error("Persisted commission has no corresponding execution trade");
            }
        if (s.locally_owned && !s.requires_reconciliation)
            for (const auto& [trade, quantity] : s.execution_trade_quantities) {
                (void)quantity;
                if (!s.commissions.count(trade))
                    throw std::runtime_error("Persisted execution trade has no explicit commission evidence");
            }
    }
    static State decode_state(const std::string& value) {
        for (const auto state : {State::Pending, State::Acknowledged, State::PartiallyFilled, State::Filled,
                                State::Cancelling, State::Cancelled, State::Rejected, State::Unknown})
            if (value == to_string(state)) return state;
        throw std::runtime_error("Unknown persisted order state");
    }
    static nlohmann::json encode(const Snapshot& s) {
        nlohmann::json commissions = nlohmann::json::array();
        for (const auto& [trade, value] : s.commissions)
            commissions.push_back({{"trade_id", trade}, {"asset", value.asset}, {"amount", value.amount}});
        nlohmann::json trades = nlohmann::json::array();
        for (const auto& [trade, quantity] : s.execution_trade_quantities)
            trades.push_back({{"trade_id", trade}, {"quantity", quantity}});
        return {{"format", 1}, {"symbol", s.symbol}, {"client_order_id", s.client_order_id},
            {"exchange_order_id", s.exchange_order_id}, {"side", s.side == Side::Buy ? "BUY" : "SELL"},
            {"state", to_string(s.state)}, {"requested_quantity", s.requested_quantity},
            {"executed_quantity", s.executed_quantity}, {"cumulative_quote_quantity", s.cumulative_quote_quantity},
            {"average_fill_price", s.average_fill_price}, {"rejection_reason", s.rejection_reason},
            {"updated_at_ms", milliseconds(s.updated_at)}, {"locally_owned", s.locally_owned},
            {"base_asset", s.base_asset}, {"quote_asset", s.quote_asset},
            {"client_order_aliases", s.client_order_aliases}, {"last_event_time_ms", s.last_event_time_ms},
            {"requires_reconciliation", s.requires_reconciliation},
            {"execution_trade_quantities", std::move(trades)},
            {"commissions", std::move(commissions)}};
    }
    static Snapshot decode(const std::string& payload) {
        const auto v = nlohmann::json::parse(payload);
        if (!v.is_object() || v.size() != 20 || !v.at("format").is_number_integer() || v.at("format") != 1 ||
            !v.at("exchange_order_id").is_number_integer() || !v.at("updated_at_ms").is_number_integer() ||
            !v.at("last_event_time_ms").is_number_integer() || !v.at("locally_owned").is_boolean() ||
            !v.at("requires_reconciliation").is_boolean() ||
            !v.at("execution_trade_quantities").is_array() ||
            !v.at("commissions").is_array() || !v.at("client_order_aliases").is_array())
            throw std::runtime_error("Unsupported or incomplete order journal payload");
        Snapshot s;
        s.symbol = v.at("symbol").get<std::string>(); s.client_order_id = v.at("client_order_id").get<std::string>();
        s.exchange_order_id = v.at("exchange_order_id").get<std::int64_t>();
        const auto side = v.at("side").get<std::string>();
        if (side != "BUY" && side != "SELL") throw std::runtime_error("Unknown persisted order side");
        s.side = side == "BUY" ? Side::Buy : Side::Sell; s.state = decode_state(v.at("state").get<std::string>());
        for (const auto* f : {"requested_quantity", "executed_quantity", "cumulative_quote_quantity", "average_fill_price"})
            if (!v.at(f).is_number()) throw std::runtime_error("Invalid persisted numeric field");
        s.requested_quantity = v.at("requested_quantity").get<double>();
        s.executed_quantity = v.at("executed_quantity").get<double>();
        s.cumulative_quote_quantity = v.at("cumulative_quote_quantity").get<double>();
        s.average_fill_price = v.at("average_fill_price").get<double>();
        s.rejection_reason = v.at("rejection_reason").get<std::string>();
        const auto maximum_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::duration::max()).count();
        const auto& stored_time = v.at("updated_at_ms");
        if ((stored_time.is_number_unsigned() && stored_time.get<std::uint64_t>() > static_cast<std::uint64_t>(maximum_ms)) ||
            (!stored_time.is_number_unsigned() && (stored_time.get<std::int64_t>() < 0 || stored_time.get<std::int64_t>() > maximum_ms)))
            throw std::runtime_error("Persisted execution timestamp is out of range");
        s.updated_at = std::chrono::system_clock::time_point(std::chrono::milliseconds(stored_time.get<std::int64_t>()));
        s.locally_owned = v.at("locally_owned").get<bool>();
        s.requires_reconciliation = v.at("requires_reconciliation").get<bool>();
        s.base_asset = v.at("base_asset").get<std::string>(); s.quote_asset = v.at("quote_asset").get<std::string>();
        s.client_order_aliases = v.at("client_order_aliases").get<std::vector<std::string>>();
        s.last_event_time_ms = v.at("last_event_time_ms").get<std::int64_t>();
        for (const auto& item : v.at("commissions")) {
            if (!item.is_object() || item.size() != 3 || !item.at("trade_id").is_number_integer() || !item.at("amount").is_number())
                throw std::runtime_error("Invalid persisted commission payload");
            if (!s.commissions.emplace(item.at("trade_id").get<std::int64_t>(),
                ExecutionCommission{item.at("asset").get<std::string>(), item.at("amount").get<double>()}).second)
                throw std::runtime_error("Duplicate persisted commission trade identity");
        }
        for (const auto& item : v.at("execution_trade_quantities")) {
            if (!item.is_object() || item.size() != 2 || !item.at("trade_id").is_number_integer() || !item.at("quantity").is_number())
                throw std::runtime_error("Invalid persisted execution trade payload");
            if (!s.execution_trade_quantities.emplace(item.at("trade_id").get<std::int64_t>(), item.at("quantity").get<double>()).second)
                throw std::runtime_error("Duplicate persisted execution trade identity");
        }
        validate(s); return s;
    }
    static std::vector<Snapshot> read_snapshots(Statement& statement, bool indexed = false) {
        std::vector<Snapshot> result;
        while (statement.row()) {
            auto s = decode(statement.string(1));
            if (s.client_order_id != statement.string(0)) throw std::runtime_error("Persisted order identity mismatch");
            if (indexed && (s.symbol != statement.string(2) || s.exchange_order_id != statement.number(3)))
                throw std::runtime_error("Persisted order index contradicts execution identity");
            result.push_back(std::move(s));
        }
        return result;
    }

    sqlite3* db_ = nullptr;
    std::string account_;
    int lease_fd_ = -1;
    bool finished_ = false;
    mutable std::mutex mutex_;
};

} // namespace sentum::order
