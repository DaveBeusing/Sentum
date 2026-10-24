#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#include <sentum/research/ModelPromotion.hpp>
#include <sentum/research/ModelSourceValidation.hpp>

namespace {
std::atomic<bool> stop_requested{false};
void signal_handler(int) noexcept { stop_requested.store(true, std::memory_order_relaxed); }
std::string read_all(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open file: " + path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void usage() {
    std::cerr << "Usage:\n"
                 "  sentum_model register <model.json>\n"
                 "  sentum_model status <model.json>\n"
                 "  sentum_model shadow <model.json>\n"
                 "  sentum_model record-evidence <model.json> <paper|testnet> <metrics.json>\n"
                 "  sentum_model promote <model.json> <shadow|paper|testnet> I_APPROVE_MODEL_PROMOTION\n";
}
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    try {
        if (argc < 3) { usage(); return EXIT_FAILURE; }
        const std::string command = argv[1], path = argv[2];
        const auto model_document = read_all(path);
        const auto model = sentum::promotion::parse_model_definition(nlohmann::json::parse(model_document));
        sentum::promotion::ModelRegistry registry;
        if (command == "register") {
            const auto research = sentum::promotion::validate_source_experiment(model);
            const auto revision = registry.register_model(model, model_document, research);
            std::cout << "Registered model " << model.model_id << " revision " << revision
                      << " at " << registry.stage(model.model_id) << " stage with validated source experiment "
                      << model.source_experiment_run << '\n';
            return EXIT_SUCCESS;
        }
        if (command == "status") {
            std::cout << registry.snapshot(model.model_id).dump(2) << '\n';
            return EXIT_SUCCESS;
        }
        if (command == "shadow") {
            const auto revision = registry.require_stage(model, "shadow");
            const auto risk = registry.approved_risk(model, "shadow", revision);
            sentum::promotion::ShadowTradingSession session(model, risk);
            session.start();
            std::cout << "Shadow trading " << model.model_id << " revision " << revision << " on live "
                      << model.symbol << " market data. No exchange orders are submitted. Ctrl+C to stop.\n";
            while (!stop_requested.load(std::memory_order_relaxed))
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto evidence = session.stop();
            std::cout << "Shadow evidence: trades=" << evidence.trades << " net=" << evidence.net_profit
                      << " sharpe=" << evidence.sharpe << " maxDD=" << evidence.max_drawdown << '\n';
            return EXIT_SUCCESS;
        }
        if (command == "record-evidence" && argc == 5) {
            const std::string stage = argv[3];
            if (stage != "paper" && stage != "testnet") throw std::runtime_error("Evidence stage must be paper or testnet");
            registry.require_stage(model, stage);
            const auto document = nlohmann::json::parse(read_all(argv[4]));
            if (document.at("model_id").get<std::string>() != model.model_id || document.at("stage").get<std::string>() != stage)
                throw std::runtime_error("Evidence model_id and stage must match the requested model");
            sentum::promotion::StageEvidence evidence;
            evidence.stage = stage;
            evidence.definition_revision = document.at("definition_revision").get<std::string>();
            evidence.model_revision = document.at("model_revision").get<std::int64_t>();
            evidence.started_at_ms = document.value("started_at_ms", std::int64_t{0});
            evidence.finished_at_ms = document.value("finished_at_ms", sentum::research::unix_ms_now());
            const auto trades = document.value("trades", nlohmann::json(0));
            if (!trades.is_number_integer() || (!trades.is_number_unsigned() && trades.get<std::int64_t>() < 0))
                throw std::runtime_error("Evidence trades must be a non-negative integer");
            evidence.trades = trades.get<std::size_t>();
            evidence.net_profit = document.value("net_profit", 0.0);
            evidence.max_drawdown = document.value("max_drawdown", 0.0);
            evidence.profit_factor = document.value("profit_factor", 0.0);
            evidence.win_rate = document.value("win_rate", 0.0);
            evidence.expectancy = document.value("expectancy", 0.0);
            evidence.sharpe = document.value("sharpe", 0.0);
            evidence.sortino = document.value("sortino", 0.0);
            evidence.artifact = argv[4];
            registry.save_evidence(model, evidence);
            std::cout << "Recorded " << stage << " evidence for " << model.model_id << " revision " << evidence.model_revision << '\n';
            return EXIT_SUCCESS;
        }
        if (command == "promote" && argc == 5) {
            registry.promote(model, sentum::promotion::parse_stage(argv[3]), argv[4]);
            std::cout << "Promoted " << model.model_id << " to " << argv[3] << '\n';
            return EXIT_SUCCESS;
        }
        usage();
        return EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
