#include "../include/rsi.hpp"
#include <numeric>
#include <cmath>

double calculate_rsi(const std::vector<double>& prices, int period) {
    if (prices.size() <= period) return -1;

    double gain = 0.0, loss = 0.0;

    for (int i = prices.size() - period; i < static_cast<int>(prices.size()) - 1; ++i) {
        double change = prices[i + 1] - prices[i];
        if (change > 0) gain += change;
        else loss -= change;
    }

    if (gain + loss == 0) return 50.0;

    double rs = gain / (loss == 0 ? 1 : loss);
    return 100.0 - (100.0 / (1.0 + rs));
}
