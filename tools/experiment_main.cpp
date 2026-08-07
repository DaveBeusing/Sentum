#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include <sentum/research/DatasetCatalog.hpp>
#include <sentum/research/ExperimentRunner.hpp>

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--list-datasets") {
            const auto catalog = sentum::research::DatasetCatalog::load(argv[2]);
            for (const auto& entry : catalog.entries()) {
                std::cout << entry.id << '\t' << entry.symbol << '\t' << entry.interval
                          << '\t' << entry.start_ms << '\t' << entry.end_ms
                          << '\t' << entry.path << '\n';
            }
            return EXIT_SUCCESS;
        }

        if (argc != 2) {
            std::cerr << "Usage:\n"
                      << "  sentum_experiment <experiment.json>\n"
                      << "  sentum_experiment --list-datasets <catalog.json>\n";
            return EXIT_FAILURE;
        }

        const std::string spec_path = argv[1];
        const auto spec = sentum::research::load_experiment_spec(spec_path);
        sentum::research::ExperimentRunner runner;
        const auto manifest = runner.run(spec, spec_path);
        std::cout << "Experiment completed\n"
                  << "Run ID: " << manifest.run_id << '\n'
                  << "Kind: " << manifest.kind << '\n'
                  << "Git commit: " << manifest.git_commit << '\n'
                  << "Config SHA256: " << manifest.config_sha256 << '\n'
                  << "Risk SHA256: " << manifest.risk_sha256 << '\n'
                  << "Output: " << manifest.output_directory << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        std::cerr << "[FATAL] " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
