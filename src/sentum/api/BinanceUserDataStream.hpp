#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <boost/asio/ssl/context.hpp>
#include <nlohmann/json.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include <sentum/api/BinanceSpotExecutionClient.hpp>

class BinanceUserDataStream {
public:
    using Handler = std::function<void(const nlohmann::json&)>;

    BinanceUserDataStream(std::string listen_key, Handler handler)
        : impl_(std::make_unique<Impl>()), listen_key_(std::move(listen_key)), handler_(std::move(handler)) {}

    ~BinanceUserDataStream() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        running_.store(false);
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->connected) {
                websocketpp::lib::error_code ec;
                impl_->client.close(impl_->connection, websocketpp::close::status::going_away, "shutdown", ec);
            }
        }
        impl_->client.stop_perpetual();
        impl_->client.stop();
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    }

private:
    using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
    struct Impl {
        Client client;
        websocketpp::connection_hdl connection;
        std::mutex mutex;
        bool connected = false;
    };

    void run() {
        try {
            impl_->client.init_asio();
            impl_->client.start_perpetual();
            impl_->client.clear_access_channels(websocketpp::log::alevel::all);
            impl_->client.clear_error_channels(websocketpp::log::elevel::all);
            impl_->client.set_tls_init_handler([](websocketpp::connection_hdl) {
                auto context = websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
                context->set_options(boost::asio::ssl::context::default_workarounds |
                                     boost::asio::ssl::context::no_sslv2 |
                                     boost::asio::ssl::context::no_sslv3);
                return context;
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
                if (!running_.load() || !handler_) return;
                try { handler_(nlohmann::json::parse(message->get_payload())); }
                catch (...) { /* malformed exchange events never mutate local order state */ }
            });
            websocketpp::lib::error_code ec;
            auto connection = impl_->client.get_connection(
                std::string(BinanceSpotExecutionClient::websocket_base_url()) + listen_key_, ec);
            if (ec) throw std::runtime_error("User data stream connection failed: " + ec.message());
            impl_->client.connect(connection);
            impl_->client.run();
        } catch (...) {
            running_.store(false);
        }
    }

    std::unique_ptr<Impl> impl_;
    std::string listen_key_;
    Handler handler_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};
