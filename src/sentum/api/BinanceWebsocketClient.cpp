/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */

#include <algorithm>
#include <cctype>
#include <iostream>
#include <mutex>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include <sentum/api/BinanceWebsocketClient.hpp>
#include <sentum/api/TlsClientContext.hpp>

using json = nlohmann::json;
using tls_client = websocketpp::client<websocketpp::config::asio_tls_client>;

struct BinanceWebsocketClient::Impl {
	tls_client client;
	websocketpp::connection_hdl connection;
	std::mutex mutex;
	bool connection_valid = false;

	Impl() {
		client.init_asio();
		client.start_perpetual();
	}
};

BinanceWebsocketClient::BinanceWebsocketClient(const std::string& sym)
	: symbol(sym), running(false) {}

BinanceWebsocketClient::~BinanceWebsocketClient() {
	stop();
}

void BinanceWebsocketClient::set_on_price(const std::function<void(double)>& cb) {
	std::lock_guard<std::mutex> lock(callback_mutex);
	on_price = cb;
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
	impl = std::make_unique<Impl>();
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
	if (!impl) return;
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		if (impl->connection_valid) {
			websocketpp::lib::error_code ec;
			impl->client.close(impl->connection, websocketpp::close::status::going_away, "shutdown", ec);
		}
		impl->client.stop_perpetual();
		impl->client.stop();
	}
}

void BinanceWebsocketClient::handle_message(const std::string& payload) {
	try {
		const auto j = json::parse(payload);
		std::function<void(double)> callback;
		{
			std::lock_guard<std::mutex> lock(callback_mutex);
			callback = on_price;
		}
		if (running.load() && j.contains("p") && callback) {
			callback(std::stod(j["p"].get<std::string>()));
		}
	} catch (const std::exception& e) {
		std::cerr << "[WS] Error parsing message: " << e.what() << '\n';
	}
}

void BinanceWebsocketClient::run() {
	try {
		if (before_connect) before_connect();
		{
			std::lock_guard<std::mutex> lock(impl->mutex);
			if (!running.load()) return;
			const std::string hostname = "stream.binance.com";
			impl->client.set_tls_init_handler([hostname](websocketpp::connection_hdl) {
				return sentum::api::make_tls_client_context(hostname);
			});
			impl->client.clear_access_channels(websocketpp::log::alevel::all);
			impl->client.clear_error_channels(websocketpp::log::elevel::all);

			std::string lower = symbol;
			std::transform(lower.begin(), lower.end(), lower.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			const std::string url = "wss://" + hostname + ":9443/ws/" + lower + "@trade";

			impl->client.set_open_handler([this](websocketpp::connection_hdl hdl) {
				std::lock_guard<std::mutex> lock(impl->mutex);
				impl->connection = hdl;
				impl->connection_valid = true;
			});
			impl->client.set_close_handler([this](websocketpp::connection_hdl) {
				std::lock_guard<std::mutex> lock(impl->mutex);
				impl->connection_valid = false;
			});
			impl->client.set_fail_handler([this](websocketpp::connection_hdl) {
				std::lock_guard<std::mutex> lock(impl->mutex);
				impl->connection_valid = false;
			});
			impl->client.set_message_handler([this](websocketpp::connection_hdl, tls_client::message_ptr msg) {
				handle_message(msg->get_payload());
			});

			websocketpp::lib::error_code ec;
			auto con = impl->client.get_connection(url, ec);
			if (ec) throw std::runtime_error("Connection failed: " + ec.message());
			impl->client.connect(con);
		}
		impl->client.run();
	} catch (const std::exception& e) {
		if (running.load()) std::cerr << "[WS] Connection error: " << e.what() << '\n';
	}
	running.store(false);
}
