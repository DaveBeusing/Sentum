#include "../include/api.hpp"
#include "../include/strategy.hpp"
#include "../include/logger.hpp"
#include "../include/risk.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <iomanip>
#include <algorithm>
#include <cstdlib>

const int MAX_POINTS = 60;
const int SMA_SHORT = 5;
const int SMA_LONG = 20;
const int REFRESH_SECONDS = 1;

void clear_terminal() {
    std::cout << "\033[2J\033[1;1H";
}

void draw_chart(const std::vector<double>& prices, const std::string& symbol) {
    if (prices.empty()) return;

    double min = *std::min_element(prices.begin(), prices.end());
    double max = *std::max_element(prices.begin(), prices.end());
    int height = 10;

    std::cout << "=== 📈 " << symbol << " Live Chart ===\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Current: $" << prices.back() << "  (Min: $" << min << " | Max: $" << max << ")\n\n";

    for (int row = height; row >= 0; --row) {
        for (double p : prices) {
            double scaled = (p - min) / (max - min + 0.0001);
            int level = scaled * height;
            std::cout << (level >= row ? "█" : " ");
        }
        std::cout << "\n";
    }
    std::cout << std::string(prices.size(), '-') << "\n";
}

int main(int argc, char* argv[]) {
    std::string symbol = (argc >= 2) ? argv[1] : "BTCUSDC";
    std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);

    std::vector<double> price_history;
    RiskConfig risk = {1000.0, 0.1, 0.03}; // 1k Max, 10% pro Trade, 3% SL
    bool in_position = false;
    double entry_price = 0.0;

    while (true) {
        double price = get_current_price(symbol);
        if (price > 0) {
            if (price_history.size() >= MAX_POINTS)
                price_history.erase(price_history.begin());
            price_history.push_back(price);
        }

        Signal signal = sma_crossover_strategy(price_history, SMA_SHORT, SMA_LONG);

        if (signal == Signal::BUY && !in_position) {
            double qty = calculate_quantity(1000.0, price, risk);
            // send_signed_order(symbol, std::to_string(qty)); // optional aktivieren
            log_trade("BUY", symbol, price, qty);
            in_position = true;
            entry_price = price;
        }

        if (in_position && (signal == Signal::SELL || should_stop_loss(entry_price, price, risk))) {
            // send_signed_order(symbol, std::to_string(qty)); // optional aktivieren
            log_trade("SELL", symbol, price, 0.0);
            in_position = false;
        }

        clear_terminal();
        draw_chart(price_history, symbol);

        std::this_thread::sleep_for(std::chrono::seconds(REFRESH_SECONDS));
    }

    return 0;
}
