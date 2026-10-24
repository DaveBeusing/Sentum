#include <sentum/research/ModelPromotion.hpp>
#include <sentum/utils/ConfigLoader.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace sentum::promotion;
const std::string confirmation = "I_APPROVE_MODEL_PROMOTION";
std::filesystem::path executable;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void rejects(const std::function<void()>& action, const std::string& message) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

void write_json(const std::filesystem::path& path, const nlohmann::json& document) {
    std::ofstream file(path);
    file << document.dump(2) << '\n';
    if (!file) throw std::runtime_error("Cannot write fixture");
}

void sql(const std::string& path, const std::string& query) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) throw std::runtime_error("Cannot open fixture database");
    char* error = nullptr;
    const int result = sqlite3_exec(db, query.c_str(), nullptr, nullptr, &error);
    const std::string message = error ? error : "SQLite fixture error";
    sqlite3_free(error);
    sqlite3_close(db);
    if (result != SQLITE_OK) throw std::runtime_error(message);
}

std::int64_t count(const std::string& path, const std::string& table) {
    sqlite3* db = nullptr;
    sqlite3_open(path.c_str(), &db);
    sqlite3_stmt* query = nullptr;
    const auto statement = "SELECT COUNT(*) FROM " + table + ";";
    if (sqlite3_prepare_v2(db, statement.c_str(), -1, &query, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("Cannot query fixture count");
    }
    const auto result = sqlite3_step(query);
    const auto value = result == SQLITE_ROW ? sqlite3_column_int64(query, 0) : -1;
    sqlite3_finalize(query);
    sqlite3_close(db);
    return value;
}

struct Fixture {
    Fixture() : previous(std::filesystem::current_path()) {
        static unsigned sequence = 0;
        root = std::filesystem::temp_directory_path() /
            ("sentum-model-revision-" + std::to_string(getpid()) + "-" + std::to_string(++sequence));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "log");
        std::filesystem::current_path(root);
        risk = risk_config_json(RiskConfig{});
        write_json("risk.json", risk);
        model.model_id = "candidate";
        model.name = "Candidate";
        model.symbol = "BTCUSDT";
        model.source_experiment_run = "source-run";
        model.source_git_commit = "source-commit";
        model.source_config_sha256 = "source-configuration";
        model.risk_config = "risk.json";
        model.strategy = {{"type", "momentum"}, {"parameters", {{"lookback", 2}, {"entry_threshold", 0.001}}}};
        write_json("model.json", model_definition_json(model));
        model = load_model_definition("model.json");
    }
    ~Fixture() {
        std::filesystem::current_path(previous);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path previous, root;
    ModelDefinition model;
    nlohmann::json risk;
};

StageEvidence source_evidence(const ModelDefinition& model) {
    StageEvidence evidence;
    evidence.stage = "research";
    evidence.trades = 50;
    evidence.net_profit = 100;
    evidence.max_drawdown = 10;
    evidence.profit_factor = 2;
    evidence.win_rate = 70;
    evidence.sharpe = 1.5;
    evidence.definition_revision = model_definition_revision(model);
    return evidence;
}

StageEvidence stage_evidence(ModelRegistry& registry, const ModelDefinition& model, const std::string& stage) {
    auto evidence = source_evidence(model);
    evidence.stage = stage;
    evidence.model_revision = registry.require_stage(model, stage);
    return evidence;
}

void register_source(ModelRegistry& registry, const ModelDefinition& model) {
    registry.register_model(model, model_definition_json(model).dump(), source_evidence(model));
}

void reach_paper(ModelRegistry& registry, const ModelDefinition& model) {
    register_source(registry, model);
    registry.promote(model, ModelStage::Shadow, confirmation);
    registry.save_evidence(model, stage_evidence(registry, model, "shadow"));
    registry.promote(model, ModelStage::Paper, confirmation);
}

void identical_registration_is_idempotent() {
    Fixture fixture;
    ModelRegistry registry;
    reach_paper(registry, fixture.model);
    const auto snapshot = registry.snapshot("candidate");
    const auto evidence_count = count("log/models.sqlite3", "stage_evidence");
    registry.register_model(fixture.model, model_definition_json(fixture.model).dump(4), source_evidence(fixture.model));
    require(registry.snapshot("candidate") == snapshot, "Identical registration must preserve approval and timestamps");
    require(registry.require_stage(fixture.model, "paper") == 1, "Initial revision must remain stable");
    require(count("log/models.sqlite3", "stage_evidence") == evidence_count, "Identical registration must not duplicate source evidence");
    require(count("log/models.sqlite3", "model_revisions") == 1, "Identical registration must not create a revision");
}

void validated_source_completes_an_unpopulated_revision() {
    Fixture fixture;
    ModelRegistry registry;
    registry.register_model(fixture.model, model_definition_json(fixture.model).dump());
    rejects([&] { registry.latest_evidence("candidate", "research"); }, "Low-level registration unexpectedly fabricated evidence");
    register_source(registry, fixture.model);
    require(registry.latest_evidence("candidate", "research").model_revision == 1,
            "Fresh source validation must complete an existing unpopulated research revision");
    require(count("log/models.sqlite3", "model_revisions") == 1, "Completing source evidence must preserve identity");
}

void definition_and_policy_changes_are_rejected() {
    Fixture fixture;
    ModelRegistry registry;
    reach_paper(registry, fixture.model);
    const auto evidence = stage_evidence(registry, fixture.model, "paper");
    registry.save_evidence(fixture.model, evidence);
    const std::vector<std::function<void(ModelDefinition&)>> changes = {
        [](auto& model) { model.symbol = "ETHUSDT"; },
        [](auto& model) { model.name += " changed"; },
        [](auto& model) { model.source_experiment_run = "other-run"; },
        [](auto& model) { model.source_git_commit = "other-commit"; },
        [](auto& model) { model.source_config_sha256 = "other-hash"; },
        [](auto& model) { model.strategy["parameters"]["lookback"] = 50; },
        [](auto& model) { model.policy.min_trades = 0; },
        [](auto& model) { model.policy.min_net_profit = -1000; },
        [](auto& model) { model.policy.min_profit_factor = 0; },
        [](auto& model) { model.policy.min_win_rate = 0; },
        [](auto& model) { model.policy.min_sharpe = -100; },
        [](auto& model) { model.policy.max_drawdown = 1000; },
        [](auto& model) { model.document["future_policy"] = "changed"; }
    };
    for (const auto& change : changes) {
        auto changed = fixture.model;
        change(changed);
        rejects([&] { registry.require_stage(changed, "paper"); }, "Changed model was accepted by runtime gate");
        rejects([&] { registry.promote(changed, ModelStage::Testnet, confirmation); }, "Changed model was promoted");
        rejects([&] { registry.save_evidence(changed, evidence); }, "Changed model accepted old evidence");
    }
    auto mismatched_document = model_definition_json(fixture.model);
    mismatched_document["symbol"] = "ETHUSDT";
    rejects([&] { registry.register_model(fixture.model, mismatched_document.dump()); }, "Mismatched registration document was accepted");
    require(registry.stage("candidate") == "paper", "Rejected edits must not change the current stage");
}

void every_risk_field_is_bound() {
    Fixture fixture;
    ModelRegistry registry;
    reach_paper(registry, fixture.model);
    const auto evidence = stage_evidence(registry, fixture.model, "paper");
    for (auto item = fixture.risk.begin(); item != fixture.risk.end(); ++item) {
        auto changed = fixture.risk;
        if (item.value().is_boolean()) changed[item.key()] = !item.value().get<bool>();
        else if (item.value().is_number_integer()) changed[item.key()] = item.value().get<std::int64_t>() + 1;
        else changed[item.key()] = item.value().get<double>() + 0.0001;
        write_json("risk.json", changed);
        rejects([&] { registry.require_stage(fixture.model, "paper"); }, "Risk field escaped revision binding: " + item.key());
        rejects([&] { registry.save_evidence(fixture.model, evidence); }, "Changed risk accepted evidence: " + item.key());
        write_json("risk.json", fixture.risk);
    }
    auto changed = fixture.risk;
    changed["future_execution_cost"] = 1;
    write_json("risk.json", changed);
    rejects([&] { registry.approved_risk(fixture.model, "paper"); }, "Complete risk document must be bound, including new fields");
    write_json("risk.json", fixture.risk);
    require(risk_config_json(registry.approved_risk(fixture.model, "paper")) == fixture.risk,
            "Unchanged approved risk must load exactly");
}

void reregistration_and_reversion_do_not_reuse_old_approval() {
    Fixture fixture;
    ModelRegistry registry;
    reach_paper(registry, fixture.model);
    const auto old_shadow = registry.latest_evidence("candidate", "shadow");
    auto changed = fixture.model;
    changed.symbol = "ETHUSDT";
    changed.source_experiment_run = "other-source";
    register_source(registry, changed);
    require(registry.require_stage(changed, "research") == 2, "Changed registration must begin a new research revision");
    rejects([&] { registry.latest_evidence("candidate", "shadow"); }, "Prior revision's shadow evidence remained active");
    rejects([&] { registry.save_evidence(changed, old_shadow); }, "Old revision evidence was rebound");
    rejects([&] { registry.require_stage(fixture.model, "paper"); }, "Old approved model remained executable after replacement");
    register_source(registry, fixture.model);
    require(registry.require_stage(fixture.model, "research") == 3, "Reverting the content must create a new registration revision");
    registry.promote(fixture.model, ModelStage::Shadow, confirmation);
    rejects([&] { registry.latest_evidence("candidate", "shadow"); }, "A content reversion revived old evidence");
    rejects([&] { registry.save_evidence(fixture.model, old_shadow); }, "Reverted content accepted a stale registration generation");
    require(count("log/models.sqlite3", "model_revisions") == 3, "Immutable revision history must be retained");
    require(count("log/models.sqlite3", "stage_evidence") == 4, "Old evidence must remain available for audit");
}

void evidence_requires_exact_revision_and_stage() {
    Fixture fixture;
    ModelRegistry registry;
    register_source(registry, fixture.model);
    auto unbound = source_evidence(fixture.model);
    rejects([&] { registry.save_evidence("candidate", unbound); }, "Unbound evidence was silently attached");
    auto wrong_stage = stage_evidence(registry, fixture.model, "research");
    wrong_stage.stage = "shadow";
    rejects([&] { registry.save_evidence(fixture.model, wrong_stage); }, "Evidence from a future stage was accepted");
    auto stale = stage_evidence(registry, fixture.model, "research");
    ++stale.model_revision;
    rejects([&] { registry.save_evidence(fixture.model, stale); }, "Wrong registration revision was accepted");
    auto missing_hash = stage_evidence(registry, fixture.model, "research");
    missing_hash.definition_revision.clear();
    rejects([&] { registry.save_evidence(fixture.model, missing_hash); }, "Evidence without a definition hash was accepted");
    auto invalid_count = stage_evidence(registry, fixture.model, "research");
    invalid_count.trades = static_cast<std::size_t>(-1);
    rejects([&] { registry.save_evidence(fixture.model, invalid_count); }, "Wrapped negative trade count bypassed evidence validation");
    auto invalid_metric = stage_evidence(registry, fixture.model, "research");
    invalid_metric.net_profit = std::numeric_limits<double>::quiet_NaN();
    rejects([&] { registry.save_evidence(fixture.model, invalid_metric); }, "Non-finite promotion evidence was accepted");
    invalid_metric = stage_evidence(registry, fixture.model, "research");
    invalid_metric.win_rate = 101;
    rejects([&] { registry.save_evidence(fixture.model, invalid_metric); }, "Invalid win rate was accepted");
    const auto source = source_evidence(fixture.model);
    auto changed_risk = fixture.risk;
    changed_risk["buy_fee_percent"] = 0.002;
    write_json("risk.json", changed_risk);
    rejects([&] { registry.register_model(fixture.model, model_definition_json(fixture.model).dump(), source); },
            "Risk changed after source validation but registration still accepted the old result");
}

void mutations_are_atomic() {
    Fixture fixture;
    ModelRegistry registry;
    sql("log/models.sqlite3", "CREATE TRIGGER fail_evidence BEFORE INSERT ON stage_evidence BEGIN SELECT RAISE(ABORT,'fixture evidence failure'); END;");
    rejects([&] { register_source(registry, fixture.model); }, "Source persistence failure must fail registration");
    require(registry.snapshot("candidate").empty(), "Partial registration survived evidence failure");
    require(count("log/models.sqlite3", "model_revisions") == 0, "Revision history must roll back with failed registration");
    sql("log/models.sqlite3", "DROP TRIGGER fail_evidence;");
    register_source(registry, fixture.model);
    sql("log/models.sqlite3", "CREATE TRIGGER fail_promotion BEFORE UPDATE OF stage ON models BEGIN SELECT RAISE(ABORT,'fixture stage failure'); END;");
    rejects([&] { registry.promote(fixture.model, ModelStage::Shadow, confirmation); }, "Stage persistence failure must fail promotion");
    require(registry.stage("candidate") == "research", "Failed promotion changed stage");
    require(count("log/models.sqlite3", "promotion_events") == 0, "An approved audit event survived a failed stage transition");
}

void legacy_records_require_fresh_registration() {
    Fixture fixture;
    sql("log/models.sqlite3", "CREATE TABLE models(model_id TEXT PRIMARY KEY,name TEXT,symbol TEXT,stage TEXT NOT NULL,source_experiment_run TEXT NOT NULL,source_git_commit TEXT,source_config_sha256 TEXT,definition_json TEXT NOT NULL,created_at_ms INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL); INSERT INTO models VALUES('candidate','Old','BTCUSDT','paper','old-source','','','{}',1,1);");
    ModelRegistry registry;
    require(!registry.snapshot("candidate").at("revision_bound").get<bool>(), "Legacy approval was automatically trusted");
    rejects([&] { registry.require_stage(fixture.model, "paper"); }, "Legacy model passed runtime validation");
    rejects([&] { registry.promote(fixture.model, ModelStage::Testnet, confirmation); }, "Legacy model was promoted");
    register_source(registry, fixture.model);
    require(registry.require_stage(fixture.model, "research") == 1, "Revalidated legacy model must restart at research");
    ModelRegistry reopened;
    require(reopened.require_stage(fixture.model, "research") == 1, "Schema migration must be idempotent");
    require(count("log/models.sqlite3", "model_revisions") == 2, "Legacy definition must remain in the revision audit");
}

void unknown_schema_is_not_downgraded() {
    Fixture fixture;
    sql("log/models.sqlite3", "PRAGMA user_version=2; CREATE TABLE future_marker(value TEXT);");
    rejects([] { ModelRegistry unsupported; }, "Newer registry schema must be rejected");
    sqlite3* db = nullptr;
    sqlite3_open("log/models.sqlite3", &db);
    sqlite3_stmt* query = nullptr;
    sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &query, nullptr);
    sqlite3_step(query);
    const auto version = sqlite3_column_int(query, 0);
    sqlite3_finalize(query);
    sqlite3_close(db);
    require(version == 2, "Opening an unsupported registry downgraded its schema marker");
}

void paper_and_shadow_runtime_paths_check_identity() {
    Fixture fixture;
    ModelRegistry registry;
    register_source(registry, fixture.model);
    registry.promote(fixture.model, ModelStage::Shadow, confirmation);
    const auto risk = parse_risk_config(fixture.risk);
    auto changed_risk = risk;
    changed_risk.buy_fee_percent *= 2;
    rejects([&] { ShadowTradingSession invalid(fixture.model, changed_risk); }, "Shadow constructor accepted different execution risk");
    {
        ShadowTradingSession session(fixture.model, risk);
        auto changed_model = fixture.model;
        changed_model.policy.min_trades = 1;
        rejects([&] { register_source(registry, changed_model); }, "Registration replaced an identity held by a shadow runtime");
        registry.save_evidence(fixture.model, stage_evidence(registry, fixture.model, "shadow"));
        rejects([&] { registry.promote(fixture.model, ModelStage::Paper, confirmation); }, "Promotion changed a running shadow session's stage");
        session.stop();
        register_source(registry, changed_model);
        rejects([&] { session.start(); }, "An obsolete shadow session started after its approval was replaced");
    }
    register_source(registry, fixture.model);
    registry.promote(fixture.model, ModelStage::Shadow, confirmation);
    registry.save_evidence(fixture.model, stage_evidence(registry, fixture.model, "shadow"));
    registry.promote(fixture.model, ModelStage::Paper, confirmation);
    write_json("config.json", {{"paper", {{"modelDefinition", "model.json"}}}});
    const auto config = load_config("config.json");
    require(config.paperModelRevision == 3, "Paper config did not capture its approved revision");
    require(risk_config_json(load_paper_risk_config(config)) == fixture.risk, "Paper must use the approved risk snapshot");
    validate_paper_model_selection(config, fixture.model.symbol, fixture.model.strategy);
    auto changed_model = fixture.model;
    changed_model.symbol = "ETHUSDT";
    rejects([&] { register_source(registry, changed_model); }, "Registration replaced a model held by the Paper runtime");
    rejects([&] { validate_paper_model_selection(config, "ETHUSDT", fixture.model.strategy); }, "Paper symbol override inherited model approval");
    rejects([&] { validate_paper_model_selection(config, fixture.model.symbol, {{"type", "trend"}}); }, "Paper strategy override inherited model approval");
    auto changed_document = model_definition_json(fixture.model);
    changed_document["strategy"]["parameters"]["lookback"] = 3;
    write_json("model.json", changed_document);
    rejects([&] { load_config("config.json"); }, "Tampered model file passed Paper startup");
    rejects([&] { load_paper_risk_config(config); }, "Tampered model file passed trader restart");
    write_json("model.json", model_definition_json(fixture.model));
    auto changed = fixture.risk;
    changed["cooldown_seconds"] = 60;
    write_json("risk.json", changed);
    rejects([&] { load_config("config.json"); }, "Tampered risk file passed Paper startup");
    rejects([&] { load_paper_risk_config(config); }, "Tampered risk file passed trader restart");
}

int lease_probe(const std::string& registry_path, bool blocked) {
    const auto model = load_model_definition("model.json");
    ModelRegistry registry(registry_path);
    register_source(registry, model);
    auto changed = model;
    changed.symbol = "ETHUSDT";
    if (blocked) {
        rejects([&] { register_source(registry, changed); }, "Other process replaced the active runtime revision");
        rejects([&] { registry.promote(model, ModelStage::Paper, confirmation); }, "Other process promoted the active runtime revision");
    } else {
        register_source(registry, changed);
        require(registry.require_stage(changed, "research") == 2, "Released runtime lease did not allow a new revision");
    }
    return 0;
}

void run_lease_probe(const std::string& path, bool blocked) {
    const auto pid = ::fork();
    if (pid < 0) throw std::runtime_error("Cannot start model lease probe");
    if (pid == 0) {
        ::execl(executable.c_str(), executable.c_str(), "--lease-probe", path.c_str(), blocked ? "blocked" : "allowed", nullptr);
        ::_exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("Independent model lease probe failed");
}

void runtime_leases_protect_aliases_across_processes() {
    Fixture fixture;
    ModelRegistry registry;
    register_source(registry, fixture.model);
    registry.promote(fixture.model, ModelStage::Shadow, confirmation);
    registry.save_evidence(fixture.model, stage_evidence(registry, fixture.model, "shadow"));
    std::filesystem::create_symlink("models.sqlite3", "log/models-alias.sqlite3");
    auto lease = registry.acquire_runtime_lease(fixture.model, "shadow");
    const auto alias = (fixture.root / "log/models-alias.sqlite3").string();
    run_lease_probe(alias, true);
    require(registry.require_stage(fixture.model, "shadow") == 1, "Blocked external mutation changed active identity");
    lease.reset();
    run_lease_probe(alias, false);
    require(registry.snapshot("candidate").at("model_revision") == 2, "External registration did not commit after lease release");
}

void concurrent_promotions_cannot_skip_a_stage() {
    Fixture fixture;
    ModelRegistry first;
    register_source(first, fixture.model);
    ModelRegistry second;
    std::atomic<unsigned> successful{0};
    auto promote = [&](ModelRegistry& registry) {
        try { registry.promote(fixture.model, ModelStage::Shadow, confirmation); ++successful; }
        catch (const std::exception&) {}
    };
    std::thread a([&] { promote(first); });
    std::thread b([&] { promote(second); });
    a.join(); b.join();
    require(successful == 1 && first.stage("candidate") == "shadow", "Concurrent promotions did not serialize one stage transition");
    require(count("log/models.sqlite3", "promotion_events") == 1, "Concurrent duplicate promotion produced duplicate approval events");
}
}

int main(int argc, char** argv) {
    try {
        executable = std::filesystem::absolute(argv[0]);
        if (argc == 4 && std::string(argv[1]) == "--lease-probe")
            return lease_probe(argv[2], std::string(argv[3]) == "blocked");
        identical_registration_is_idempotent();
        validated_source_completes_an_unpopulated_revision();
        definition_and_policy_changes_are_rejected();
        every_risk_field_is_bound();
        reregistration_and_reversion_do_not_reuse_old_approval();
        evidence_requires_exact_revision_and_stage();
        mutations_are_atomic();
        legacy_records_require_fresh_registration();
        unknown_schema_is_not_downgraded();
        paper_and_shadow_runtime_paths_check_identity();
        runtime_leases_protect_aliases_across_processes();
        concurrent_promotions_cannot_skip_a_stage();
        std::cout << "model revision tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "model revision tests failed: " << error.what() << '\n';
        return 1;
    }
}
