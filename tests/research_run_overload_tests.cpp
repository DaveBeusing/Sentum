#include <sentum/research/ResearchPlatform.hpp>
#include <stdexcept>
#include <string>
#include <iostream>

int main() {
    sentum::research::ResearchConfig config;
    config.dataset = "/nonexistent/sentum-research-overload-input.csv";
    config.symbol = "BTCUSDT";
    RiskConfig risk{};
    sentum::research::ResearchRunner mutable_runner(risk);
    const sentum::research::ResearchRunner const_runner(risk);
    auto rejects = [&](const auto& runner) {
        try {
            (void)runner.run(config);
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    if (!rejects(mutable_runner) || !rejects(const_runner)) {
        std::cerr << "Research runner overload bypassed validation\n";
        return 1;
    }
    return 0;
}
