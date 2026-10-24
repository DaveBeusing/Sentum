/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */

#include <algorithm>
#include <cctype>
#include <iostream>
#include <cmath>
#include <mutex>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include <sentum/api/BinanceWebsocketClient.hpp>

using json = nlohmann::json;
struct BinanceWebsocketClient::Impl {
    sentum::api::ReconnectingMarketStream stream;

    Impl(std::string url, std::string hostname, sentum::api::MarketStreamSettings settings,
         sentum::api::ReconnectingMarketStream::MessageHandler message,
         sentum::api::ReconnectingMarketStream::HealthHandler health)
        : stream(std::move(url), std::move(hostname), settings, std::move(message), std::move(health)) {}
};

BinanceWebsocketClient::BinanceWebsocketClient(const std::string& sym, std::chrono::milliseconds maximum_event_age,
    sentum::api::BinanceEnvironment environment)
    : symbol(sym), environment_(environment), running(false) {
    (void)sentum::api::market_trade_url(environment_, symbol);
    if (maximum_event_age.count() <= 0 || maximum_event_age > std::chrono::hours(24)) {
        throw std::invalid_argument("Market event age must be positive and no greater than 24 hours");
    }
    stream_settings.maximum_event_age = maximum_event_age;
    stream_settings.idle_reconnect_timeout = std::max(std::chrono::milliseconds(10000), maximum_event_age * 5);
}

BinanceWebsocketClient::~BinanceWebsocketClient() {
	stop();
}

void BinanceWebsocketClient::set_on_price(const std::function<void(double)>& cb) {
	std::lock_guard<std::mutex> lock(callback_mutex);
	on_price = cb;
}

void BinanceWebsocketClient::set_on_trade(const TradeCallback& cb) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    on_trade = cb;
}

void BinanceWebsocketClient::set_on_health(const HealthCallback& cb) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    on_health = cb;
}

sentum::api::MarketStreamHealth BinanceWebsocketClient::stream_health() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    return impl ? impl->stream.health() : stopped_health;
}

void BinanceWebsocketClient::start() {
	std::unique_lock<std::mutex> lock(lifecycle_mutex);
	if (worker_id == std::this_thread::get_id()) {
		if (running.load()) return;
		throw std::logic_error("A WebSocket worker cannot restart itself before it has been joined");
	}
	lifecycle_cv.wait(lock, [this] { return !joining; });
	if (running.load()) return;
	join_worker(lock);
    const std::string hostname = test_hostname.empty() ? sentum::api::market_stream_host(environment_) : test_hostname;
    const std::string url = test_url.empty() ? endpoint() : test_url;
    stopped_health = {};
    impl = std::make_unique<Impl>(url, hostname, stream_settings,
        [this](const std::string& payload) { handle_message(payload); },
        [this](const sentum::api::MarketStreamHealth& health) {
            HealthCallback callback;
            { std::lock_guard<std::mutex> lock(callback_mutex); callback = on_health; }
            if (callback) callback(health);
        });
	running.store(true);
	try {
		ws_thread = std::thread(&BinanceWebsocketClient::run, this);
		worker_id = ws_thread.get_id();
	} catch (...) {
		request_stop();
		impl.reset();
		throw;
	}
}

void BinanceWebsocketClient::stop() {
	std::unique_lock<std::mutex> lock(lifecycle_mutex);
	request_stop();
	if (worker_id == std::this_thread::get_id()) return;
	lifecycle_cv.wait(lock, [this] { return !joining; });
	request_stop();
	join_worker(lock);
    if (impl) stopped_health = impl->stream.health();
	impl.reset();
}

void BinanceWebsocketClient::join_worker(std::unique_lock<std::mutex>& lock) {
	if (!ws_thread.joinable()) return;
	joining = true;
	lock.unlock();
	ws_thread.join();
	lock.lock();
	worker_id = {};
	joining = false;
	lifecycle_cv.notify_all();
}

void BinanceWebsocketClient::request_stop() {
    running.store(false);
    if (impl) impl->stream.stop();
}

void BinanceWebsocketClient::handle_message(const std::string& payload, bool network_message) {
    try {
        const auto j = json::parse(payload);
        if (!j.is_object() || !j.contains("p") || !j["p"].is_string()) return;
        const auto text = j["p"].get<std::string>();
        std::size_t consumed = 0;
        const double price = std::stod(text, &consumed);
        if (consumed != text.size() || !std::isfinite(price) || price <= 0.0) return;
        auto timestamp = std::chrono::system_clock::now();
        if (network_message) {
            if (!j.contains("T") || !j["T"].is_number_integer()) return;
            const auto milliseconds = j["T"].get<std::int64_t>();
            if (milliseconds <= 0) return;
            if (!impl->stream.accept_event(milliseconds)) return;
            timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(milliseconds));
        }
        std::function<void(double)> price_callback;
        TradeCallback trade_callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            price_callback = on_price;
            trade_callback = on_trade;
        }
        if (!running.load()) return;
        if (trade_callback) trade_callback(price, timestamp);
        if (running.load() && price_callback) price_callback(price);
    } catch (const std::exception& e) {
        std::cerr << "[WS] Error parsing message: " << e.what() << '\n';
    }
}

void BinanceWebsocketClient::run() {
    try {
        if (before_connect) before_connect();
        if (running.load()) impl->stream.run();
    } catch (const std::exception& e) {
        if (running.load()) std::cerr << "[WS] Connection error: " << e.what() << '\n';
    }
    running.store(false);
}
