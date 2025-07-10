#include "../include/sma.hpp"
#include <numeric>

double calculate_sma(const std::vector<double>& prices, int period) {
	//if (prices.size() < period) return 0.0;
	if (prices.size() < static_cast<std::size_t>(period)) return 0.0;
	double sum = std::accumulate(prices.end() - period, prices.end(), 0.0);
	return sum / period;
}
