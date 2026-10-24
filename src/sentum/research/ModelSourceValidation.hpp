#pragma once

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sqlite3.h>

#include <sentum/research/ModelPromotion.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

namespace sentum::promotion {
namespace source_validation_detail {

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Source experiment: " + message);
}

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

inline Statement query(sqlite3* db, const char* sql, const std::string& run_id) {
    sqlite3_stmt* raw = nullptr;
    const auto result = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    Statement statement(raw, sqlite3_finalize);
    require(result == SQLITE_OK, "cannot query provenance: " + std::string(sqlite3_errmsg(db)));
    require(sqlite3_bind_text(raw, 1, run_id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK,
            "cannot bind provenance run identity");
    return statement;
}

inline bool next(sqlite3_stmt* statement) {
    const auto result = sqlite3_step(statement);
    require(result == SQLITE_ROW || result == SQLITE_DONE,
            "cannot read provenance: " + std::string(sqlite3_errmsg(sqlite3_db_handle(statement))));
    return result == SQLITE_ROW;
}

inline std::string text(sqlite3_stmt* statement, int column) {
    require(sqlite3_column_type(statement, column) == SQLITE_TEXT, "invalid text in registry provenance");
    return std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, column)),
                       static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
}

inline std::int64_t integer(sqlite3_stmt* statement, int column) {
    require(sqlite3_column_type(statement, column) == SQLITE_INTEGER, "invalid integer in registry provenance");
    return sqlite3_column_int64(statement, column);
}

struct RegistrySnapshot {
    nlohmann::json run;
    nlohmann::json datasets = nlohmann::json::array();
    std::map<std::string, std::string> artifacts;
};

inline RegistrySnapshot registry_snapshot(const std::string& path, const std::string& run_id) {
    sqlite3* raw = nullptr;
    const auto result = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    Database db(raw, sqlite3_close);
    require(result == SQLITE_OK, "cannot open registry: " + path);
    require(sqlite3_exec(db.get(), "BEGIN;", nullptr, nullptr, nullptr) == SQLITE_OK,
            "cannot begin a consistent provenance read");

    RegistrySnapshot snapshot;
    {
        auto statement = query(db.get(),
            "SELECT run_id,name,kind,status,started_at_ms,finished_at_ms,git_commit,config_sha256,"
            "risk_sha256,output_directory FROM research_runs WHERE run_id=?;", run_id);
        require(next(statement.get()), "run not found");
        snapshot.run = {{"run_id", text(statement.get(), 0)}, {"name", text(statement.get(), 1)},
            {"kind", text(statement.get(), 2)}, {"status", text(statement.get(), 3)},
            {"started_at_ms", integer(statement.get(), 4)}, {"finished_at_ms", integer(statement.get(), 5)},
            {"git_commit", text(statement.get(), 6)}, {"config_sha256", text(statement.get(), 7)},
            {"risk_sha256", text(statement.get(), 8)}, {"output_directory", text(statement.get(), 9)}};
        require(!next(statement.get()), "duplicate run identity");
    }
    {
        auto statement = query(db.get(),
            "SELECT dataset_id,symbol,source_path,materialized_path,sha256,from_ms,to_ms "
            "FROM research_datasets WHERE run_id=? ORDER BY dataset_id;", run_id);
        while (next(statement.get())) {
            snapshot.datasets.push_back({{"dataset_id", text(statement.get(), 0)},
                {"symbol", text(statement.get(), 1)}, {"source_path", text(statement.get(), 2)},
                {"materialized_path", text(statement.get(), 3)}, {"sha256", text(statement.get(), 4)},
                {"from_ms", integer(statement.get(), 5)}, {"to_ms", integer(statement.get(), 6)}});
        }
    }
    {
        auto statement = query(db.get(), "SELECT path,sha256 FROM research_artifacts WHERE run_id=?;", run_id);
        while (next(statement.get())) {
            require(snapshot.artifacts.emplace(text(statement.get(), 0), text(statement.get(), 1)).second,
                    "duplicate artifact identity");
        }
    }
    require(sqlite3_exec(db.get(), "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK,
            "cannot finish provenance read");
    return snapshot;
}

struct FileSnapshot {
    std::string sha256;
    std::string contents;
};

inline FileSnapshot read_file(const std::filesystem::path& path, bool capture) {
    require(std::filesystem::is_regular_file(path), "missing or non-regular artifact: " + path.string());
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "cannot read artifact: " + path.string());
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(digest && EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) == 1,
            "cannot initialize artifact digest");
    FileSnapshot snapshot;
    char buffer[64 * 1024];
    while (input) {
        input.read(buffer, sizeof(buffer));
        const auto length = static_cast<std::size_t>(input.gcount());
        if (length == 0) continue;
        require(EVP_DigestUpdate(digest.get(), buffer, length) == 1, "cannot hash artifact");
        if (capture) snapshot.contents.append(buffer, length);
    }
    require(input.eof() && !input.bad(), "incomplete artifact read: " + path.string());
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    require(EVP_DigestFinal_ex(digest.get(), bytes, &length) == 1, "cannot finish artifact digest");
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < length; ++i) {
        snapshot.sha256 += hex[bytes[i] >> 4];
        snapshot.sha256 += hex[bytes[i] & 15];
    }
    return snapshot;
}

inline nlohmann::json object(const std::string& contents, const std::string& name) {
    auto value = nlohmann::json::parse(contents);
    require(value.is_object(), name + " must be an object");
    return value;
}

inline std::filesystem::path absolute_path(const std::string& value) {
    require(!value.empty(), "empty artifact path");
    return std::filesystem::absolute(value).lexically_normal();
}

inline bool is_sha256(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

inline double finite_number(const nlohmann::json& object, const char* key) {
    require(object.contains(key) && object.at(key).is_number(), std::string("missing numeric field: ") + key);
    const auto value = object.at(key).get<double>();
    require(std::isfinite(value), std::string("non-finite field: ") + key);
    return value;
}

inline std::size_t nonnegative_integer(const nlohmann::json& object, const char* key) {
    require(object.contains(key) && object.at(key).is_number_integer(),
            std::string("missing integer field: ") + key);
    require(object.at(key).is_number_unsigned() || object.at(key).get<std::int64_t>() >= 0,
            std::string("negative integer field: ") + key);
    return object.at(key).get<std::size_t>();
}

} // namespace source_validation_detail

inline StageEvidence validate_source_experiment(const ModelDefinition& model,
                                                const std::string& registry_path = "log/experiments.sqlite3") {
    using namespace source_validation_detail;
    const auto registry = registry_snapshot(registry_path, model.source_experiment_run);
    const auto& run = registry.run;
    require(run.at("kind") == "research" && run.at("status") == "completed",
            "promotion requires a completed single-asset research run");
    require(run.at("started_at_ms").get<std::int64_t>() > 0 &&
            run.at("finished_at_ms").get<std::int64_t>() >= run.at("started_at_ms").get<std::int64_t>(),
            "invalid run timestamps");
    require(model.source_git_commit.empty() || model.source_git_commit == run.at("git_commit"), "Git commit mismatch");
    require(model.source_config_sha256.empty() || model.source_config_sha256 == run.at("config_sha256"),
            "config hash mismatch");
    const auto root = absolute_path(run.at("output_directory").get<std::string>());
    const auto manifest = object(read_file(root / "manifest.json", true).contents, "manifest");
    require(manifest.value("manifest_version", 0) == 2, "manifest version 2 is required; regenerate legacy research evidence");
    for (const auto& [key, value] : run.items())
        require(manifest.contains(key) && manifest.at(key) == value, "manifest/registry mismatch: " + key);

    require(registry.datasets.size() == 1 && manifest.contains("datasets") &&
            manifest.at("datasets") == registry.datasets, "dataset provenance is missing or inconsistent");
    const auto& dataset = registry.datasets.front();
    require(!dataset.at("dataset_id").get<std::string>().empty() && dataset.at("symbol") == model.symbol,
            "candidate symbol does not match researched dataset");
    const auto dataset_path = absolute_path(dataset.at("materialized_path").get<std::string>());
    require(dataset_path.parent_path() == root / "datasets", "dataset is outside the recorded run directory");
    const auto dataset_hash = dataset.at("sha256").get<std::string>();
    require(is_sha256(dataset_hash) && read_file(dataset_path, false).sha256 == dataset_hash,
            "materialized dataset hash mismatch");

    require(manifest.contains("artifacts") && manifest.at("artifacts").is_array() &&
            manifest.contains("artifact_sha256") && manifest.at("artifact_sha256").is_object(),
            "artifact identities are missing");
    const auto& declared = manifest.at("artifacts");
    const auto& hashes = manifest.at("artifact_sha256");
    require(declared.size() == registry.artifacts.size() && hashes.size() == registry.artifacts.size(),
            "manifest/registry artifact sets differ");
    std::set<std::string> recorded_paths;
    std::set<std::string> names;
    std::map<std::string, FileSnapshot> captured;
    for (const auto& value : declared) {
        require(value.is_string(), "invalid artifact path");
        const auto recorded = value.get<std::string>();
        require(recorded_paths.insert(recorded).second, "duplicate manifest artifact");
        const auto path = absolute_path(recorded);
        require(path.parent_path() == root, "artifact is outside the recorded run directory");
        const auto name = path.filename().string();
        require(names.insert(name).second, "duplicate artifact filename");
        const auto entry = registry.artifacts.find(recorded);
        require(entry != registry.artifacts.end() && is_sha256(entry->second) && hashes.contains(recorded) &&
                hashes.at(recorded) == entry->second, "artifact identity disagrees with registry: " + name);
        const bool capture = name == "research.json" || name == "risk.json" || name == "experiment.json";
        auto file = read_file(path, capture);
        require(file.sha256 == entry->second, "artifact hash mismatch: " + name);
        if (capture) captured.emplace(name, std::move(file));
    }
    for (const auto* name : {"experiment.json", "dataset-catalog.json", "risk.json", "research-config.json",
                             "research.json", "trials.csv", "research-visualization.json"})
        require(names.count(name) == 1, std::string("missing required artifact: ") + name);
    require(captured.at("experiment.json").sha256 == run.at("config_sha256"), "immutable experiment config hash mismatch");
    require(captured.at("risk.json").sha256 == run.at("risk_sha256"), "immutable risk config hash mismatch");

    // Parse the same bytes whose hashes were checked; never reopen evidence after validation.
    const auto research = object(captured.at("research.json").contents, "research report");
    require(research.value("symbol", std::string{}) == model.symbol, "report symbol does not match candidate");
    require(research.contains("generated_at_ms") && research.at("generated_at_ms").is_number_integer() &&
            research.at("generated_at_ms") >= run.at("started_at_ms") &&
            research.at("generated_at_ms") <= run.at("finished_at_ms"),
            "report generation time is outside its recorded source run");
    require(absolute_path(research.at("dataset").get<std::string>()) == dataset_path,
            "report dataset does not match the registered slice");
    require(research.value("holdout_evaluated", false) && research.contains("selected_parameters") &&
            research.at("selected_parameters").is_object() && research.contains("final_holdout") &&
            research.at("final_holdout").is_object(), "selected final holdout evidence is missing");
    const auto& selected = research.at("selected_parameters");
    require(research.contains("leaderboard") && research.at("leaderboard").is_array(), "candidate leaderboard is missing");
    const auto& leaderboard = research.at("leaderboard");
    const auto chosen = std::find_if(leaderboard.begin(), leaderboard.end(), [](const auto& candidate) {
        return candidate.is_object() && candidate.value("eligible", false);
    });
    require(chosen != leaderboard.end() && chosen->at("parameters") == selected,
            "selected parameters do not identify the eligible research candidate");
    const auto lookback = nonnegative_integer(selected, "lookback");
    require(lookback > 0, "invalid selected lookback");
    const auto threshold = finite_number(selected, "entry_threshold");
    const nlohmann::json expected_strategy{{"type", "momentum"},
        {"parameters", {{"lookback", lookback}, {"entry_threshold", threshold}}}};
    require(model.strategy == expected_strategy, "strategy does not match the complete researched candidate");

    auto expected_risk = parse_risk_config(object(captured.at("risk.json").contents, "immutable risk config"));
    expected_risk.stop_loss_percent = finite_number(selected, "stop_loss_percent");
    expected_risk.take_profit_percent = finite_number(selected, "take_profit_percent");
    expected_risk.slippage_percent = finite_number(selected, "slippage_percent");
    const auto current_risk_json = object(read_file(model.risk_config, true).contents, "candidate risk config");
    const auto current_risk = parse_risk_config(current_risk_json);
    require(risk_config_json(current_risk) == risk_config_json(expected_risk),
            "candidate risk configuration does not match the complete researched risk configuration");

    const auto& metrics = research.at("final_holdout");
    StageEvidence evidence;
    evidence.stage = "research";
    evidence.started_at_ms = run.at("started_at_ms").get<std::int64_t>();
    evidence.finished_at_ms = run.at("finished_at_ms").get<std::int64_t>();
    evidence.trades = nonnegative_integer(metrics, "trades");
    evidence.net_profit = finite_number(metrics, "net_profit");
    evidence.max_drawdown = finite_number(metrics, "max_drawdown");
    evidence.profit_factor = finite_number(metrics, "profit_factor");
    evidence.win_rate = finite_number(metrics, "win_rate");
    evidence.expectancy = finite_number(metrics, "expectancy");
    evidence.sharpe = finite_number(metrics, "sharpe");
    evidence.sortino = finite_number(metrics, "sortino");
    require(evidence.max_drawdown >= 0.0 && evidence.profit_factor >= 0.0 &&
            evidence.win_rate >= 0.0 && evidence.win_rate <= 100.0, "invalid final holdout metric ranges");
    evidence.artifact = (root / "research.json").string();
    evidence.definition_revision = model_definition_revision(model, current_risk_json);
    return evidence;
}

} // namespace sentum::promotion
