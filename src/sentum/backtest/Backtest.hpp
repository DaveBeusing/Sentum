#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sentum/market/MarketEvent.hpp>
#include <sentum/trader/types/TradePosition.hpp>

struct BacktestMetrics {
    double net_profit = 0.0;
    double max_drawdown = 0.0;
    double profit_factor = 0.0;
    double win_rate = 0.0;
    double expectancy = 0.0;
    double sharpe = 0.0;
    double sortino = 0.0;
    double fee_share = 0.0;
    double slippage_sensitivity = 0.0;
    std::size_t trades = 0;
};

class HistoricalEventReader {
public:
    static std::vector<MarketEvent> read_csv(const std::string& path, const std::string& symbol) {
        std::ifstream file(path);
        if (!file) throw std::runtime_error("Cannot open replay file: " + path);
        std::vector<MarketEvent> events;
        std::string line;
        bool first = true;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            if (first && line.find("timestamp") != std::string::npos) { first = false; continue; }
            first = false;
            std::stringstream ss(line);
            std::string field;
            std::vector<std::string> cols;
            while (std::getline(ss, field, ',')) cols.push_back(field);
            if (cols.size() < 2) throw std::runtime_error("Replay CSV requires timestamp_ms,price[,volume]");
            MarketEvent e;
            e.type = MarketEvent::Type::Trade;
            e.symbol = symbol;
            e.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(std::stoll(cols[0])));
            e.price = std::stod(cols[1]);
            e.close = e.price;
            e.volume = cols.size() > 2 ? std::stod(cols[2]) : 0.0;
            events.push_back(e);
        }
        std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.timestamp < b.timestamp; });
        return events;
    }
};

class MetricsCalculator {
public:
    static BacktestMetrics calculate(const std::vector<TradePosition>& trades, double slippage_delta_profit = 0.0) {
        BacktestMetrics m;
        m.trades = trades.size();
        if (trades.empty()) return m;
        double equity = 0.0, peak = 0.0, gross_win = 0.0, gross_loss = 0.0, fees = 0.0;
        std::size_t wins = 0;
        std::vector<double> returns;
        for (const auto& t : trades) {
            equity += t.net_profit;
            peak = std::max(peak, equity);
            m.max_drawdown = std::max(m.max_drawdown, peak - equity);
            if (t.net_profit > 0.0) { gross_win += t.net_profit; ++wins; }
            else gross_loss += -t.net_profit;
            fees += t.fee_entry + t.fee_exit;
            returns.push_back(t.net_profit / std::max(1.0, t.entry_price * t.quantity));
        }
        m.net_profit = equity;
        m.profit_factor = gross_loss > 0.0 ? gross_win / gross_loss : std::numeric_limits<double>::infinity();
        m.win_rate = 100.0 * static_cast<double>(wins) / trades.size();
        m.expectancy = equity / trades.size();
        m.fee_share = (std::abs(equity) + fees) > 0.0 ? fees / (std::abs(equity) + fees) : 0.0;
        const double mean = mean_of(returns);
        const double sd = deviation(returns, mean, false);
        const double downside = deviation(returns, 0.0, true);
        m.sharpe = sd > 0.0 ? mean / sd * std::sqrt(static_cast<double>(returns.size())) : 0.0;
        m.sortino = downside > 0.0 ? mean / downside * std::sqrt(static_cast<double>(returns.size())) : 0.0;
        m.slippage_sensitivity = slippage_delta_profit;
        return m;
    }
private:
    static double mean_of(const std::vector<double>& xs) {
        double sum = 0.0; for (double x : xs) sum += x; return xs.empty() ? 0.0 : sum / xs.size();
    }
    static double deviation(const std::vector<double>& xs, double center, bool downside_only) {
        double sum = 0.0; std::size_t n = 0;
        for (double x : xs) { if (downside_only && x >= 0.0) continue; const double d = x - center; sum += d*d; ++n; }
        return n > 1 ? std::sqrt(sum / static_cast<double>(n - 1)) : 0.0;
    }
};
