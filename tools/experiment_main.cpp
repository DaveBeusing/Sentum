#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include <sentum/research/ExperimentRunner.hpp>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: sentum_experiment <experiment.json>\n";
        return EXIT_FAILURE;
    }
    try {
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
