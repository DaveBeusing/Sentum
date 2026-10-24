#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio/steady_timer.hpp>
#include <nlohmann/json.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/api/TlsClientContext.hpp>
#include <sentum/api/UserDataSubscriptionProtocol.hpp>

class IUserDataStream {
public:
    using SubscriptionRequest = std::function<nlohmann::json(const std::string&)>;
    using Handler = std::function<void(const nlohmann::json&)>;
    using HealthHandler = std::function<void(bool)>;
    virtual ~IUserDataStream() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool running() const noexcept = 0;
    virtual bool healthy() const noexcept = 0;
    virtual bool wait_until_healthy(std::chrono::milliseconds timeout) = 0;
};

class BinanceUserDataStream final : public IUserDataStream {
public:
    BinanceUserDataStream(SubscriptionRequest subscription_request, Handler handler, HealthHandler health_handler = {})
        : subscription_request_(std::move(subscription_request)), handler_(std::move(handler)),
          health_handler_(std::move(health_handler)) {
        if (!subscription_request_) throw std::invalid_argument("User data subscription signing is required");
    }

    ~BinanceUserDataStream() override { stop(); }

    void start() override {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        if (worker_id_ == std::this_thread::get_id()) {
            if (running()) return;
            throw std::logic_error("A user data worker cannot restart itself before it has been joined");
        }
        lifecycle_cv_.wait(lock, [this] { return !joining_; });
        if (running()) return;
        join_worker(lock);
        impl_ = std::make_unique<Impl>();
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            request_id_ = "sentum-user-" + std::to_string(++generation_);
            protocol_.begin(request_id_);
            healthy_.store(false, std::memory_order_release);
            running_.store(true, std::memory_order_release);
        }
        try {
            thread_ = std::thread([this] { run(); });
            worker_id_ = thread_.get_id();
        } catch (...) {
            request_stop();
            impl_.reset();
            throw;
        }
    }

    void stop() override {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        request_stop();
        if (worker_id_ == std::this_thread::get_id()) return;
        lifecycle_cv_.wait(lock, [this] { return !joining_; });
        request_stop();
        join_worker(lock);
        impl_.reset();
    }

    bool running() const noexcept override { return running_.load(std::memory_order_acquire); }
    bool healthy() const noexcept override { return healthy_.load(std::memory_order_acquire); }

    bool wait_until_healthy(std::chrono::milliseconds timeout) override {
        std::unique_lock<std::mutex> lock(state_mutex_);
        state_cv_.wait_for(lock, timeout, [this] { return healthy() || !running(); });
        return running() && healthy();
    }

private:
    friend struct WebsocketLifecycleTestAccess;
    friend struct UserDataStreamTestAccess;
    using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
    using Protocol = sentum::api::UserDataSubscriptionProtocol;

    struct Impl {
        Client client;
        websocketpp::connection_hdl connection;
        std::unique_ptr<boost::asio::steady_timer> deadline;
        std::mutex mutex;
        bool connected = false;

        Impl() {
            client.init_asio();
            client.start_perpetual();
            deadline = std::make_unique<boost::asio::steady_timer>(client.get_io_service());
        }
    };

    void join_worker(std::unique_lock<std::mutex>& lock) {
        if (!thread_.joinable()) return;
        joining_ = true;
        lock.unlock();
        thread_.join();
        lock.lock();
        worker_id_ = {};
        joining_ = false;
        lifecycle_cv_.notify_all();
    }

    bool clear_health() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        healthy_.store(false, std::memory_order_release);
        protocol_.fail();
        return running_.exchange(false, std::memory_order_acq_rel);
    }

    void stop_transport() {
        if (!impl_) return;
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->connected) {
            websocketpp::lib::error_code ec;
            impl_->client.close(impl_->connection, websocketpp::close::status::going_away, "shutdown", ec);
            impl_->connected = false;
        }
        impl_->client.stop_perpetual();
        impl_->client.stop();
    }

    void request_stop() {
        clear_health();
        state_cv_.notify_all();
        stop_transport();
    }

    void report_health(bool value) noexcept {
        if (!health_handler_) return;
        try { health_handler_(value); }
        catch (...) { /* callbacks cannot escape the transport worker */ }
    }

    void fail_stream() {
        const bool notify = clear_health();
        state_cv_.notify_all();
        if (notify) report_health(false);
        stop_transport();
    }

    void arm_deadline(std::chrono::milliseconds timeout) {
        const auto generation = ++deadline_generation_;
        impl_->deadline->expires_after(timeout);
        impl_->deadline->async_wait([this, generation](const boost::system::error_code& error) {
            if (!error && generation == deadline_generation_ && running()) fail_stream();
        });
    }

    void handle_message(const std::string& payload) {
        if (!running()) return;
        try {
            const auto message = nlohmann::json::parse(payload);
            Protocol::Result result;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!running()) return;
                result = protocol_.receive(message);
                if (result == Protocol::Result::Acknowledged) {
                    healthy_.store(true, std::memory_order_release);
                } else if (result == Protocol::Result::Failed) {
                    healthy_.store(false, std::memory_order_release);
                }
            }
            if (result == Protocol::Result::Failed) {
                fail_stream();
            } else if (result == Protocol::Result::Acknowledged) {
                arm_deadline(activity_timeout_);
                state_cv_.notify_all();
                if (healthy()) report_health(true);
            } else if (result == Protocol::Result::Event && running() && healthy()) {
                arm_deadline(activity_timeout_);
                if (handler_) handler_(message.at("event"));
            }
        } catch (...) {
            // Losing an authenticated order event requires reconciliation.
            fail_stream();
        }
    }

    void opened(websocketpp::connection_hdl handle) {
        try {
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                if (!running()) return;
                impl_->connection = handle;
                impl_->connected = true;
            }
            if (!running()) return;
            const auto request = subscription_request_(request_id_);
            if (!request.is_object() || !request.contains("id") || request.at("id") != request_id_ ||
                request.value("method", std::string{}) != "userDataStream.subscribe.signature") {
                throw std::runtime_error("User data subscription request is invalid");
            }
            websocketpp::lib::error_code error;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                if (!running()) return;
                arm_deadline(subscription_timeout_);
                impl_->client.send(handle, request.dump(), websocketpp::frame::opcode::text, error);
            }
            if (error) fail_stream();
        } catch (...) { fail_stream(); }
    }

    void run() {
        try {
            if (before_connect_) before_connect_();
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                if (!running()) return;
                impl_->client.clear_access_channels(websocketpp::log::alevel::all);
                impl_->client.clear_error_channels(websocketpp::log::elevel::all);
                const websocketpp::uri endpoint(endpoint_url_);
                if (!endpoint.get_valid() || !endpoint.get_secure()) {
                    throw std::runtime_error("User data stream requires a valid TLS endpoint");
                }
                impl_->client.set_tls_init_handler([hostname = endpoint.get_host()](websocketpp::connection_hdl) {
                    return sentum::api::make_tls_client_context(hostname);
                });
                impl_->client.set_open_handler([this](websocketpp::connection_hdl handle) { opened(handle); });
                impl_->client.set_close_handler([this](websocketpp::connection_hdl) { fail_stream(); });
                impl_->client.set_fail_handler([this](websocketpp::connection_hdl) { fail_stream(); });
                impl_->client.set_ping_handler([this](websocketpp::connection_hdl, const std::string&) {
                    if (running() && healthy()) arm_deadline(activity_timeout_);
                    return true; // retain automatic pong responses with the received payload
                });
                impl_->client.set_message_handler([this](websocketpp::connection_hdl, Client::message_ptr message) {
                    handle_message(message->get_payload());
                });
                arm_deadline(connection_timeout_);
                websocketpp::lib::error_code error;
                auto connection = impl_->client.get_connection(endpoint_url_, error);
                if (error) throw std::runtime_error("User data stream connection failed: " + error.message());
                impl_->client.connect(connection);
            }
            impl_->client.run();
        } catch (...) { /* all transport failures invalidate the subscription */ }
        fail_stream();
    }

    std::unique_ptr<Impl> impl_;
    SubscriptionRequest subscription_request_;
    Handler handler_;
    HealthHandler health_handler_;
    std::atomic<bool> running_{false};
    std::atomic<bool> healthy_{false};
    std::mutex state_mutex_;
    std::condition_variable state_cv_;
    Protocol protocol_;
    std::string request_id_;
    std::uint64_t generation_ = 0;
    std::uint64_t deadline_generation_ = 0;
    std::thread thread_;
    std::thread::id worker_id_;
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    bool joining_ = false;
    std::string endpoint_url_ = BinanceSpotExecutionClient::websocket_base_url();
    std::chrono::milliseconds connection_timeout_{10000};
    std::chrono::milliseconds subscription_timeout_{10000};
    std::chrono::milliseconds activity_timeout_{60000};
    // Lifecycle qualification can pause preparation before any connection work.
    std::function<void()> before_connect_;
};
