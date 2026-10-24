#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/api/BinanceUserDataStream.hpp>
#include <sentum/trader/execution/ExchangeMetadataCache.hpp>
#include <sentum/trader/order/ConfirmedPositionLedger.hpp>
#include <sentum/trader/order/OrderManager.hpp>
#include <sentum/trader/order/OrderEventRepository.hpp>

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

    struct Persistence {
        std::string database_path;
        std::string credential_namespace;
        bool ephemeral_fixture = false;

        static Persistence durable(std::string path, std::string identity) {
            if (path.empty() || identity.empty())
                throw std::invalid_argument("Durable session requires a database and credential identity");
            return {std::move(path), std::move(identity), false};
        }
        static Persistence ephemeral() { return {{}, {}, true}; }
    };

    static std::unique_ptr<LiveOrderSession> from_environment() {
        const char* key = std::getenv("SENTUM_BINANCE_TESTNET_API_KEY");
        const char* secret = std::getenv("SENTUM_BINANCE_TESTNET_API_SECRET");
        const char* enabled = std::getenv("SENTUM_ENABLE_SPOT_TESTNET");
        if (!enabled || std::string(enabled) != "I_UNDERSTAND_TESTNET_ONLY")
            throw std::runtime_error("Spot Testnet execution requires SENTUM_ENABLE_SPOT_TESTNET=I_UNDERSTAND_TESTNET_ONLY");
        if (!key || !secret) throw std::runtime_error("Binance Spot Testnet credentials are missing");

        std::ifstream configuration("config/config.json");
        if (!configuration) throw std::runtime_error("Testnet runtime configuration is unavailable");
        nlohmann::json config;
        configuration >> config;
        if (!config.is_object()) throw std::runtime_error("Testnet runtime configuration must be an object");
        const auto database_path = config.value("databasePath", std::string("log/sentum.sqlite3"));
        if (database_path.empty()) throw std::runtime_error("Testnet runtime database path is empty");
        const auto parent = std::filesystem::path(database_path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        auto persistence = Persistence::durable(database_path, OrderEventRepository::account_fingerprint(key));
        auto exchange = std::make_unique<BinanceSpotExecutionClient>(key, secret);
        return from_dependencies(std::move(exchange), default_stream_factory(), std::move(persistence));
    }

    static std::unique_ptr<LiveOrderSession> from_dependencies(
        std::unique_ptr<BinanceSpotExecutionClient> exchange,
        StreamFactory stream_factory,
        Persistence persistence) {
        return from_dependencies(std::move(exchange), std::move(stream_factory), Timing{}, std::move(persistence));
    }

    static std::unique_ptr<LiveOrderSession> from_dependencies(
        std::unique_ptr<BinanceSpotExecutionClient> exchange,
        StreamFactory stream_factory,
        Timing timing,
        Persistence persistence) {
        if (!exchange) throw std::invalid_argument("Live order session requires an exchange client");
        if (!stream_factory) throw std::invalid_argument("Live order session requires a user stream factory");
        if (!persistence.ephemeral_fixture && (persistence.database_path.empty() || persistence.credential_namespace.empty()))
            throw std::invalid_argument("Live order session requires explicit persistence");
        if (persistence.ephemeral_fixture && (!persistence.database_path.empty() || !persistence.credential_namespace.empty()))
            throw std::invalid_argument("Ephemeral fixtures cannot override durable session identity");
        if (timing.supervisor_poll_interval <= std::chrono::milliseconds::zero() ||
            timing.keepalive_interval <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("Live order session timing must be positive");
        }
        return std::unique_ptr<LiveOrderSession>(
            new LiveOrderSession(std::move(exchange), std::move(stream_factory), timing, std::move(persistence)));
    }

    ~LiveOrderSession() { stop(); }

    void set_update_handler(UpdateHandler handler) {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        external_handler_ = std::move(handler);
    }

    void start() {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (stopped_permanently_) throw std::logic_error("Stopped execution sessions cannot be restarted");
        if (started_.exchange(true)) return;
        manager_.set_update_handler([this](const Snapshot& update) {
            try {
                if ((!update.locally_owned || update.requires_reconciliation) && !recovery_blocked_.exchange(true)) {
                    ready_.store(false);
                    manager_.activate_kill_switch("Unowned or incomplete execution evidence requires full reconciliation");
                }
                ledger_.on_order_update(update);
                UpdateHandler handler;
                { std::lock_guard<std::mutex> lock(handler_mutex_); handler = external_handler_; }
                if (handler) handler(update);
            } catch (...) {
                ready_.store(false);
                if (!handler_failed_.exchange(true)) {
                    try { manager_.activate_kill_switch("Execution observer failed"); } catch (...) {}
                }
                throw;
            }
        });

        try {
            recover_stream();
            supervisor_running_.store(true);
            supervisor_thread_ = std::thread([this] { supervisor_loop(); });
        } catch (...) {
            started_.store(false);
            ready_.store(false);
            try { manager_.activate_kill_switch("Execution session startup failed"); } catch (...) {}
            throw;
        }
    }

    void stop() noexcept {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        stopped_permanently_ = true;
        if (!started_.exchange(false)) return;
        ready_.store(false);
        supervisor_running_.store(false);
        try { manager_.activate_kill_switch("Execution session stopping"); } catch (...) {}
        if (supervisor_thread_.joinable()) supervisor_thread_.join();

        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (stream_) { try { stream_->stop(); } catch (...) {} }
        if (!listen_key_.empty()) {
            try { exchange_->close_listen_key(listen_key_); } catch (...) {}
        }
        stream_.reset();
        listen_key_.clear();
        if (repository_) { try { repository_->finish_session(); } catch (...) {} }
    }

    Snapshot submit(const Request& request) {
        if (!started_.load() || !ready()) throw std::logic_error("Live order session is not reconciled and ready");
        return manager_.submit_market(request);
    }

    Snapshot cancel(const std::string& id) { return manager_.cancel(id); }
    void kill() noexcept {
        ready_.store(false);
        try { manager_.activate_kill_switch("Operator requested execution kill"); } catch (...) {}
    }
    bool killed() const noexcept { return manager_.kill_switch_active(); }
    bool ready() const noexcept { return ready_.load() && !killed() && !recovery_blocked_.load() && !handler_failed_.load(); }
    std::optional<Snapshot> get(const std::string& id) const { return manager_.get(id); }
    std::optional<ConfirmedPosition> confirmed_position(const std::string& symbol) const { return ledger_.get(symbol); }
    nlohmann::json account_snapshot() const { return exchange_->account(); }
    nlohmann::json open_orders_snapshot() const { return exchange_->open_orders(); }
    sentum::execution::ExchangeRules exchange_rules(const std::string& symbol) const { return metadata_cache_.rules(symbol); }

    bool resume_after_reconcile(const std::string& confirmation) {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (confirmation != "I_CONFIRM_RECONCILED_TESTNET_RESUME" || !started_.load() ||
            recovery_blocked_.load() || handler_failed_.load()) return false;
        try {
            std::lock_guard<std::mutex> recovery(recovery_mutex_);
            const auto expected_generation = manager_.kill_generation();
            recover_stream_locked();
            if (recovery_blocked_.load() || handler_failed_.load()) return false;
            manager_.clear_kill_switch_for_reconciled_resume(expected_generation);
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
                     Timing timing,
                     Persistence persistence)
        : exchange_(std::move(exchange)),
          repository_(persistence.ephemeral_fixture ? nullptr :
              std::make_unique<OrderEventRepository>(persistence.database_path, persistence.credential_namespace)),
          manager_(*exchange_, repository_.get()),
          metadata_cache_(*exchange_),
          stream_factory_(std::move(stream_factory)),
          timing_(timing) {}

    static StreamFactory default_stream_factory() {
        return [](std::string listen_key, IUserDataStream::Handler handler) {
            return std::make_unique<BinanceUserDataStream>(std::move(listen_key), std::move(handler));
        };
    }

    void recover_stream() {
        std::lock_guard<std::mutex> recovery(recovery_mutex_);
        recover_stream_locked();
    }

    void recover_stream_locked() {
        ready_.store(false);
        std::set<std::string> recovered_symbols;
        if (repository_) {
            if (repository_->has_unbound_legacy_history()) recovery_blocked_.store(true);
            for (const auto& snapshot : repository_->load_accepted_history()) {
                if (!snapshot.locally_owned || snapshot.requires_reconciliation) recovery_blocked_.store(true);
                ledger_.on_order_update(snapshot);
                if (snapshot.locally_owned) recovered_symbols.insert(snapshot.symbol);
            }
        }
        manager_.reconcile_startup();
        if (repository_) {
            for (const auto& snapshot : repository_->load_latest()) {
                if (snapshot.requires_reconciliation) recovery_blocked_.store(true);
                if (snapshot.locally_owned) recovered_symbols.insert(snapshot.symbol);
            }
            for (const auto& symbol : recovered_symbols) {
                const auto position = ledger_.get(symbol);
                if (position && position->quantity > 0.0) recovery_blocked_.store(true);
            }
        }
        if (recovery_blocked_.load())
            manager_.activate_kill_switch("Recovered inventory or legacy history requires full account and strategy reconciliation");

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
        ready_.store(!manager_.kill_switch_active() && !recovery_blocked_.load() && !handler_failed_.load());
    }

    void fail_closed_and_recover() noexcept {
        ready_.store(false);
        try { manager_.activate_kill_switch("User stream recovery required"); } catch (...) {}
        try { recover_stream(); } catch (...) {}
    }

    void supervisor_loop() {
        while (supervisor_running_.load()) {
            std::this_thread::sleep_for(timing_.supervisor_poll_interval);
            if (!supervisor_running_.load()) break;

            if (manager_.kill_switch_active()) {
                try {
                    std::lock_guard<std::mutex> recovery(recovery_mutex_);
                    manager_.reconcile_unresolved();
                } catch (...) {
                    ready_.store(false);
                    try { manager_.activate_kill_switch("Unresolved order reconciliation failed"); } catch (...) {}
                }
            }

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
    std::unique_ptr<OrderEventRepository> repository_;
    OrderManager manager_;
    ConfirmedPositionLedger ledger_;
    sentum::execution::ExchangeMetadataCache metadata_cache_;
    StreamFactory stream_factory_;
    Timing timing_;
    std::unique_ptr<IUserDataStream> stream_;
    std::string listen_key_;
    mutable std::mutex handler_mutex_;
    std::mutex stream_mutex_;
    std::mutex lifecycle_mutex_;
    std::mutex recovery_mutex_;
    UpdateHandler external_handler_;
    std::atomic<bool> started_{false}, ready_{false}, supervisor_running_{false};
    std::atomic<bool> recovery_blocked_{false}, handler_failed_{false};
    bool stopped_permanently_ = false;
    std::thread supervisor_thread_;
    std::chrono::steady_clock::time_point last_keepalive_{};
};

} // namespace sentum::order
