#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
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
#include <sentum/trader/execution/AccountReconciler.hpp>
#include <sentum/trader/execution/ExchangeMetadataCache.hpp>
#include <sentum/trader/execution/ExecutionRecoveryState.hpp>
#include <sentum/trader/order/ConfirmedPositionLedger.hpp>
#include <sentum/trader/order/OrderManager.hpp>
#include <sentum/trader/order/OrderEventRepository.hpp>
#include <sentum/trader/utils/RiskConfigLoader.hpp>
#include <sentum/trader/utils/StrictNumeric.hpp>

namespace sentum::order {

class LiveOrderSession {
public:
    using UpdateHandler = OrderManager::UpdateHandler;
    using RecoveryHandler = std::function<void(const execution::ExecutionRecoveryState&)>;
    using HealthHandler = std::function<void(bool)>;
    using StreamFactory = std::function<std::unique_ptr<IUserDataStream>(
        IUserDataStream::SubscriptionRequest, IUserDataStream::Handler, IUserDataStream::HealthHandler)>;

    struct Timing {
        std::chrono::milliseconds supervisor_poll_interval{std::chrono::seconds(5)};
        std::chrono::milliseconds subscription_timeout{std::chrono::seconds(20)};
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
        return from_dependencies(std::make_unique<BinanceSpotExecutionClient>(key, secret),
                                 default_stream_factory(), std::move(persistence));
    }

    static std::unique_ptr<LiveOrderSession> from_dependencies(
        std::unique_ptr<BinanceSpotExecutionClient> exchange, StreamFactory stream_factory, Persistence persistence) {
        return from_dependencies(std::move(exchange), std::move(stream_factory), Timing{}, std::move(persistence));
    }
    static std::unique_ptr<LiveOrderSession> from_dependencies(
        std::unique_ptr<BinanceSpotExecutionClient> exchange, StreamFactory stream_factory,
        Timing timing, Persistence persistence) {
        if (!exchange || !stream_factory) throw std::invalid_argument("Execution client and user stream factory are required");
        if (!persistence.ephemeral_fixture && (persistence.database_path.empty() || persistence.credential_namespace.empty()))
            throw std::invalid_argument("Live order session requires explicit persistence");
        if (persistence.ephemeral_fixture && (!persistence.database_path.empty() || !persistence.credential_namespace.empty()))
            throw std::invalid_argument("Ephemeral fixtures cannot override durable session identity");
        if (timing.supervisor_poll_interval <= std::chrono::milliseconds::zero() ||
            timing.subscription_timeout <= std::chrono::milliseconds::zero())
            throw std::invalid_argument("Live order session timing must be positive");
        return std::unique_ptr<LiveOrderSession>(new LiveOrderSession(
            std::move(exchange), std::move(stream_factory), timing, std::move(persistence)));
    }

    ~LiveOrderSession() { stop(); }

    void configure_market(std::string symbol, RiskConfig risk) {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (started_.load()) throw std::logic_error("Cannot change an active execution market");
        (void)api::market_trade_url(BinanceSpotExecutionClient::environment(), symbol);
        validate_risk_config(risk);
        std::lock_guard<std::mutex> policy(policy_mutex_);
        symbol_ = std::move(symbol);
        risk_ = risk;
    }
    void set_update_handler(UpdateHandler handler) {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        external_handler_ = std::move(handler);
    }
    void set_recovery_handler(RecoveryHandler handler) {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        recovery_handler_ = std::move(handler);
    }
    void set_health_handler(HealthHandler handler) {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        health_handler_ = std::move(handler);
    }

    void start() {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (stopped_permanently_) throw std::logic_error("Stopped execution sessions cannot be restarted");
        if (symbol_.empty()) throw std::logic_error("Configure the execution market before startup");
        if (started_.exchange(true)) return;
        manager_.set_admission_guard([this] {
            return ready_.load() && user_healthy_.load() && reconciled_account_.load() && !recovery_blocked_.load() &&
                processed_event_generation_.load() == event_generation_.load();
        });
        manager_.set_update_handler([this](const Snapshot& update) {
            std::lock_guard<std::recursive_mutex> gate(state_gate_);
            try {
                if (!update.locally_owned || update.symbol != symbol_ || update.requires_reconciliation) {
                    ready_.store(false);
                    reconciled_account_.store(false);
                    if (!recovery_blocked_.exchange(true))
                        manager_.block_submissions("Unowned or incomplete execution evidence requires full reconciliation");
                }
                if (recovering_.load()) return;
                ledger_.on_order_update(update);
                UpdateHandler handler;
                { std::lock_guard<std::mutex> lock(handler_mutex_); handler = external_handler_; }
                if (handler) handler(update);
            } catch (...) {
                ready_.store(false);
                if (!handler_failed_.exchange(true)) manager_.block_submissions("Execution observer failed");
                throw;
            }
        });
        try {
            std::lock_guard<std::mutex> recovery(recovery_mutex_);
            recover_stream_locked();
            supervisor_running_.store(true);
            supervisor_thread_ = std::thread([this] { supervisor_loop(); });
        } catch (...) {
            started_.store(false);
            ready_.store(false);
            manager_.block_submissions("Execution session startup failed");
            stop_stream();
            throw;
        }
    }

    void stop() noexcept {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        stopped_permanently_ = true;
        const bool was_started = started_.exchange(false);
        ready_.store(false);
        { std::lock_guard<std::mutex> lock(supervisor_mutex_); supervisor_running_.store(false); }
        supervisor_cv_.notify_all();
        if (was_started) manager_.activate_kill_switch("Execution session stopping");
        if (supervisor_thread_.joinable()) supervisor_thread_.join();
        stop_stream();
        if (was_started && repository_) { try { repository_->finish_session(); } catch (...) {} }
    }

    Snapshot submit(const Request& request) {
        if (!started_.load() || !ready()) throw std::logic_error("Live order session is not reconciled and ready");
        if (request.symbol != symbol_) throw std::invalid_argument("Order does not belong to the configured Testnet market");
        const auto generation = stream_generation_.load();
        const auto began = std::chrono::steady_clock::now();
        try {
            const auto rules = metadata_cache_.rules(symbol_, true);
            std::set<std::string> permissions;
            { std::lock_guard<std::recursive_mutex> gate(state_gate_); permissions = account_permissions_; }
            rules.validate_permissions(permissions);
            auto normalized = rules.normalize_market_quantity(request.quantity, risk_.step_size);
            if (!std::isfinite(normalized.quantity) || normalized.quantity <= 0.0 || normalized.quantity > request.quantity)
                throw std::invalid_argument("Exchange normalization exceeded approved size");
            const double price = ticker_price();
            validate_local_size(normalized.quantity, price, request.side);
            validate_notional(rules, normalized.quantity);
            if (std::chrono::steady_clock::now() - began > std::chrono::milliseconds(risk_.max_data_age_ms))
                throw std::runtime_error("Submission market evidence became stale during validation");
            Request approved = request;
            approved.quantity = normalized.quantity;
            approved.quantity_text = std::move(normalized.text);
            approved.base_asset = rules.base_asset;
            approved.quote_asset = rules.quote_asset;
            std::lock_guard<std::recursive_mutex> gate(state_gate_);
            if (generation != stream_generation_.load() || !ready())
                throw std::logic_error("Execution state changed during order validation");
            if (request.side == Side::Sell) {
                const auto position = ledger_.get(symbol_);
                if (!position || approved.quantity > position->quantity)
                    throw std::invalid_argument("Testnet sale exceeds confirmed owned inventory");
            }
            const auto result = manager_.submit_market(approved);
            if (result.state == State::Rejected) metadata_cache_.invalidate(symbol_);
            return result;
        } catch (...) {
            metadata_cache_.invalidate(symbol_);
            throw;
        }
    }

    Snapshot cancel(const std::string& id) { return manager_.cancel(id); }
    void kill() noexcept {
        ready_.store(false);
        manager_.activate_kill_switch("Operator requested execution kill");
        notify_health();
    }
    bool killed() const noexcept { return manager_.kill_switch_active(); }
    bool ready() const noexcept {
        return started_.load() && ready_.load() && user_healthy_.load() &&
            reconciled_account_.load() && processed_event_generation_.load() == event_generation_.load() &&
            !killed() && !recovery_blocked_.load() && !handler_failed_.load();
    }
    bool user_stream_healthy() const noexcept { return user_healthy_.load(); }
    bool reconciliation_complete() const noexcept { return reconciled_account_.load(); }
    std::string reconciliation_details() const {
        std::lock_guard<std::recursive_mutex> gate(state_gate_);
        return reconciliation_details_;
    }
    std::optional<Snapshot> get(const std::string& id) const { return manager_.get(id); }
    std::optional<ConfirmedPosition> confirmed_position(const std::string& symbol) const { return ledger_.get(symbol); }
    std::optional<nlohmann::json> load_position_policy(const std::string& symbol) const {
        // Runtime callbacks already hold their position mutex. This storage path
        // never takes the session state gate or invokes a runtime observer.
        std::lock_guard<std::mutex> policy(policy_mutex_);
        if (symbol.empty() || symbol != symbol_)
            throw std::invalid_argument("Position policy does not belong to the configured Testnet market");
        if (repository_) return repository_->load_position_policy(symbol);
        const auto found = ephemeral_position_policies_.find(symbol);
        return found == ephemeral_position_policies_.end() ? std::nullopt
            : std::optional<nlohmann::json>(found->second);
    }
    void save_position_policy(const std::string& symbol, const nlohmann::json& payload) {
        std::lock_guard<std::mutex> policy(policy_mutex_);
        if (symbol.empty() || symbol != symbol_)
            throw std::invalid_argument("Position policy does not belong to the configured Testnet market");
        if (repository_) repository_->save_position_policy(symbol, payload);
        else {
            const auto canonical = risk::PositionPolicyCheckpoint::from_json(payload).to_json();
            if (canonical.at("symbol") != symbol)
                throw std::invalid_argument("Position-policy symbol does not match its storage key");
            ephemeral_position_policies_.insert_or_assign(symbol, canonical);
        }
    }
    nlohmann::json account_snapshot() const { return exchange_->account(); }
    nlohmann::json open_orders_snapshot() const { return exchange_->open_orders(); }
    execution::ExchangeRules exchange_rules(const std::string& symbol) const { return metadata_cache_.rules(symbol); }

    bool resume_after_reconcile(const std::string& confirmation) {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (confirmation != "I_CONFIRM_RECONCILED_TESTNET_RESUME" || !started_.load() || handler_failed_.load()) return false;
        try {
            std::lock_guard<std::mutex> recovery(recovery_mutex_);
            const auto expected_generation = manager_.kill_generation();
            recover_stream_locked();
            std::lock_guard<std::recursive_mutex> gate(state_gate_);
            if (!reconciled_account_.load() || recovery_blocked_.load() || handler_failed_.load() || !user_healthy_.load()) return false;
            manager_.clear_kill_switch_for_reconciled_resume(expected_generation);
            ready_.store(true);
            notify_health();
            return ready();
        } catch (...) {
            ready_.store(false);
            return false;
        }
    }

private:
    LiveOrderSession(std::unique_ptr<BinanceSpotExecutionClient> exchange, StreamFactory stream_factory,
                     Timing timing, Persistence persistence)
        : exchange_(std::move(exchange)),
          repository_(persistence.ephemeral_fixture ? nullptr :
              std::make_unique<OrderEventRepository>(persistence.database_path, persistence.credential_namespace)),
          manager_(*exchange_, repository_.get()), metadata_cache_(*exchange_),
          stream_factory_(std::move(stream_factory)), timing_(timing) {}

    static StreamFactory default_stream_factory() {
        return [](IUserDataStream::SubscriptionRequest signing, IUserDataStream::Handler handler,
                  IUserDataStream::HealthHandler health) {
            return std::make_unique<BinanceUserDataStream>(std::move(signing), std::move(handler), std::move(health));
        };
    }
    void notify_health() noexcept {
        try {
            HealthHandler handler;
            { std::lock_guard<std::mutex> lock(handler_mutex_); handler = health_handler_; }
            if (handler) handler(user_healthy_.load());
        } catch (...) {
            ready_.store(false);
            handler_failed_.store(true);
            manager_.block_submissions("Execution health observer failed");
        }
    }
    void stop_stream() noexcept {
        ++stream_generation_;
        user_healthy_.store(false);
        std::unique_ptr<IUserDataStream> previous;
        { std::lock_guard<std::mutex> lock(stream_mutex_); previous = std::move(stream_); }
        if (previous) { try { previous->stop(); } catch (...) {} }
    }
    void on_stream_health(std::uint64_t generation, bool healthy) noexcept {
        if (generation != stream_generation_.load()) return;
        user_healthy_.store(healthy);
        if (!healthy && started_.load()) {
            ready_.store(false);
            reconciled_account_.store(false);
            manager_.block_submissions("Authenticated user-data subscription is unavailable");
            recovery_requested_.store(true);
            supervisor_cv_.notify_all();
        }
        notify_health();
    }
    void on_stream_event(std::uint64_t generation, const nlohmann::json& event) {
        if (generation != stream_generation_.load()) return;
        const auto observed_event = ++event_generation_;
        std::lock_guard<std::recursive_mutex> gate(state_gate_);
        if (generation != stream_generation_.load()) return;
        manager_.on_user_data_event(event);
        const auto type = event.value("e", std::string{});
        if (type == "balanceUpdate") {
            ready_.store(false);
            reconciled_account_.store(false);
            recovery_blocked_.store(true);
            manager_.block_submissions("Account balance transfer requires reconciliation");
        } else if (type == "outboundAccountPosition") {
            // Account-wide balance reports never create strategy ownership.
            try {
                if (!event.at("B").is_array()) throw std::runtime_error("Malformed account balance event");
                const auto rules = metadata_cache_.rules(symbol_);
                std::set<std::string> assets;
                for (const auto& balance : event.at("B")) {
                    const auto asset = balance.at("a").get<std::string>();
                    if (!assets.insert(asset).second) throw std::runtime_error("Duplicate account asset event");
                    const auto free = number(balance.at("f"));
                    const auto locked = number(balance.at("l"));
                    if (asset != rules.base_asset) continue;
                    const auto position = ledger_.get(symbol_);
                    const auto owned = position ? position->quantity : 0.0;
                    if (!std::isfinite(free + locked) || std::abs(free + locked - owned) >
                        64 * std::numeric_limits<double>::epsilon() * std::max({free + locked, owned, 1e-12}))
                        throw std::runtime_error("Account balance differs from owned inventory");
                }
            } catch (...) {
                ready_.store(false); recovery_blocked_.store(true);
                reconciled_account_.store(false);
                manager_.block_submissions("Account balance event requires reconciliation");
            }
        }
        processed_event_generation_.store(observed_event);
    }

    void recover_stream_locked() {
        struct RecoveryScope {
            std::atomic<bool>& active;
            explicit RecoveryScope(std::atomic<bool>& flag) : active(flag) { active.store(true); }
            ~RecoveryScope() { active.store(false); }
        } recovery_scope(recovering_);
        ready_.store(false);
        reconciled_account_.store(false);
        manager_.restore_persisted_state();
        stop_stream();
        // The old worker has joined. Any callback discarded because its
        // generation expired is covered by the new full account proof.
        processed_event_generation_.store(event_generation_.load());
        const auto generation = ++stream_generation_;
        recovery_requested_.store(false);
        auto replacement = stream_factory_(
            [this](const std::string& id) { return exchange_->user_data_subscription(id); },
            [this, generation](const nlohmann::json& event) { on_stream_event(generation, event); },
            [this, generation](bool healthy) { on_stream_health(generation, healthy); });
        if (!replacement) throw std::runtime_error("User data stream factory returned no stream");
        auto* active = replacement.get();
        { std::lock_guard<std::mutex> lock(stream_mutex_); stream_ = std::move(replacement); }
        active->start();
        if (!active->wait_until_healthy(timing_.subscription_timeout) || !active->healthy() || !user_healthy_.load()) {
            stop_stream();
            throw std::runtime_error("Authenticated user-data subscription was not acknowledged in time");
        }
        manager_.reconcile_startup();
        const auto orders = manager_.snapshots();
        if (repository_ && repository_->has_unbound_legacy_history()) {
            recovery_blocked_.store(true);
            manager_.block_submissions("Unbound execution history requires a verified credential migration");
            return;
        }
        for (const auto& order : orders)
            if (order.symbol != symbol_ || !order.locally_owned) {
                recovery_blocked_.store(true);
                manager_.block_submissions("Unrelated account activity requires explicit ownership reconciliation");
                return;
            }
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::uint64_t observed_events;
            std::vector<Snapshot> owned;
            {
                std::lock_guard<std::recursive_mutex> gate(state_gate_);
                observed_events = event_generation_.load();
                if (processed_event_generation_.load() != observed_events) continue;
                owned = manager_.snapshots();
            }
            const auto rules = metadata_cache_.rules(symbol_, true);
            execution::AccountReconciler reconciler(*exchange_);
            const auto recovered = reconciler.recover(symbol_, rules.base_asset, rules.quote_asset, owned);
            std::lock_guard<std::recursive_mutex> gate(state_gate_);
            reconciliation_details_ = recovered.report.details;
            if (generation != stream_generation_.load() || !active->healthy() || !user_healthy_.load())
                throw std::runtime_error("User subscription changed during account reconciliation");
            if (observed_events != event_generation_.load()) continue;
            if (!recovered.report.success) {
                recovery_blocked_.store(true);
                manager_.block_submissions("Account reconciliation failed: " + recovered.report.details);
                notify_health();
                return;
            }
            rules.validate_permissions(recovered.account_permissions);
            ConfirmedPositionLedger verified;
            verified.restore(recovered.replay);
            const nlohmann::json proof{{"format", 1}, {"started_ms", recovered.started_ms},
                {"finished_ms", recovered.finished_ms},
                {"account_uid", recovered.account_uid ? nlohmann::json(*recovered.account_uid) : nlohmann::json(nullptr)},
                {"account_permissions", recovered.account_permissions}, {"details", recovered.report.details}};
            manager_.accept_account_reconciliation(recovered.orders, recovered.replay, symbol_, proof);
            ledger_.restore(recovered.replay);
            account_permissions_ = recovered.account_permissions;
            execution::ExecutionRecoveryState state{symbol_, recovered.orders, recovered.replay,
                recovered.position, recovered.entry_time, recovered.last_exit};
            RecoveryHandler handler;
            { std::lock_guard<std::mutex> lock(handler_mutex_); handler = recovery_handler_; }
            if (handler) handler(state);
            if (observed_events != event_generation_.load() || !user_healthy_.load()) continue;
            recovering_.store(false);
            recovery_blocked_.store(false);
            reconciled_account_.store(true);
            ready_.store(!manager_.kill_switch_active() && !handler_failed_.load());
            notify_health();
            return;
        }
        recovery_blocked_.store(true);
        manager_.block_submissions("Account changed throughout the bounded reconciliation window");
    }

    static double number(const nlohmann::json& value) {
        double parsed;
        if (value.is_string()) {
            const auto text = value.get<std::string>();
            std::istringstream input(text);
            input.imbue(std::locale::classic());
            input >> std::noskipws >> parsed;
            if (!input || !input.eof()) throw std::runtime_error("Malformed exchange numeric evidence");
        } else if (value.is_number()) parsed = value.get<double>();
        else throw std::runtime_error("Exchange numeric evidence has an invalid type");
        if (!std::isfinite(parsed) || parsed < 0.0) throw std::runtime_error("Exchange numeric evidence is not finite and nonnegative");
        return parsed;
    }
    double ticker_price() const {
        const auto ticker = exchange_->ticker_price(symbol_);
        if (ticker.at("symbol").get<std::string>() != symbol_) throw std::runtime_error("Ticker belongs to a different market");
        const auto price = number(ticker.at("price"));
        if (price <= 0.0) throw std::runtime_error("Ticker price must be positive");
        return price;
    }
    void validate_local_size(double quantity, double price, Side side) const {
        const auto notional = quantity * price;
        if (!std::isfinite(notional) || notional <= 0.0 || quantity < risk_.min_quantity ||
            (risk_.max_quantity > 0.0 && quantity > risk_.max_quantity) || notional < risk_.min_notional)
            throw std::invalid_argument("Order violates configured quantity or notional limits");
        const auto steps = quantity / risk_.step_size;
        if (!std::isfinite(steps) || std::abs(steps - std::round(steps)) >
            16 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(steps)))
            throw std::invalid_argument("Exchange quantity does not satisfy the local quantity step");
        if (side == Side::Buy && (notional > risk_.max_total_capital * risk_.leverage ||
            notional * risk_.stop_loss_percent > risk_.max_total_capital * risk_.risk_per_trade))
            throw std::invalid_argument("Order exceeds the approved local capital or stop-risk budget");
    }
    void validate_notional(const execution::ExchangeRules& rules, double quantity) const {
        const auto windows = rules.notional_price_windows();
        if (windows.empty()) return;
        std::optional<double> reference;
        try {
            const auto value = exchange_->reference_price(symbol_);
            if (value.at("symbol").get<std::string>() != symbol_)
                throw std::runtime_error("Reference price belongs to a different market");
            if (!value.at("timestamp").is_number_integer() ||
                (value.at("timestamp").is_number_unsigned() && value.at("timestamp").get<std::uint64_t>() >
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) ||
                value.at("timestamp").get<std::int64_t>() <= 0)
                throw std::runtime_error("Reference price has no valid evidence timestamp");
            if (!value.at("referencePrice").is_null()) {
                reference = number(value.at("referencePrice"));
                if (*reference <= 0.0) throw std::runtime_error("Invalid reference price");
                require_fresh(value.at("timestamp"));
            }
        } catch (const BinanceRequestError& error) {
            if (error.code() != -2043) throw;
        }
        std::map<int, double> prices;
        if (!reference) for (const auto minutes : windows) {
            if (minutes == 0) prices.emplace(0, ticker_price());
            else {
                const auto average = exchange_->average_price(symbol_);
                if (!average.at("mins").is_number_integer() || average.at("mins").get<std::int64_t>() != minutes)
                    throw std::runtime_error("Exchange average price window does not match its notional filter");
                require_fresh(average.at("closeTime"));
                const auto price = number(average.at("price"));
                if (price <= 0.0) throw std::runtime_error("Invalid average market price");
                prices.emplace(minutes, price);
            }
        }
        rules.validate_market_notional(quantity, prices, reference);
    }
    void require_fresh(const nlohmann::json& timestamp) const {
        if (!timestamp.is_number_integer() ||
            (timestamp.is_number_unsigned() && timestamp.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
            throw std::runtime_error("Invalid exchange evidence timestamp");
        const auto milliseconds = timestamp.get<std::int64_t>();
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (milliseconds <= 0 || milliseconds > now + 1000 || now - milliseconds > risk_.max_data_age_ms)
            throw std::runtime_error("Exchange price evidence is stale or from the future");
    }

    void supervisor_loop() noexcept {
        std::unique_lock<std::mutex> wait(supervisor_mutex_);
        while (supervisor_running_.load()) {
            supervisor_cv_.wait_for(wait, timing_.supervisor_poll_interval,
                [this] { return !supervisor_running_.load() || recovery_requested_.load(); });
            if (!supervisor_running_.load()) break;
            wait.unlock();
            try {
                bool unhealthy;
                { std::lock_guard<std::mutex> lock(stream_mutex_); unhealthy = !stream_ || !stream_->healthy(); }
                if (unhealthy || recovery_requested_.exchange(false)) {
                    ready_.store(false);
                    manager_.activate_kill_switch("User stream recovery required");
                    std::lock_guard<std::mutex> recovery(recovery_mutex_);
                    if (supervisor_running_.load()) recover_stream_locked();
                } else if (manager_.kill_switch_active()) {
                    std::lock_guard<std::mutex> recovery(recovery_mutex_);
                    manager_.reconcile_unresolved();
                }
            } catch (...) {
                ready_.store(false);
                recovery_requested_.store(false);
                manager_.block_submissions("Execution recovery remains incomplete");
            }
            wait.lock();
        }
    }

    std::unique_ptr<BinanceSpotExecutionClient> exchange_;
    std::unique_ptr<OrderEventRepository> repository_;
    OrderManager manager_;
    ConfirmedPositionLedger ledger_;
    execution::ExchangeMetadataCache metadata_cache_;
    StreamFactory stream_factory_;
    Timing timing_;
    std::unique_ptr<IUserDataStream> stream_;
    std::string symbol_;
    RiskConfig risk_;
    std::set<std::string> account_permissions_;
    std::string reconciliation_details_;
    mutable std::mutex handler_mutex_;
    mutable std::mutex policy_mutex_;
    std::map<std::string, nlohmann::json> ephemeral_position_policies_;
    mutable std::recursive_mutex state_gate_;
    std::mutex stream_mutex_, lifecycle_mutex_, recovery_mutex_, supervisor_mutex_;
    std::condition_variable supervisor_cv_;
    UpdateHandler external_handler_;
    RecoveryHandler recovery_handler_;
    HealthHandler health_handler_;
    std::atomic<bool> started_{false}, ready_{false}, supervisor_running_{false};
    std::atomic<bool> user_healthy_{false}, reconciled_account_{false}, recovery_requested_{false};
    std::atomic<bool> recovery_blocked_{false}, handler_failed_{false};
    std::atomic<bool> recovering_{false};
    std::atomic<std::uint64_t> stream_generation_{0}, event_generation_{0};
    std::atomic<std::uint64_t> processed_event_generation_{0};
    bool stopped_permanently_ = false;
    std::thread supervisor_thread_;
};

} // namespace sentum::order
