#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>

#include <sentum/research/PortfolioResearch.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: sentum_portfolio_research <portfolio-research.json>\n";
            return EXIT_FAILURE;
        }

        const auto config = sentum::research::load_portfolio_research_config(argv[1]);
        const auto risk = load_risk_config("config/risk.json");
        sentum::research::PortfolioResearchRunner runner(risk);
        const auto summary = runner.run(config);
        sentum::research::PortfolioResearchRunner::write_artifact(summary);

        std::cout << std::fixed << std::setprecision(6)
                  << "Portfolio research complete\n"
                  << "Assets: " << summary.assets.size() << '\n'
                  << "Candidate trades: " << summary.candidate_trades << '\n'
                  << "Accepted trades: " << summary.accepted_trades << '\n'
                  << "Rejected trades: " << summary.rejected_trades << '\n'
                  << "Raw net profit: " << summary.raw_combined.net_profit << '\n'
                  << "Portfolio net profit: " << summary.portfolio_filtered.net_profit << '\n'
                  << "Portfolio max drawdown: " << summary.portfolio_filtered.max_drawdown << '\n'
                  << "Portfolio Sharpe: " << summary.portfolio_filtered.sharpe << '\n'
                  << "Artifact: log/portfolio_research_latest.json\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
