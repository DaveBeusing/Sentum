#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/research/DatasetCatalog.hpp>
#include <sentum/research/ExperimentManager.hpp>
#include <sentum/research/PortfolioResearch.hpp>
#include <sentum/research/ResearchPlatform.hpp>
#include <sentum/research/ResearchVisualization.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

#ifndef SENTUM_GIT_COMMIT
#define SENTUM_GIT_COMMIT "unknown"
#endif

namespace sentum::research {

struct ExperimentDatasetSelection {
    std::string id;
    std::int64_t from_ms = 0;
    std::int64_t to_ms = 0;
    double weight = 1.0;
};

struct ExperimentSpec {
    std::string name;
    std::string kind = "research";
    std::string dataset_catalog;
    std::string research_config;
    std::string portfolio_config;
    std::string risk_config = "config/risk.json";
    std::string output_root = "log/experiments";
    std::string registry_path = "log/experiments.sqlite3";
    std::vector<ExperimentDatasetSelection> datasets;
};

inline ExperimentSpec load_experiment_spec(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open experiment spec: " + path);
    nlohmann::json json; file >> json;
    ExperimentSpec spec;
    spec.name = json.value("name", std::string{});
    spec.kind = json.value("kind", std::string("research"));
    spec.dataset_catalog = json.value("dataset_catalog", std::string{});
    spec.research_config = json.value("research_config", std::string{});
    spec.portfolio_config = json.value("portfolio_config", std::string{});
    spec.risk_config = json.value("risk_config", spec.risk_config);
    spec.output_root = json.value("output_root", spec.output_root);
    spec.registry_path = json.value("registry_path", spec.registry_path);
    if (spec.name.empty() || spec.dataset_catalog.empty()) throw std::runtime_error("Experiment requires name and dataset_catalog");
    if (spec.kind != "research" && spec.kind != "portfolio") throw std::runtime_error("Experiment kind must be research or portfolio");
    if (spec.kind == "research" && spec.research_config.empty()) throw std::runtime_error("Research experiment requires research_config");
    if (spec.kind == "portfolio" && spec.portfolio_config.empty()) throw std::runtime_error("Portfolio experiment requires portfolio_config");
    if (!json.contains("datasets") || !json.at("datasets").is_array() || json.at("datasets").empty())
        throw std::runtime_error("Experiment requires datasets");
    for (const auto& item : json.at("datasets")) {
        ExperimentDatasetSelection selection;
        selection.id = item.value("id", std::string{});
        selection.from_ms = item.value("from_ms", std::int64_t{0});
        selection.to_ms = item.value("to_ms", std::int64_t{0});
        selection.weight = item.value("weight", 1.0);
        if (selection.id.empty() || !(selection.weight > 0.0) || (selection.from_ms > 0 && selection.to_ms > 0 && selection.to_ms < selection.from_ms))
            throw std::runtime_error("Invalid experiment dataset selection");
        spec.datasets.push_back(std::move(selection));
    }
    if (spec.kind == "research" && spec.datasets.size() != 1)
        throw std::runtime_error("Single research experiment requires exactly one dataset selection");
    return spec;
}

class ExperimentRunner {
public:
    ExperimentManifest run(const ExperimentSpec& spec, const std::string& spec_path) const {
        const auto catalog = DatasetCatalog::load(spec.dataset_catalog);
        const auto risk = load_risk_config(spec.risk_config);
        ExperimentManifest manifest;
        manifest.name = spec.name;
        manifest.kind = spec.kind;
        manifest.started_at_ms = unix_ms_now();
        manifest.git_commit = SENTUM_GIT_COMMIT;
        manifest.config_sha256 = Sha256::file(spec_path);
        manifest.risk_sha256 = Sha256::file(spec.risk_config);
        manifest.run_id = make_run_id(spec.name, manifest.config_sha256, manifest.git_commit, manifest.started_at_ms);
        manifest.output_directory = (std::filesystem::path(spec.output_root) / manifest.run_id).string();
        std::filesystem::create_directories(std::filesystem::path(manifest.output_directory) / "datasets");

        const auto spec_copy = (std::filesystem::path(manifest.output_directory) / "experiment.json").string();
        const auto catalog_copy = (std::filesystem::path(manifest.output_directory) / "dataset-catalog.json").string();
        const auto risk_copy = (std::filesystem::path(manifest.output_directory) / "risk.json").string();
        copy_input(spec_path, spec_copy);
        copy_input(spec.dataset_catalog, catalog_copy);
        copy_input(spec.risk_config, risk_copy);
        manifest.artifacts = {spec_copy, catalog_copy, risk_copy};

        ExperimentRepository repository(spec.registry_path);
        materialize(spec, catalog, manifest);
        write_manifest(manifest);
        repository.save(manifest);

        try {
            if (spec.kind == "research") run_single(spec, risk, manifest);
            else run_portfolio(spec, risk, manifest);
            manifest.status = "completed";
            manifest.finished_at_ms = unix_ms_now();
        } catch (...) {
            manifest.status = "failed";
            manifest.finished_at_ms = unix_ms_now();
            write_manifest(manifest);
            repository.save(manifest);
            throw;
        }

        write_manifest(manifest);
        repository.save(manifest);
        return manifest;
    }

private:
    static void materialize(const ExperimentSpec& spec, const DatasetCatalog& catalog, ExperimentManifest& manifest) {
        for (const auto& selected : spec.datasets) {
            const auto& entry = catalog.by_id(selected.id);
            DatasetSelection selection{entry, selected.from_ms, selected.to_ms};
            const auto filename = entry.id + ".csv";
            const auto output = (std::filesystem::path(manifest.output_directory) / "datasets" / filename).string();
            DatasetCatalog::write_selection_csv(selection, output);
            ExperimentDatasetRecord record;
            record.dataset_id = entry.id;
            record.symbol = entry.symbol;
            record.source_path = entry.path;
            record.materialized_path = output;
            record.sha256 = Sha256::file(output);
            record.from_ms = selected.from_ms > 0 ? selected.from_ms : entry.start_ms;
            record.to_ms = selected.to_ms > 0 ? selected.to_ms : entry.end_ms;
            manifest.datasets.push_back(std::move(record));
        }
    }

    static void run_single(const ExperimentSpec& spec, const RiskConfig& risk, ExperimentManifest& manifest) {
        auto config = load_research_config(spec.research_config);
        config.dataset = manifest.datasets.front().materialized_path;
        config.symbol = manifest.datasets.front().symbol;
        const ResearchRunner runner(risk);
        const auto summary = runner.run(config);
        const auto root = std::filesystem::path(manifest.output_directory);
        const auto json_path = (root / "research.json").string();
        const auto csv_path = (root / "trials.csv").string();
        const auto visual_path = (root / "research-visualization.json").string();
        ResearchRunner::write_artifacts(summary, json_path, csv_path);
        write_research_visualization(build_research_visualization(config, summary, risk), visual_path);
        manifest.artifacts.push_back(json_path);
        manifest.artifacts.push_back(csv_path);
        manifest.artifacts.push_back(visual_path);
        const auto config_copy = (root / "research-config.json").string();
        copy_input(spec.research_config, config_copy);
        manifest.artifacts.push_back(config_copy);
    }

    static void run_portfolio(const ExperimentSpec& spec, const RiskConfig& risk, ExperimentManifest& manifest) {
        auto config = load_portfolio_research_config(spec.portfolio_config);
        config.datasets.clear();
        for (std::size_t i = 0; i < manifest.datasets.size(); ++i) {
            const auto& record = manifest.datasets[i];
            config.datasets.push_back({record.materialized_path, record.symbol, spec.datasets[i].weight});
        }
        PortfolioResearchRunner runner(risk);
        const auto summary = runner.run(config);
        const auto output = (std::filesystem::path(manifest.output_directory) / "portfolio-research.json").string();
        PortfolioResearchRunner::write_artifact(summary, output);
        manifest.artifacts.push_back(output);
        const auto config_copy = (std::filesystem::path(manifest.output_directory) / "portfolio-config.json").string();
        copy_input(spec.portfolio_config, config_copy);
        manifest.artifacts.push_back(config_copy);
    }

    static void copy_input(const std::string& source, const std::string& target) {
        std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
    }
};

} // namespace sentum::research
