#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/api/TlsClientContext.hpp>

class IUserDataStream {
public:
    using Handler = std::function<void(const nlohmann::json&)>;
    virtual ~IUserDataStream() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool running() const noexcept = 0;
};

class BinanceUserDataStream final : public IUserDataStream {
public:
    using Handler = IUserDataStream::Handler;

    BinanceUserDataStream(std::string listen_key, Handler handler)
        : listen_key_(std::move(listen_key)), handler_(std::move(handler)) {}

    ~BinanceUserDataStream() override { stop(); }

    void start() override {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        if (worker_id_ == std::this_thread::get_id()) {
            if (running_.load()) return;
            throw std::logic_error("A user data worker cannot restart itself before it has been joined");
        }
        lifecycle_cv_.wait(lock, [this] { return !joining_; });
        if (running_.load()) return;
        join_worker(lock);
        impl_ = std::make_unique<Impl>();
        running_.store(true);
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

private:
    friend struct WebsocketLifecycleTestAccess;
    using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
    struct Impl {
        Client client;
        websocketpp::connection_hdl connection;
        std::mutex mutex;
        bool connected = false;

        Impl() {
            client.init_asio();
            client.start_perpetual();
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

    void request_stop() {
        running_.store(false);
        if (!impl_) return;
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->connected) {
            websocketpp::lib::error_code ec;
            impl_->client.close(impl_->connection, websocketpp::close::status::going_away, "shutdown", ec);
        }
        impl_->client.stop_perpetual();
        impl_->client.stop();
    }

    void handle_message(const std::string& payload) {
        if (!running_.load() || !handler_) return;
        try { handler_(nlohmann::json::parse(payload)); }
        catch (...) { /* malformed exchange events never mutate local order state */ }
    }

    void run() {
        try {
            if (before_connect_) before_connect_();
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                if (!running_.load()) return;
                impl_->client.clear_access_channels(websocketpp::log::alevel::all);
                impl_->client.clear_error_channels(websocketpp::log::elevel::all);
                const std::string websocket_base = BinanceSpotExecutionClient::websocket_base_url();
                const websocketpp::uri endpoint(websocket_base);
                if (!endpoint.get_valid() || !endpoint.get_secure()) {
                    throw std::runtime_error("User data stream requires a valid TLS endpoint");
                }
                impl_->client.set_tls_init_handler([hostname = endpoint.get_host()](websocketpp::connection_hdl) {
                    return sentum::api::make_tls_client_context(hostname);
                });
                impl_->client.set_open_handler([this](websocketpp::connection_hdl handle) {
                    std::lock_guard<std::mutex> lock(impl_->mutex);
                    impl_->connection = handle;
                    impl_->connected = true;
                });
                auto disconnected = [this](websocketpp::connection_hdl) {
                    std::lock_guard<std::mutex> lock(impl_->mutex);
                    impl_->connected = false;
                };
                impl_->client.set_close_handler(disconnected);
                impl_->client.set_fail_handler(disconnected);
                impl_->client.set_message_handler([this](websocketpp::connection_hdl, Client::message_ptr message) {
                    handle_message(message->get_payload());
                });
                websocketpp::lib::error_code ec;
                auto connection = impl_->client.get_connection(websocket_base + listen_key_, ec);
                if (ec) throw std::runtime_error("User data stream connection failed: " + ec.message());
                impl_->client.connect(connection);
            }
            impl_->client.run();
            running_.store(false, std::memory_order_release);
        } catch (...) {
            running_.store(false, std::memory_order_release);
        }
    }

    std::unique_ptr<Impl> impl_;
    std::string listen_key_;
    Handler handler_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::thread::id worker_id_;
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    bool joining_ = false;
    // Lifecycle qualification can pause preparation before any connection work.
    std::function<void()> before_connect_;
};
