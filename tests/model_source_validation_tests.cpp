#include <sentum/research/ExperimentRunner.hpp>
#include <sentum/research/ModelSourceValidation.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using nlohmann::json;
using namespace sentum::promotion;
using namespace sentum::research;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.is_open(), "cannot read fixture " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& value) {
    std::filesystem::create_directories(path.parent_path().empty() ? "." : path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
    check(output.good(), "cannot write fixture " + path.string());
}

void write_json(const std::filesystem::path& path, const json& value) {
    write(path, value.dump(2) + '\n');
}

class TestDirectory {
public:
    TestDirectory() : previous_(std::filesystem::current_path()) {
        auto pattern = (std::filesystem::temp_directory_path() / "sentum-model-source-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const auto* directory = mkdtemp(buffer.data());
        check(directory != nullptr, "cannot create temporary directory");
        path_ = directory;
        std::filesystem::current_path(path_);
    }
    ~TestDirectory() {
        std::error_code error;
        std::filesystem::current_path(previous_, error);
        std::filesystem::remove_all(path_, error);
    }
private:
    std::filesystem::path previous_, path_;
};

class Fixture {
public:
    Fixture() {
        RiskConfig risk;
        risk.max_total_capital = 1000.0;
        risk.stop_loss_percent = 0.02;
        risk.take_profit_percent = 0.04;
        risk.slippage_percent = 0.0005;
        write_json("base-risk.json", risk_config_json(risk));
        constexpr std::int64_t base_ms = 1710000000000;
        {
            std::ofstream market("market.csv");
            market << "timestamp_ms,price,volume\n";
            market.precision(17);
            for (int i = 0; i < 360; ++i)
                market << base_ms + i * 1000 << ',' << 100.0 + i * 0.015 + std::sin(i / 6.0) * 1.8 << ",1\n";
        }
        write_json("datasets.json", {{"datasets", json::array({{
            {"id", "source-fixture"}, {"symbol", "BTCUSDT"}, {"path", "market.csv"},
            {"interval", "1s"}, {"start_ms", base_ms}, {"end_ms", base_ms + 359000}}})}});
        write_json("research-config.json", {{"dataset", "managed-by-experiment"}, {"symbol", "BTCUSDT"},
            {"train_fraction", 0.60}, {"holdout_fraction", 0.20}, {"walk_forward_folds", 2},
            {"purge_events", 2}, {"embargo_events", 1}, {"min_validation_trades", 0},
            {"max_trials", 1}, {"leaderboard_size", 1}, {"parallelism", 1},
            {"bootstrap_samples", 16}, {"monte_carlo_samples", 16}, {"random_seed", 42},
            {"grid", {{"lookback", {5}}, {"entry_threshold", {0.0005}}, {"stop_loss_percent", {0.01}},
                {"take_profit_percent", {0.03}}, {"slippage_percent", {0.0008}}}}});
        write_json("experiment.json", {{"name", "model-source-validation"}, {"kind", "research"},
            {"dataset_catalog", "datasets.json"}, {"research_config", "research-config.json"},
            {"risk_config", "base-risk.json"}, {"output_root", "experiments"},
            {"registry_path", registry_path}, {"datasets", json::array({{{"id", "source-fixture"}}})}});
        manifest = ExperimentRunner{}.run(load_experiment_spec("experiment.json"), "experiment.json");
        root = manifest.output_directory;
        auto candidate_risk = risk_config_json(risk);
        candidate_risk["stop_loss_percent"] = 0.01;
        candidate_risk["take_profit_percent"] = 0.03;
        candidate_risk["slippage_percent"] = 0.0008;
        write_json("candidate-risk.json", candidate_risk);
        definition.model_id = "source-candidate";
        definition.name = "Source candidate";
        definition.symbol = "BTCUSDT";
        definition.source_experiment_run = manifest.run_id;
        definition.source_git_commit = manifest.git_commit;
        definition.source_config_sha256 = manifest.config_sha256;
        definition.risk_config = "candidate-risk.json";
        definition.strategy = {{"type", "momentum"}, {"parameters", {{"lookback", 5}, {"entry_threshold", 0.0005}}}};
        for (const auto& path : manifest.artifacts) originals.emplace(path, read(path));
        originals.emplace((root / "manifest.json").string(), read(root / "manifest.json"));
        originals.emplace(manifest.datasets.front().materialized_path, read(manifest.datasets.front().materialized_path));
        originals.emplace(definition.risk_config, read(definition.risk_config));
    }

    void restore() const {
        for (const auto& [path, contents] : originals) write(path, contents);
        std::filesystem::remove(registry_path);
        std::filesystem::remove(registry_path + "-wal");
        std::filesystem::remove(registry_path + "-shm");
        ExperimentRepository(registry_path).save(manifest);
    }

    void edit(const std::filesystem::path& path, const std::function<void(json&)>& edit) const {
        auto value = json::parse(read(path));
        edit(value);
        write_json(path, value);
    }

    void sql(const std::string& statement) const {
        sqlite3* raw = nullptr;
        check(sqlite3_open(registry_path.c_str(), &raw) == SQLITE_OK, "cannot open fixture registry");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        char* error = nullptr;
        const auto result = sqlite3_exec(db.get(), statement.c_str(), nullptr, nullptr, &error);
        const auto message = error ? std::string(error) : "SQL failure";
        sqlite3_free(error);
        check(result == SQLITE_OK, message);
    }

    void reseal() const {
        write_manifest(manifest);
        ExperimentRepository(registry_path).save(manifest);
    }

    std::string registry_path = "experiments.sqlite3";
    ExperimentManifest manifest;
    std::filesystem::path root;
    ModelDefinition definition;
    std::map<std::string, std::string> originals;
};

void expect_rejected(Fixture& fixture, const std::string& name, const std::function<void(ModelDefinition&)>& mutate,
                     const std::string& error_fragment = {}) {
    fixture.restore();
    auto model = fixture.definition;
    mutate(model);
    try {
        validate_source_experiment(model, fixture.registry_path);
    } catch (const std::exception& error) {
        check(error_fragment.empty() || std::string(error.what()).find(error_fragment) != std::string::npos,
              name + " failed for an unexpected reason: " + error.what());
        return;
    }
    throw std::runtime_error(name + " accepted invalid source evidence");
}

void test_success(Fixture& fixture) {
    const auto evidence = validate_source_experiment(fixture.definition, fixture.registry_path);
    const auto report = json::parse(read(fixture.root / "research.json"));
    check(evidence.stage == "research" && evidence.trades > 0, "genuine managed research did not return executed holdout evidence");
    check(evidence.trades == report.at("final_holdout").at("trades"), "wrong holdout trade count");
    check(evidence.net_profit == report.at("final_holdout").at("net_profit"), "wrong holdout profit");
    check(evidence.definition_revision == model_definition_revision(fixture.definition), "research evidence is not bound to the validated model");
    check(evidence.model_revision == 0, "source validation must not invent a registry revision");
    check(evidence.started_at_ms == fixture.manifest.started_at_ms && evidence.finished_at_ms == fixture.manifest.finished_at_ms,
          "research evidence lost its source run boundaries");
    check(Sha256::file(fixture.definition.risk_config) != fixture.manifest.risk_sha256,
          "fixture must exercise selected risk parameters that differ from the base configuration");
    const auto absolute_registry = std::filesystem::absolute(fixture.registry_path).string();
    check(validate_source_experiment(fixture.definition, absolute_registry).trades == evidence.trades,
          "absolute registry paths must retain the same provenance");
    auto without_optional_hints = fixture.definition;
    without_optional_hints.source_git_commit.clear();
    without_optional_hints.source_config_sha256.clear();
    check(validate_source_experiment(without_optional_hints, fixture.registry_path).trades == evidence.trades,
          "omitted source hints must still validate the registry-backed provenance");
    ModelRegistry registry("models.sqlite3");
    const auto revision = registry.register_model(fixture.definition, model_definition_json(fixture.definition).dump(), evidence);
    const auto recorded = registry.latest_evidence(fixture.definition.model_id, "research");
    check(revision > 0 && recorded.model_revision == revision && recorded.definition_revision == evidence.definition_revision,
          "registration did not atomically bind the validated source to its immutable model revision");
    check(recorded.net_profit == evidence.net_profit && recorded.trades == evidence.trades,
          "registration changed the validated source metrics");
}

void test_tampered_artifacts(Fixture& fixture) {
    expect_rejected(fixture, "changed holdout PnL", [&](auto&) {
        fixture.edit(fixture.root / "research.json", [](auto& value) { value["final_holdout"]["net_profit"] = 99999.0; });
    }, "artifact hash mismatch");
    expect_rejected(fixture, "changed report and manifest hash", [&](auto&) {
        fixture.edit(fixture.root / "research.json", [](auto& value) { value["final_holdout"]["net_profit"] = 99999.0; });
        write_manifest(fixture.manifest);
    }, "artifact identity disagrees");
    expect_rejected(fixture, "changed immutable base risk", [&](auto&) {
        fixture.edit(fixture.root / "risk.json", [](auto& value) { value["buy_fee_percent"] = 0.1; });
    }, "artifact hash mismatch");
    expect_rejected(fixture, "changed dataset", [&](auto&) {
        std::ofstream(fixture.manifest.datasets.front().materialized_path, std::ios::app) << "1710000400000,500,1\n";
    }, "dataset hash mismatch");
    expect_rejected(fixture, "changed trials", [&](auto&) {
        std::ofstream(fixture.root / "trials.csv", std::ios::app) << "unexpected row\n";
    }, "artifact hash mismatch");
    expect_rejected(fixture, "missing report", [&](auto&) { std::filesystem::remove(fixture.root / "research.json"); }, "missing");
    expect_rejected(fixture, "missing manifest", [&](auto&) { std::filesystem::remove(fixture.root / "manifest.json"); }, "missing");
    expect_rejected(fixture, "legacy manifest", [&](auto&) {
        fixture.edit(fixture.root / "manifest.json", [](auto& value) { value["manifest_version"] = 1; });
    }, "version 2");
    expect_rejected(fixture, "stale run identity", [&](auto&) {
        fixture.edit(fixture.root / "manifest.json", [](auto& value) { value["run_id"] = "another-run"; });
    }, "manifest/registry mismatch");
    expect_rejected(fixture, "missing artifact hash", [&](auto&) {
        fixture.edit(fixture.root / "manifest.json", [](auto& value) { value.erase("artifact_sha256"); });
    }, "artifact identities");
    expect_rejected(fixture, "duplicate manifest artifact", [&](auto&) {
        fixture.edit(fixture.root / "manifest.json", [](auto& value) { value["artifacts"][1] = value["artifacts"][0]; });
    }, "duplicate manifest artifact");
    expect_rejected(fixture, "registry artifact removed", [&](auto&) {
        fixture.sql("DELETE FROM research_artifacts WHERE path LIKE '%research.json';");
    }, "artifact sets differ");
    expect_rejected(fixture, "registry hash changed", [&](auto&) {
        fixture.sql("UPDATE research_artifacts SET sha256='" + std::string(64, '0') + "' WHERE path LIKE '%research.json';");
    }, "artifact identity disagrees");
    expect_rejected(fixture, "registry dataset mismatch", [&](auto&) {
        fixture.sql("UPDATE research_datasets SET symbol='ETHUSDT';");
    }, "dataset provenance");
    expect_rejected(fixture, "registry unavailable", [&](auto&) { std::filesystem::remove(fixture.registry_path); }, "cannot open registry");
    check(!std::filesystem::exists(fixture.registry_path), "validation must not recreate a missing experiment registry");
    expect_rejected(fixture, "invalid registry schema", [&](auto&) { fixture.sql("DROP TABLE research_artifacts;"); }, "cannot query provenance");
    expect_rejected(fixture, "unfinished source run", [&](auto&) { fixture.sql("UPDATE research_runs SET status='started';"); }, "completed single-asset");
    expect_rejected(fixture, "unsupported source kind", [&](auto&) { fixture.sql("UPDATE research_runs SET kind='portfolio';"); }, "completed single-asset");
}

void test_candidate_identity(Fixture& fixture) {
    expect_rejected(fixture, "changed model symbol", [](auto& model) { model.symbol = "ETHUSDT"; }, "candidate symbol");
    expect_rejected(fixture, "changed source run", [](auto& model) { model.source_experiment_run = "missing"; }, "run not found");
    expect_rejected(fixture, "changed source commit", [](auto& model) { model.source_git_commit = "different"; }, "Git commit mismatch");
    expect_rejected(fixture, "changed source config", [](auto& model) { model.source_config_sha256 = std::string(64, '1'); }, "config hash mismatch");
    expect_rejected(fixture, "changed strategy", [](auto& model) { model.strategy["type"] = "trend"; }, "strategy does not match");
    expect_rejected(fixture, "changed lookback", [](auto& model) { model.strategy["parameters"]["lookback"] = 10; }, "strategy does not match");
    expect_rejected(fixture, "changed threshold", [](auto& model) { model.strategy["parameters"]["entry_threshold"] = 0.002; }, "strategy does not match");
    expect_rejected(fixture, "unresearched parameter", [](auto& model) { model.strategy["parameters"]["another_parameter"] = 1; }, "strategy does not match");

    fixture.restore();
    const auto baseline_risk = json::parse(read(fixture.definition.risk_config));
    for (const auto& [field, original] : baseline_risk.items()) {
        expect_rejected(fixture, "changed risk field " + field, [&](auto&) {
            auto changed = baseline_risk;
            if (original.is_boolean()) changed[field] = !original.get<bool>();
            else if (original.is_number_integer()) changed[field] = original.get<std::int64_t>() + 1;
            else changed[field] = original.get<double>() == 0.0 ? 1.0 : original.get<double>() * 1.1;
            write_json(fixture.definition.risk_config, changed);
        }, "complete researched risk configuration");
    }
    expect_rejected(fixture, "base risk used instead of selected grid risk", [&](auto&) {
        write(fixture.definition.risk_config, read(fixture.root / "risk.json"));
    }, "complete researched risk configuration");
    expect_rejected(fixture, "missing candidate risk", [&](auto&) { std::filesystem::remove(fixture.definition.risk_config); }, "missing");
}

void test_consistent_hashes_do_not_replace_candidate_checks(Fixture& fixture) {
    expect_rejected(fixture, "report symbol disagrees with dataset", [&](auto&) {
        fixture.edit(fixture.root / "research.json", [](auto& value) { value["symbol"] = "ETHUSDT"; });
        fixture.reseal();
    }, "report symbol");
    expect_rejected(fixture, "wrong report dataset", [&](auto&) {
        fixture.edit(fixture.root / "research.json", [](auto& value) { value["dataset"] = "another.csv"; });
        fixture.reseal();
    }, "report dataset");
    expect_rejected(fixture, "selected candidate disagrees with leaderboard", [&](auto&) {
        fixture.edit(fixture.root / "research.json", [](auto& value) { value["selected_parameters"]["lookback"] = 10; });
        fixture.reseal();
    }, "eligible research candidate");
    expect_rejected(fixture, "no final holdout", [&](auto&) {
        fixture.edit(fixture.root / "research.json", [](auto& value) { value["holdout_evaluated"] = false; });
        fixture.reseal();
    }, "final holdout evidence");
    expect_rejected(fixture, "stale report from another run", [&](auto&) {
        fixture.edit(fixture.root / "research.json", [&](auto& value) {
            value["generated_at_ms"] = fixture.manifest.started_at_ms - 1;
        });
        fixture.reseal();
    }, "generation time");
    expect_rejected(fixture, "report generated after completion", [&](auto&) {
        fixture.edit(fixture.root / "research.json", [&](auto& value) {
            value["generated_at_ms"] = fixture.manifest.finished_at_ms + 1;
        });
        fixture.reseal();
    }, "generation time");
    expect_rejected(fixture, "missing holdout metric", [&](auto&) {
        fixture.edit(fixture.root / "research.json", [](auto& value) { value["final_holdout"].erase("net_profit"); });
        fixture.reseal();
    }, "missing numeric field");
    expect_rejected(fixture, "negative holdout trade count", [&](auto&) {
        fixture.edit(fixture.root / "research.json", [](auto& value) { value["final_holdout"]["trades"] = -1; });
        fixture.reseal();
    }, "negative integer field");
    expect_rejected(fixture, "null holdout metric", [&](auto&) {
        fixture.edit(fixture.root / "research.json", [](auto& value) { value["final_holdout"]["profit_factor"] = nullptr; });
        fixture.reseal();
    }, "missing numeric field");
    expect_rejected(fixture, "risk hash disagrees with immutable risk", [&](auto&) {
        auto changed = fixture.manifest;
        changed.risk_sha256 = std::string(64, '0');
        write_manifest(changed);
        ExperimentRepository(fixture.registry_path).save(changed);
    }, "immutable risk config hash mismatch");
    expect_rejected(fixture, "missing required registered artifact", [&](auto&) {
        auto changed = fixture.manifest;
        const auto removed = (fixture.root / "trials.csv").string();
        changed.artifacts.erase(std::remove(changed.artifacts.begin(), changed.artifacts.end(), removed), changed.artifacts.end());
        write_manifest(changed);
        fixture.sql("DELETE FROM research_artifacts WHERE path LIKE '%trials.csv';");
    }, "missing required artifact");
}

} // namespace

int main() {
    try {
        TestDirectory directory;
        Fixture fixture;
        test_success(fixture);
        test_tampered_artifacts(fixture);
        test_candidate_identity(fixture);
        test_consistent_hashes_do_not_replace_candidate_checks(fixture);
        fixture.restore();
        test_success(fixture);
        std::cout << "Model source provenance: managed experiment, complete candidate identity and tampering regressions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Model source provenance failed: " << error.what() << '\n';
        return 1;
    }
}
