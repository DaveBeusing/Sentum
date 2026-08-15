#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace sentum::paper {

class PaperAccount {
public:
    PaperAccount(std::string state_path, std::string currency, double initial_balance)
        : path_(std::move(state_path)), currency_(std::move(currency)), initial_balance_(initial_balance), equity_(initial_balance) {
        load();
    }

    double equity() const { std::lock_guard<std::mutex> lock(mutex_); return equity_; }
    double initial_balance() const noexcept { return initial_balance_; }
    const std::string& currency() const noexcept { return currency_; }

    void apply_realized_profit(double delta) {
        std::lock_guard<std::mutex> lock(mutex_);
        equity_ += delta;
        realized_profit_ += delta;
        ++closed_trades_;
        save_locked();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        equity_ = initial_balance_;
        realized_profit_ = 0.0;
        closed_trades_ = 0;
        save_locked();
    }

    nlohmann::json snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {{"currency", currency_}, {"initial_balance", initial_balance_}, {"equity", equity_},
                {"realized_profit", realized_profit_}, {"closed_trades", closed_trades_}};
    }

private:
    void load() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ifstream file(path_);
        if (!file) { save_locked(); return; }
        try {
            nlohmann::json json; file >> json;
            if (json.value("currency", currency_) != currency_) return;
            equity_ = json.value("equity", initial_balance_);
            realized_profit_ = json.value("realized_profit", equity_ - initial_balance_);
            closed_trades_ = json.value("closed_trades", 0);
        } catch (...) { equity_ = initial_balance_; realized_profit_ = 0.0; closed_trades_ = 0; }
    }

    void save_locked() const {
        const std::filesystem::path path(path_);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());

        auto tmp = path;
        tmp += ".tmp";

        std::ofstream(tmp, std::ios::trunc) << snapshot_unlocked().dump(2) << '\n';
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(tmp, path, ec);
        }
    }

    nlohmann::json snapshot_unlocked() const {
        return {{"currency", currency_}, {"initial_balance", initial_balance_}, {"equity", equity_},
                {"realized_profit", realized_profit_}, {"closed_trades", closed_trades_}};
    }

    std::string path_, currency_;
    double initial_balance_ = 0.0;
    mutable std::mutex mutex_;
    double equity_ = 0.0;
    double realized_profit_ = 0.0;
    int closed_trades_ = 0;
};

} // namespace sentum::paper
