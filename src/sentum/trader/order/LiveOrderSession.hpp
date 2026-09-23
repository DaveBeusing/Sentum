#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/api/BinanceUserDataStream.hpp>
#include <sentum/trader/execution/ExchangeMetadataCache.hpp>
#include <sentum/trader/order/ConfirmedPositionLedger.hpp>
#include <sentum/trader/order/OrderManager.hpp>

namespace sentum::order {

class LiveOrderSession {
public:
    using UpdateHandler = OrderManager::UpdateHandler;
    using StreamFactory = std::function<std::unique_ptr<IUserDataStream>(
        std::string, IUserDataStream::Handler)>;

    struct Timing {
        std::chrono::milliseconds supervisor_poll_interval{std::chrono::seconds(5)};
        std::chrono::milliseconds keepalive_interval{std::chrono::minutes(25)};
    };

    static std::unique_ptr<LiveOrderSession> from_environment() {
        const char* key = std::getenv("SENTUM_BINANCE_TESTNET_API_KEY");
        const char* secret = std::getenv("SENTUM_BINANCE_TESTNET_API_SECRET");
        const char* enabled = std::getenv("SENTUM_ENABLE_SPOT_TESTNET");
        if (!enabled || std::string(enabled) != "I_UNDERSTAND_TESTNET_ONLY")
            throw std::runtime_error("Spot Testnet execution requires SENTUM_ENABLE_SPOT_TESTNET=I_UNDERSTAND_TESTNET_ONLY");
        if (!key || !secret) throw std::runtime_error("Binance Spot Testnet credentials are missing");

        auto exchange = std::make_unique<BinanceSpotExecutionClient>(key, secret);
        return from_dependencies(std::move(exchange), default_stream_factory());
    }

    static std::unique_ptr<LiveOrderSession> from_dependencies(
        std::unique_ptr<BinanceSpotExecutionClient> exchange,
        StreamFactory stream_factory) {
        return from_dependencies(std::move(exchange), std::move(stream_factory), Timing{});
    }

    static std::unique_ptr<LiveOrderSession> from_dependencies(
        std::unique_ptr<BinanceSpotExecutionClient> exchange,
        StreamFactory stream_factory,
        Timing timing) {
        if (!exchange) throw std::invalid_argument("Live order session requires an exchange client");
        if (!stream_factory) throw std::invalid_argument("Live order session requires a user stream factory");
        if (timing.supervisor_poll_interval <= std::chrono::milliseconds::zero() ||
            timing.keepalive_interval <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("Live order session timing must be positive");
        }
        return std::unique_ptr<LiveOrderSession>(
            new LiveOrderSession(std::move(exchange), std::move(stream_factory), timing));
    }

    ~LiveOrderSession() { stop(); }

    void set_update_handler(UpdateHandler handler) {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        external_handler_ = std::move(handler);
    }

    void start() {
        if (started_.exchange(true)) return;
        manager_.set_update_handler([this](const Snapshot& update) {
            ledger_.on_order_update(update);
            UpdateHandler handler;
            { std::lock_guard<std::mutex> lock(handler_mutex_); handler = external_handler_; }
            if (handler) handler(update);
        });

        try {
            recover_stream();
            supervisor_running_.store(true);
            supervisor_thread_ = std::thread([this] { supervisor_loop(); });
        } catch (...) {
            started_.store(false);
            ready_.store(false);
            manager_.activate_kill_switch();
            throw;
        }
    }

    void stop() {
        if (!started_.exchange(false)) return;
        manager_.activate_kill_switch();
        ready_.store(false);
        supervisor_running_.store(false);
        if (supervisor_thread_.joinable()) supervisor_thread_.join();

        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (stream_) stream_->stop();
        if (!listen_key_.empty()) {
            try { exchange_->close_listen_key(listen_key_); } catch (...) {}
        }
        stream_.reset();
        listen_key_.clear();
    }

    Snapshot submit(const Request& request) {
        if (!started_.load() || !ready()) throw std::logic_error("Live order session is not reconciled and ready");
        return manager_.submit_market(request);
    }

    Snapshot cancel(const std::string& id) { return manager_.cancel(id); }
    void kill() { manager_.activate_kill_switch(); ready_.store(false); }
    bool killed() const noexcept { return manager_.kill_switch_active(); }
    bool ready() const noexcept { return ready_.load() && !killed(); }
    std::optional<Snapshot> get(const std::string& id) const { return manager_.get(id); }
    std::optional<ConfirmedPosition> confirmed_position(const std::string& symbol) const { return ledger_.get(symbol); }
    nlohmann::json account_snapshot() const { return exchange_->account(); }
    nlohmann::json open_orders_snapshot() const { return exchange_->open_orders(); }
    sentum::execution::ExchangeRules exchange_rules(const std::string& symbol) const { return metadata_cache_.rules(symbol); }

    bool resume_after_reconcile(const std::string& confirmation) {
        if (confirmation != "I_CONFIRM_RECONCILED_TESTNET_RESUME" || !started_.load()) return false;
        try {
            recover_stream();
            manager_.clear_kill_switch_for_reconciled_resume();
            ready_.store(true);
            return true;
        } catch (...) {
            ready_.store(false);
            return false;
        }
    }

private:
    LiveOrderSession(std::unique_ptr<BinanceSpotExecutionClient> exchange,
                     StreamFactory stream_factory,
                     Timing timing)
        : exchange_(std::move(exchange)),
          manager_(*exchange_),
          metadata_cache_(*exchange_),
          stream_factory_(std::move(stream_factory)),
          timing_(timing) {}

    static StreamFactory default_stream_factory() {
        return [](std::string listen_key, IUserDataStream::Handler handler) {
            return std::make_unique<BinanceUserDataStream>(std::move(listen_key), std::move(handler));
        };
    }

    void recover_stream() {
        ready_.store(false);
        manager_.reconcile_startup();

        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (stream_) stream_->stop();
        if (!listen_key_.empty()) {
            try { exchange_->close_listen_key(listen_key_); } catch (...) {}
        }

        listen_key_ = exchange_->create_listen_key();
        stream_ = stream_factory_(listen_key_, [this](const nlohmann::json& event) {
            manager_.on_user_data_event(event);
        });
        if (!stream_) throw std::runtime_error("User Data Stream factory returned no stream");
        stream_->start();
        if (!stream_->running()) throw std::runtime_error("User Data Stream did not enter running state");

        last_keepalive_ = std::chrono::steady_clock::now();
        ready_.store(!manager_.kill_switch_active());
    }

    void fail_closed_and_recover() noexcept {
        ready_.store(false);
        manager_.activate_kill_switch();
        try { recover_stream(); } catch (...) {}
    }

    void supervisor_loop() {
        while (supervisor_running_.load()) {
            std::this_thread::sleep_for(timing_.supervisor_poll_interval);
            if (!supervisor_running_.load()) break;

            bool recovery_required = false;
            {
                std::lock_guard<std::mutex> lock(stream_mutex_);
                if (!stream_ || !stream_->running()) {
                    recovery_required = true;
                } else {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_keepalive_ >= timing_.keepalive_interval) {
                        try {
                            exchange_->keepalive_listen_key(listen_key_);
                            last_keepalive_ = now;
                        } catch (...) {
                            recovery_required = true;
                        }
                    }
                }
            }

            if (recovery_required) fail_closed_and_recover();
        }
    }

    std::unique_ptr<BinanceSpotExecutionClient> exchange_;
    OrderManager manager_;
    ConfirmedPositionLedger ledger_;
    sentum::execution::ExchangeMetadataCache metadata_cache_;
    StreamFactory stream_factory_;
    Timing timing_;
    std::unique_ptr<IUserDataStream> stream_;
    std::string listen_key_;
    mutable std::mutex handler_mutex_;
    std::mutex stream_mutex_;
    UpdateHandler external_handler_;
    std::atomic<bool> started_{false}, ready_{false}, supervisor_running_{false};
    std::thread supervisor_thread_;
    std::chrono::steady_clock::time_point last_keepalive_{};
};

} // namespace sentum::order
