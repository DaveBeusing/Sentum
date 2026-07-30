#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/api/BinanceUserDataStream.hpp>
#include <sentum/trader/order/ConfirmedPositionLedger.hpp>
#include <sentum/trader/order/OrderManager.hpp>

namespace sentum::order {

class LiveOrderSession {
public:
    using UpdateHandler = OrderManager::UpdateHandler;

    static std::unique_ptr<LiveOrderSession> from_environment() {
        const char* key = std::getenv("SENTUM_BINANCE_TESTNET_API_KEY");
        const char* secret = std::getenv("SENTUM_BINANCE_TESTNET_API_SECRET");
        const char* enabled = std::getenv("SENTUM_ENABLE_SPOT_TESTNET");
        if (!enabled || std::string(enabled) != "I_UNDERSTAND_TESTNET_ONLY") {
            throw std::runtime_error("Spot Testnet execution requires SENTUM_ENABLE_SPOT_TESTNET=I_UNDERSTAND_TESTNET_ONLY");
        }
        if (!key || !secret) throw std::runtime_error("Binance Spot Testnet credentials are missing");
        return std::unique_ptr<LiveOrderSession>(new LiveOrderSession(key, secret));
    }

    ~LiveOrderSession() { stop(); }

    void set_update_handler(UpdateHandler handler) {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        external_handler_ = std::move(handler);
    }

    void start() {
        if (started_.exchange(true)) return;
        try {
            manager_.set_update_handler([this](const Snapshot& update) {
                ledger_.on_order_update(update);
                UpdateHandler copy;
                {
                    std::lock_guard<std::mutex> lock(handler_mutex_);
                    copy = external_handler_;
                }
                if (copy) copy(update);
            });
            manager_.reconcile_startup();
            listen_key_ = exchange_.create_listen_key();
            stream_ = std::make_unique<BinanceUserDataStream>(listen_key_,
                [this](const nlohmann::json& event) { manager_.on_user_data_event(event); });
            stream_->start();
            keepalive_running_.store(true);
            keepalive_thread_ = std::thread([this] {
                while (keepalive_running_.load()) {
                    for (int i = 0; i < 1800 && keepalive_running_.load(); ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (!keepalive_running_.load()) break;
                    try { exchange_.keepalive_listen_key(listen_key_); }
                    catch (...) { manager_.activate_kill_switch(); break; }
                }
            });
        } catch (...) {
            started_.store(false);
            throw;
        }
    }

    void stop() {
        if (!started_.exchange(false)) return;
        manager_.activate_kill_switch();
        keepalive_running_.store(false);
        if (keepalive_thread_.joinable()) keepalive_thread_.join();
        if (stream_) stream_->stop();
        if (!listen_key_.empty()) {
            try { exchange_.close_listen_key(listen_key_); } catch (...) {}
        }
    }

    Snapshot submit(const Request& request) {
        if (!started_.load()) throw std::logic_error("Live order session is not started");
        return manager_.submit_market(request);
    }

    Snapshot cancel(const std::string& client_order_id) { return manager_.cancel(client_order_id); }
    void kill() { manager_.activate_kill_switch(); }
    bool killed() const noexcept { return manager_.kill_switch_active(); }
    std::optional<Snapshot> get(const std::string& client_order_id) const { return manager_.get(client_order_id); }
    std::optional<ConfirmedPosition> confirmed_position(const std::string& symbol) const { return ledger_.get(symbol); }

private:
    LiveOrderSession(std::string key, std::string secret)
        : exchange_(std::move(key), std::move(secret)), manager_(exchange_) {}

    BinanceSpotExecutionClient exchange_;
    OrderManager manager_;
    ConfirmedPositionLedger ledger_;
    std::unique_ptr<BinanceUserDataStream> stream_;
    std::string listen_key_;
    mutable std::mutex handler_mutex_;
    UpdateHandler external_handler_;
    std::atomic<bool> started_{false};
    std::atomic<bool> keepalive_running_{false};
    std::thread keepalive_thread_;
};

} // namespace sentum::order
