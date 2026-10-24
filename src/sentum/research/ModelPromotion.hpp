#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <sentum/api/BinanceWebsocketClient.hpp>
#include <sentum/backtest/Backtest.hpp>
#include <sentum/research/ExperimentManager.hpp>
#include <sentum/time/Clock.hpp>
#include <sentum/trader/TradeEngine.hpp>
#include <sentum/trader/strategy/StrategyFramework.hpp>
#include <sentum/trader/types/RiskConfig.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

namespace sentum::promotion {

enum class ModelStage { Research, Shadow, Paper, Testnet };

inline std::string stage_name(ModelStage stage) {
    switch (stage) {
        case ModelStage::Research: return "research";
        case ModelStage::Shadow: return "shadow";
        case ModelStage::Paper: return "paper";
        case ModelStage::Testnet: return "testnet";
    }
    return "unknown";
}

inline ModelStage parse_stage(const std::string& value) {
    if (value == "research") return ModelStage::Research;
    if (value == "shadow") return ModelStage::Shadow;
    if (value == "paper") return ModelStage::Paper;
    if (value == "testnet") return ModelStage::Testnet;
    throw std::runtime_error("Unsupported model stage: " + value);
}

inline bool valid_transition(ModelStage from, ModelStage to) {
    return (from == ModelStage::Research && to == ModelStage::Shadow) ||
           (from == ModelStage::Shadow && to == ModelStage::Paper) ||
           (from == ModelStage::Paper && to == ModelStage::Testnet);
}

struct PromotionPolicy {
    std::size_t min_trades = 30;
    double min_net_profit = 0.0;
    double min_profit_factor = 1.10;
    double min_win_rate = 45.0;
    double min_sharpe = 0.50;
    double max_drawdown = 0.0; // 0 disables absolute cap
};

struct StageEvidence {
    std::string stage;
    std::int64_t started_at_ms = 0;
    std::int64_t finished_at_ms = 0;
    std::size_t trades = 0;
    double net_profit = 0.0;
    double max_drawdown = 0.0;
    double profit_factor = 0.0;
    double win_rate = 0.0;
    double expectancy = 0.0;
    double sharpe = 0.0;
    double sortino = 0.0;
    std::string artifact;
    std::string definition_revision;
    std::int64_t model_revision = 0;
};

struct ModelDefinition {
    std::string model_id;
    std::string name;
    std::string symbol;
    std::string source_experiment_run;
    std::string source_git_commit;
    std::string source_config_sha256;
    std::string risk_config = "config/risk.json";
    nlohmann::json strategy;
    PromotionPolicy policy;
    nlohmann::json document = nlohmann::json::object();
};

inline ModelDefinition parse_model_definition(const nlohmann::json& j) {
    if (!j.is_object()) throw std::runtime_error("Model definition must be a JSON object");
    ModelDefinition d;
    d.document = j;
    d.model_id = j.value("model_id", std::string{});
    d.name = j.value("name", d.model_id);
    d.symbol = j.value("symbol", std::string{});
    d.source_experiment_run = j.value("source_experiment_run", std::string{});
    d.source_git_commit = j.value("source_git_commit", std::string{});
    d.source_config_sha256 = j.value("source_config_sha256", std::string{});
    d.risk_config = j.value("risk_config", d.risk_config);
    d.strategy = j.value("strategy", nlohmann::json::object());
    if (j.contains("promotion_policy")) {
        const auto& p = j.at("promotion_policy");
        d.policy.min_trades = p.value("min_trades", d.policy.min_trades);
        d.policy.min_net_profit = p.value("min_net_profit", d.policy.min_net_profit);
        d.policy.min_profit_factor = p.value("min_profit_factor", d.policy.min_profit_factor);
        d.policy.min_win_rate = p.value("min_win_rate", d.policy.min_win_rate);
        d.policy.min_sharpe = p.value("min_sharpe", d.policy.min_sharpe);
        d.policy.max_drawdown = p.value("max_drawdown", d.policy.max_drawdown);
    }
    if (d.model_id.empty() || d.symbol.empty() || d.source_experiment_run.empty() || !d.strategy.is_object())
        throw std::runtime_error("Model definition requires model_id, symbol, source_experiment_run and strategy");
    return d;
}

inline ModelDefinition load_model_definition(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open model definition: " + path);
    nlohmann::json j; file >> j;
    return parse_model_definition(j);
}

inline nlohmann::json risk_config_json(const RiskConfig& risk) {
    return {
        {"max_total_capital", risk.max_total_capital}, {"risk_per_trade", risk.risk_per_trade},
        {"stop_loss_percent", risk.stop_loss_percent}, {"take_profit_percent", risk.take_profit_percent},
        {"trailing_sl_enabled", risk.trailing_sl_enabled}, {"trailing_sl_percent", risk.trailing_sl_percent},
        {"trailing_tp_enabled", risk.trailing_tp_enabled}, {"trailing_tp_percent", risk.trailing_tp_percent},
        {"buy_fee_percent", risk.buy_fee_percent}, {"sell_fee_percent", risk.sell_fee_percent},
        {"slippage_percent", risk.slippage_percent}, {"spread_percent", risk.spread_percent},
        {"leverage", risk.leverage}, {"min_quantity", risk.min_quantity}, {"max_quantity", risk.max_quantity},
        {"step_size", risk.step_size}, {"min_notional", risk.min_notional},
        {"cooldown_seconds", risk.cooldown_seconds}, {"max_holding_seconds", risk.max_holding_seconds},
        {"max_data_age_ms", risk.max_data_age_ms}
    };
}

inline nlohmann::json model_definition_json(const ModelDefinition& d) {
    if (d.model_id.empty() || d.symbol.empty() || d.source_experiment_run.empty() || !d.strategy.is_object())
        throw std::runtime_error("Model identity requires model_id, symbol, source experiment and strategy");
    for (const auto value : {d.policy.min_net_profit, d.policy.min_profit_factor, d.policy.min_win_rate,
                             d.policy.min_sharpe, d.policy.max_drawdown})
        if (!std::isfinite(value)) throw std::runtime_error("Promotion policy thresholds must be finite");
    if (d.policy.min_trades > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
        throw std::runtime_error("Promotion policy trade count exceeds the supported range");
    auto document = d.document;
    if (!document.is_object()) document = nlohmann::json::object();
    document["model_id"] = d.model_id;
    document["name"] = d.name;
    document["symbol"] = d.symbol;
    document["source_experiment_run"] = d.source_experiment_run;
    document["source_git_commit"] = d.source_git_commit;
    document["source_config_sha256"] = d.source_config_sha256;
    document["risk_config"] = d.risk_config;
    document["strategy"] = d.strategy;
    auto policy = document.value("promotion_policy", nlohmann::json::object());
    if (!policy.is_object()) throw std::runtime_error("Promotion policy must be a JSON object");
    policy["min_trades"] = d.policy.min_trades;
    policy["min_net_profit"] = d.policy.min_net_profit;
    policy["min_profit_factor"] = d.policy.min_profit_factor;
    policy["min_win_rate"] = d.policy.min_win_rate;
    policy["min_sharpe"] = d.policy.min_sharpe;
    policy["max_drawdown"] = d.policy.max_drawdown;
    document["promotion_policy"] = std::move(policy);
    return document;
}

inline nlohmann::json read_model_risk_document(const ModelDefinition& model) {
    std::ifstream file(model.risk_config);
    if (!file) throw std::runtime_error("Cannot open model risk configuration: " + model.risk_config);
    nlohmann::json risk; file >> risk;
    if (!risk.is_object()) throw std::runtime_error("Model risk configuration must be a JSON object");
    return risk;
}

inline std::string model_definition_revision(const ModelDefinition& model, const nlohmann::json& risk_document) {
    const auto definition = model_definition_json(model);
    const auto risk = risk_config_json(parse_risk_config(risk_document));
    // Bind both the complete documents and the effective default-expanded risk.
    // This prevents newly supported fields from silently inheriting old approval.
    return sentum::research::Sha256::string(nlohmann::json{
        {"revision_format", 1}, {"definition", definition},
        {"risk_document", risk_document}, {"effective_risk", risk}
    }.dump());
}

inline std::string model_definition_revision(const ModelDefinition& model) {
    return model_definition_revision(model, read_model_risk_document(model));
}

inline StageEvidence evidence_from_metrics(const std::string& stage, const BacktestMetrics& m,
                                           std::int64_t start, std::int64_t finish, std::string artifact = {}) {
    return {stage,start,finish,m.trades,m.net_profit,m.max_drawdown,m.profit_factor,m.win_rate,m.expectancy,m.sharpe,m.sortino,std::move(artifact)};
}

inline bool passes_policy(const StageEvidence& e, const PromotionPolicy& p, std::string& reason) {
    for (const auto value : {e.net_profit, e.max_drawdown, e.win_rate, e.sharpe})
        if (!std::isfinite(value)) { reason = "non-finite promotion evidence"; return false; }
    if (e.trades < p.min_trades) { reason = "insufficient trades"; return false; }
    if (e.net_profit < p.min_net_profit) { reason = "net profit below threshold"; return false; }
    if (!std::isfinite(e.profit_factor) || e.profit_factor < p.min_profit_factor) { reason = "profit factor below threshold"; return false; }
    if (e.win_rate < p.min_win_rate) { reason = "win rate below threshold"; return false; }
    if (e.sharpe < p.min_sharpe) { reason = "Sharpe below threshold"; return false; }
    if (p.max_drawdown > 0.0 && e.max_drawdown > p.max_drawdown) { reason = "drawdown above threshold"; return false; }
    reason = "promotion gates passed";
    return true;
}

// Runtime leases prevent a registered identity from changing underneath an
// executing immutable snapshot. The operating system releases leases on exit.
class ModelRevisionLease {
public:
    ModelRevisionLease(const std::string& registry, const std::string& model_id, bool exclusive) {
        const auto directory = std::filesystem::weakly_canonical(registry).string() + ".locks";
        std::filesystem::create_directories(directory);
        const auto path = std::filesystem::path(directory) / (sentum::research::Sha256::string(model_id) + ".lock");
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd_ < 0) throw std::runtime_error("Cannot open model revision lease");
        if (::flock(fd_, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(exclusive ?
                "Model revision is in use; stop its runtimes before registration or promotion" :
                "Model revision is being changed; retry runtime startup after registration or promotion");
        }
    }
    ~ModelRevisionLease() { if (fd_ >= 0) ::close(fd_); }
    ModelRevisionLease(const ModelRevisionLease&) = delete;
    ModelRevisionLease& operator=(const ModelRevisionLease&) = delete;
private:
    int fd_ = -1;
};

class ModelRegistry {
public:
    explicit ModelRegistry(std::string path = "log/models.sqlite3") : path_(std::move(path)) {
        const auto parent = std::filesystem::path(path_).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        if (sqlite3_open_v2(path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("Cannot open model registry");
        }
        try {
            sqlite3_busy_timeout(db_, 2500);
            {
                auto version = statement("PRAGMA user_version;");
                if (!row(version.get()) || sqlite3_column_int(version.get(), 0) > 1)
                    throw std::runtime_error("Unsupported newer model registry schema");
            }
            exec("PRAGMA journal_mode=WAL;");
            exec("PRAGMA synchronous=FULL;");
            Transaction transaction(*this);
            exec("CREATE TABLE IF NOT EXISTS models(model_id TEXT PRIMARY KEY,name TEXT,symbol TEXT,stage TEXT NOT NULL,source_experiment_run TEXT NOT NULL,source_git_commit TEXT,source_config_sha256 TEXT,definition_json TEXT NOT NULL,created_at_ms INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL);");
            exec("CREATE TABLE IF NOT EXISTS stage_evidence(id INTEGER PRIMARY KEY AUTOINCREMENT,model_id TEXT NOT NULL,stage TEXT NOT NULL,started_at_ms INTEGER,finished_at_ms INTEGER,trades INTEGER,net_profit REAL,max_drawdown REAL,profit_factor REAL,win_rate REAL,expectancy REAL,sharpe REAL,sortino REAL,artifact TEXT);");
            exec("CREATE TABLE IF NOT EXISTS promotion_events(id INTEGER PRIMARY KEY AUTOINCREMENT,model_id TEXT NOT NULL,from_stage TEXT NOT NULL,to_stage TEXT NOT NULL,approved INTEGER NOT NULL,reason TEXT NOT NULL,operator_confirmation TEXT,created_at_ms INTEGER NOT NULL);");
            for (const auto* table : {"models", "stage_evidence", "promotion_events"}) {
                add_column(table, "definition_revision", "TEXT NOT NULL DEFAULT ''");
                add_column(table, "model_revision", "INTEGER NOT NULL DEFAULT 0");
            }
            exec("CREATE TABLE IF NOT EXISTS model_revisions(model_id TEXT NOT NULL,model_revision INTEGER NOT NULL,definition_revision TEXT NOT NULL,definition_json TEXT NOT NULL,risk_json TEXT NOT NULL,created_at_ms INTEGER NOT NULL,PRIMARY KEY(model_id,model_revision));");
            exec("INSERT OR IGNORE INTO model_revisions(model_id,model_revision,definition_revision,definition_json,risk_json,created_at_ms) SELECT model_id,0,'',definition_json,'{}',created_at_ms FROM models WHERE model_revision=0;");
            exec("CREATE INDEX IF NOT EXISTS stage_evidence_revision ON stage_evidence(model_id,model_revision,definition_revision,stage,id);");
            exec("PRAGMA user_version=1;");
            transaction.commit();
        } catch (...) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }

    ~ModelRegistry() { if (db_) sqlite3_close(db_); }
    ModelRegistry(const ModelRegistry&) = delete;
    ModelRegistry& operator=(const ModelRegistry&) = delete;

    std::int64_t register_model(const ModelDefinition& d, const std::string& document) {
        return register_model_impl(d, document, nullptr);
    }

    // Registration and the freshly validated source evidence form one transaction.
    std::int64_t register_model(const ModelDefinition& d, const std::string& document, const StageEvidence& source) {
        return register_model_impl(d, document, &source);
    }

    std::string stage(const std::string& model_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return require_current(model_id).stage;
    }

    std::int64_t require_revision(const ModelDefinition& d) const {
        const auto hash = model_definition_revision(d);
        std::lock_guard<std::mutex> lock(mutex_);
        return require_bound(d.model_id, hash).number;
    }

    std::int64_t require_stage(const ModelDefinition& d, const std::string& expected) const {
        const auto hash = model_definition_revision(d);
        std::lock_guard<std::mutex> lock(mutex_);
        const auto row = require_bound(d.model_id, hash);
        if (row.stage != expected) throw std::runtime_error("Model requires stage " + expected + "; current stage=" + row.stage);
        return row.number;
    }

    std::shared_ptr<ModelRevisionLease> acquire_runtime_lease(const ModelDefinition& d, const std::string& expected,
                                                            std::int64_t expected_revision = 0) const {
        auto lease = std::make_shared<ModelRevisionLease>(path_, d.model_id, false);
        const auto revision = require_stage(d, expected);
        if (expected_revision != 0 && expected_revision != revision)
            throw std::runtime_error("Model revision changed; reload the approved model before execution");
        return lease;
    }

    RiskConfig approved_risk(const ModelDefinition& d, const std::string& expected,
                             std::int64_t expected_revision = 0) const {
        const auto document = read_model_risk_document(d);
        const auto risk = parse_risk_config(document);
        const auto hash = model_definition_revision(d, document);
        std::lock_guard<std::mutex> lock(mutex_);
        const auto row = require_bound(d.model_id, hash);
        if (row.stage != expected || (expected_revision != 0 && row.number != expected_revision))
            throw std::runtime_error("Model stage or revision changed; reload the approved model before execution");
        return risk;
    }

    void save_evidence(const ModelDefinition& d, const StageEvidence& evidence) {
        const auto hash = model_definition_revision(d);
        std::lock_guard<std::mutex> lock(mutex_);
        Transaction transaction(*this);
        const auto row = require_bound(d.model_id, hash);
        insert_evidence(d.model_id, row, evidence);
        transaction.commit();
    }

    // Identity-only callers must supply both revision identifiers explicitly.
    void save_evidence(const std::string& model_id, const StageEvidence& evidence) {
        std::lock_guard<std::mutex> lock(mutex_);
        Transaction transaction(*this);
        const auto row = require_bound(model_id, evidence.definition_revision);
        insert_evidence(model_id, row, evidence);
        transaction.commit();
    }

    StageEvidence latest_evidence(const std::string& model_id, const std::string& stage_value) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto row = require_current(model_id);
        validate_bound(row);
        return find_evidence(model_id, row, stage_value);
    }

    void promote(const ModelDefinition& d, ModelStage target, const std::string& confirmation) {
        if (target == ModelStage::Research) throw std::runtime_error("Cannot promote to research");
        const auto hash = model_definition_revision(d);
        ModelRevisionLease lease(path_, d.model_id, true);
        std::lock_guard<std::mutex> lock(mutex_);
        Transaction transaction(*this);
        const auto row = require_bound(d.model_id, hash);
        const auto current = parse_stage(row.stage);
        if (!valid_transition(current, target))
            throw std::runtime_error("Invalid promotion transition: " + row.stage + " -> " + stage_name(target));
        if (confirmation != "I_APPROVE_MODEL_PROMOTION") throw std::runtime_error("Explicit operator confirmation required");
        const auto evidence = find_evidence(d.model_id, row, row.stage);
        std::string reason;
        const bool approved = passes_policy(evidence, d.policy, reason);
        auto event = statement("INSERT INTO promotion_events(model_id,from_stage,to_stage,approved,reason,operator_confirmation,created_at_ms,definition_revision,model_revision) VALUES(?,?,?,?,?,?,?,?,?);");
        bind(event.get(), 1, d.model_id); bind(event.get(), 2, row.stage); bind(event.get(), 3, stage_name(target));
        sqlite3_bind_int(event.get(), 4, approved ? 1 : 0); bind(event.get(), 5, reason); bind(event.get(), 6, confirmation);
        sqlite3_bind_int64(event.get(), 7, sentum::research::unix_ms_now()); bind(event.get(), 8, row.hash);
        sqlite3_bind_int64(event.get(), 9, row.number); done(event.get());
        if (approved) {
            auto update = statement("UPDATE models SET stage=?,updated_at_ms=? WHERE model_id=?;");
            bind(update.get(), 1, stage_name(target)); sqlite3_bind_int64(update.get(), 2, sentum::research::unix_ms_now());
            bind(update.get(), 3, d.model_id); done(update.get());
        }
        transaction.commit();
        if (!approved) throw std::runtime_error("Promotion blocked: " + reason);
    }

    nlohmann::json snapshot(const std::string& model_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto query = statement("SELECT model_id,name,symbol,stage,source_experiment_run,source_git_commit,source_config_sha256,created_at_ms,updated_at_ms,definition_revision,model_revision FROM models WHERE model_id=?;");
        bind(query.get(), 1, model_id);
        if (!row(query.get())) return nlohmann::json::object();
        return {{"model_id", text(query.get(), 0)}, {"name", text(query.get(), 1)}, {"symbol", text(query.get(), 2)},
                {"stage", text(query.get(), 3)}, {"source_experiment_run", text(query.get(), 4)},
                {"source_git_commit", text(query.get(), 5)}, {"source_config_sha256", text(query.get(), 6)},
                {"created_at_ms", sqlite3_column_int64(query.get(), 7)}, {"updated_at_ms", sqlite3_column_int64(query.get(), 8)},
                {"definition_revision", text(query.get(), 9)}, {"model_revision", sqlite3_column_int64(query.get(), 10)},
                {"revision_bound", !text(query.get(), 9).empty() && sqlite3_column_int64(query.get(), 10) > 0}};
    }

private:
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
    struct Revision { std::string stage; std::string hash; std::int64_t number = 0; };
    struct Transaction {
        explicit Transaction(ModelRegistry& registry) : registry(registry) { registry.exec("BEGIN IMMEDIATE;"); }
        ~Transaction() { if (!committed) sqlite3_exec(registry.db_, "ROLLBACK;", nullptr, nullptr, nullptr); }
        void commit() { registry.exec("COMMIT;"); committed = true; }
        ModelRegistry& registry;
        bool committed = false;
    };

    std::int64_t register_model_impl(const ModelDefinition& d, const std::string& document, const StageEvidence* source) {
        const auto normalized = model_definition_json(d);
        if (model_definition_json(parse_model_definition(nlohmann::json::parse(document))) != normalized)
            throw std::runtime_error("Registration document does not match the supplied model definition");
        const auto risk = read_model_risk_document(d);
        const auto hash = model_definition_revision(d, risk);
        if (source && (source->stage != "research" || source->definition_revision != hash || source->model_revision != 0))
            throw std::runtime_error("Source evidence does not match the model definition being registered");
        std::lock_guard<std::mutex> lock(mutex_);
        Transaction transaction(*this);
        const auto previous = current(d.model_id);
        if (previous && previous->hash == hash && previous->number > 0) {
            if (source && previous->stage == "research") {
                auto existing = statement("SELECT 1 FROM stage_evidence WHERE model_id=? AND model_revision=? AND definition_revision=? AND stage='research' LIMIT 1;");
                bind(existing.get(), 1, d.model_id); sqlite3_bind_int64(existing.get(), 2, previous->number);
                bind(existing.get(), 3, hash);
                if (!row(existing.get())) {
                    auto evidence = *source;
                    evidence.model_revision = previous->number;
                    insert_evidence(d.model_id, *previous, evidence);
                }
            }
            transaction.commit();
            return previous->number;
        }
        ModelRevisionLease lease(path_, d.model_id, true);
        if (previous && previous->number == std::numeric_limits<std::int64_t>::max())
            throw std::runtime_error("Model revision counter exhausted");
        const auto number = previous ? previous->number + 1 : 1;
        const auto now = sentum::research::unix_ms_now();
        auto revision = statement("INSERT INTO model_revisions(model_id,model_revision,definition_revision,definition_json,risk_json,created_at_ms) VALUES(?,?,?,?,?,?);");
        bind(revision.get(), 1, d.model_id); sqlite3_bind_int64(revision.get(), 2, number); bind(revision.get(), 3, hash);
        bind(revision.get(), 4, normalized.dump()); bind(revision.get(), 5, risk.dump()); sqlite3_bind_int64(revision.get(), 6, now);
        done(revision.get());
        auto update = statement("INSERT INTO models(model_id,name,symbol,stage,source_experiment_run,source_git_commit,source_config_sha256,definition_json,created_at_ms,updated_at_ms,definition_revision,model_revision) VALUES(?,?,?,'research',?,?,?,?,?,?,?,?) ON CONFLICT(model_id) DO UPDATE SET name=excluded.name,symbol=excluded.symbol,stage='research',source_experiment_run=excluded.source_experiment_run,source_git_commit=excluded.source_git_commit,source_config_sha256=excluded.source_config_sha256,definition_json=excluded.definition_json,updated_at_ms=excluded.updated_at_ms,definition_revision=excluded.definition_revision,model_revision=excluded.model_revision;");
        bind(update.get(), 1, d.model_id); bind(update.get(), 2, d.name); bind(update.get(), 3, d.symbol);
        bind(update.get(), 4, d.source_experiment_run); bind(update.get(), 5, d.source_git_commit);
        bind(update.get(), 6, d.source_config_sha256); bind(update.get(), 7, normalized.dump());
        sqlite3_bind_int64(update.get(), 8, now); sqlite3_bind_int64(update.get(), 9, now);
        bind(update.get(), 10, hash); sqlite3_bind_int64(update.get(), 11, number); done(update.get());
        if (source) {
            auto evidence = *source;
            evidence.model_revision = number;
            insert_evidence(d.model_id, {"research", hash, number}, evidence);
        }
        transaction.commit();
        return number;
    }

    std::optional<Revision> current(const std::string& id) const {
        auto query = statement("SELECT stage,definition_revision,model_revision FROM models WHERE model_id=?;");
        bind(query.get(), 1, id);
        if (!row(query.get())) return std::nullopt;
        return Revision{text(query.get(), 0), text(query.get(), 1), sqlite3_column_int64(query.get(), 2)};
    }
    Revision require_current(const std::string& id) const {
        const auto result = current(id);
        if (!result) throw std::runtime_error("Unknown model: " + id);
        return *result;
    }
    static void validate_bound(const Revision& revision) {
        if (revision.hash.empty() || revision.number <= 0)
            throw std::runtime_error("Legacy model has no immutable revision; register it again with verified research evidence");
    }
    Revision require_bound(const std::string& id, const std::string& hash) const {
        const auto revision = require_current(id);
        validate_bound(revision);
        if (hash.empty() || revision.hash != hash)
            throw std::runtime_error("Model definition or risk configuration differs from the registered revision: " + id);
        return revision;
    }
    void insert_evidence(const std::string& id, const Revision& revision, const StageEvidence& e) {
        if (e.definition_revision != revision.hash || e.model_revision != revision.number || e.stage != revision.stage)
            throw std::runtime_error("Evidence must identify the current model revision and stage");
        if (e.trades > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()) ||
            e.started_at_ms < 0 || e.finished_at_ms < e.started_at_ms || e.max_drawdown < 0 ||
            e.profit_factor < 0 || e.win_rate < 0 || e.win_rate > 100)
            throw std::runtime_error("Promotion evidence contains an invalid count, time or metric range");
        for (const auto value : {e.net_profit, e.max_drawdown, e.profit_factor, e.win_rate, e.expectancy, e.sharpe, e.sortino})
            if (!std::isfinite(value)) throw std::runtime_error("Promotion evidence metrics must be finite");
        auto insert = statement("INSERT INTO stage_evidence(model_id,stage,started_at_ms,finished_at_ms,trades,net_profit,max_drawdown,profit_factor,win_rate,expectancy,sharpe,sortino,artifact,definition_revision,model_revision) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
        bind(insert.get(), 1, id); bind(insert.get(), 2, e.stage);
        sqlite3_bind_int64(insert.get(), 3, e.started_at_ms); sqlite3_bind_int64(insert.get(), 4, e.finished_at_ms);
        sqlite3_bind_int64(insert.get(), 5, static_cast<sqlite3_int64>(e.trades));
        sqlite3_bind_double(insert.get(), 6, e.net_profit); sqlite3_bind_double(insert.get(), 7, e.max_drawdown);
        sqlite3_bind_double(insert.get(), 8, e.profit_factor); sqlite3_bind_double(insert.get(), 9, e.win_rate);
        sqlite3_bind_double(insert.get(), 10, e.expectancy); sqlite3_bind_double(insert.get(), 11, e.sharpe);
        sqlite3_bind_double(insert.get(), 12, e.sortino); bind(insert.get(), 13, e.artifact);
        bind(insert.get(), 14, revision.hash); sqlite3_bind_int64(insert.get(), 15, revision.number); done(insert.get());
    }
    StageEvidence find_evidence(const std::string& id, const Revision& revision, const std::string& stage_value) const {
        auto query = statement("SELECT stage,started_at_ms,finished_at_ms,trades,net_profit,max_drawdown,profit_factor,win_rate,expectancy,sharpe,sortino,artifact FROM stage_evidence WHERE model_id=? AND stage=? AND definition_revision=? AND model_revision=? ORDER BY id DESC LIMIT 1;");
        bind(query.get(), 1, id); bind(query.get(), 2, stage_value); bind(query.get(), 3, revision.hash);
        sqlite3_bind_int64(query.get(), 4, revision.number);
        if (!row(query.get())) throw std::runtime_error("No evidence for current model revision at stage " + stage_value);
        StageEvidence e;
        e.stage = text(query.get(), 0); e.started_at_ms = sqlite3_column_int64(query.get(), 1);
        e.finished_at_ms = sqlite3_column_int64(query.get(), 2); e.trades = static_cast<std::size_t>(sqlite3_column_int64(query.get(), 3));
        e.net_profit = sqlite3_column_double(query.get(), 4); e.max_drawdown = sqlite3_column_double(query.get(), 5);
        e.profit_factor = sqlite3_column_double(query.get(), 6); e.win_rate = sqlite3_column_double(query.get(), 7);
        e.expectancy = sqlite3_column_double(query.get(), 8); e.sharpe = sqlite3_column_double(query.get(), 9);
        e.sortino = sqlite3_column_double(query.get(), 10); e.artifact = text(query.get(), 11);
        e.definition_revision = revision.hash; e.model_revision = revision.number;
        return e;
    }
    void add_column(const std::string& table, const std::string& name, const std::string& type) {
        auto query = statement(("PRAGMA table_info(" + table + ");").c_str());
        while (row(query.get())) if (text(query.get(), 1) == name) return;
        query.reset();
        exec(("ALTER TABLE " + table + " ADD COLUMN " + name + " " + type + ";").c_str());
    }
    Statement statement(const char* sql) const {
        sqlite3_stmt* result = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &result, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
        return Statement(result, &sqlite3_finalize);
    }
    bool row(sqlite3_stmt* query) const {
        const auto result = sqlite3_step(query);
        if (result != SQLITE_ROW && result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
        return result == SQLITE_ROW;
    }
    void done(sqlite3_stmt* query) { if (sqlite3_step(query) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_)); }
    void exec(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : "Model registry SQLite error";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }
    static std::string text(sqlite3_stmt* query, int i) {
        const auto* value = sqlite3_column_text(query, i);
        return value ? reinterpret_cast<const char*>(value) : "";
    }
    static void bind(sqlite3_stmt* query, int i, const std::string& value) {
        if (sqlite3_bind_text(query, i, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("Cannot bind model registry value");
    }
    std::string path_;
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
};

class ShadowTradingSession {
public:
    ShadowTradingSession(ModelDefinition definition, RiskConfig risk, std::string registry_path = "log/models.sqlite3")
        : definition_(std::move(definition)), registry_path_(std::move(registry_path)),
          lease_(ModelRegistry(registry_path_).acquire_runtime_lease(definition_, "shadow")),
          model_revision_(ModelRegistry(registry_path_).require_stage(definition_, "shadow")),
          definition_revision_(model_definition_revision(definition_)),
          risk_(checked_risk(definition_, risk, registry_path_, model_revision_)),
          clock_(std::make_shared<SystemClock>()),
          engine_(definition_.symbol,risk_,clock_,sentum::strategy::StrategyFactory::create(definition_.strategy),shadow_db_path()),
          stream_(definition_.symbol) {}

    void start() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (running_.load()) return;
        if (!lease_) lease_ = ModelRegistry(registry_path_).acquire_runtime_lease(definition_, "shadow", model_revision_);
        (void)checked_risk(definition_, risk_, registry_path_, model_revision_);
        if(running_.exchange(true)) return;
        started_at_ms_=sentum::research::unix_ms_now();
        stream_.set_on_price([this](double price){if(!running_.load())return; MarketEvent e;e.type=MarketEvent::Type::Trade;e.symbol=definition_.symbol;e.price=price;e.close=price;e.timestamp=clock_->now();std::lock_guard<std::mutex> lock(mutex_);engine_.process_event(e);});
        stream_.start();
    }

    StageEvidence stop() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if(!running_.exchange(false)) { lease_.reset(); return last_; }
        auto lease = std::move(lease_);
        stream_.stop();
        std::lock_guard<std::mutex> lock(mutex_);
        const auto metrics=MetricsCalculator::calculate(engine_.completed_trades());
        last_=evidence_from_metrics("shadow",metrics,started_at_ms_,sentum::research::unix_ms_now(),shadow_db_path());
        last_.definition_revision = definition_revision_;
        last_.model_revision = model_revision_;
        ModelRegistry(registry_path_).save_evidence(definition_,last_);
        write_report(last_);
        return last_;
    }
    ~ShadowTradingSession(){try{stop();}catch(...){}}
private:
    static RiskConfig checked_risk(const ModelDefinition& definition, const RiskConfig& supplied,
                                   const std::string& registry, std::int64_t revision) {
        const auto approved = ModelRegistry(registry).approved_risk(definition, "shadow", revision);
        if (risk_config_json(approved) != risk_config_json(supplied))
            throw std::runtime_error("Shadow execution risk differs from the approved model revision");
        return approved;
    }
    std::string shadow_db_path() const {
        std::filesystem::create_directories("log/shadow");
        return "log/shadow/" + definition_.model_id + "-r" + std::to_string(model_revision_) + ".sqlite3";
    }
    void write_report(const StageEvidence&e)const{nlohmann::json j{{"model_id",definition_.model_id},{"definition_revision",e.definition_revision},{"model_revision",e.model_revision},{"stage",e.stage},{"started_at_ms",e.started_at_ms},{"finished_at_ms",e.finished_at_ms},{"trades",e.trades},{"net_profit",e.net_profit},{"max_drawdown",e.max_drawdown},{"profit_factor",e.profit_factor},{"win_rate",e.win_rate},{"expectancy",e.expectancy},{"sharpe",e.sharpe},{"sortino",e.sortino},{"artifact",e.artifact}};std::ofstream("log/shadow/"+definition_.model_id+"-latest.json")<<j.dump(2)<<'\n';}
    ModelDefinition definition_;std::string registry_path_;std::shared_ptr<ModelRevisionLease> lease_;std::int64_t model_revision_;std::string definition_revision_;RiskConfig risk_;std::shared_ptr<SystemClock> clock_;TradeEngine engine_;BinanceWebsocketClient stream_;std::atomic<bool> running_{false};std::mutex lifecycle_mutex_;std::mutex mutex_;std::int64_t started_at_ms_=0;StageEvidence last_;
};

} // namespace sentum::promotion
