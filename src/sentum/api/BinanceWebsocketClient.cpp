/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <mutex>

#include <boost/asio/ssl/context.hpp>
#include <nlohmann/json.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include <sentum/api/BinanceWebsocketClient.hpp>

using json = nlohmann::json;
using tls_client = websocketpp::client<websocketpp::config::asio_tls_client>;

struct BinanceWebsocketClient::Impl {
	tls_client client;
	websocketpp::connection_hdl connection;
	std::mutex mutex;
	bool connection_valid = false;
};

BinanceWebsocketClient::BinanceWebsocketClient(const std::string& sym)
	: impl(std::make_unique<Impl>()), symbol(sym), running(false) {}

BinanceWebsocketClient::~BinanceWebsocketClient() {
	stop();
}

void BinanceWebsocketClient::set_on_price(const std::function<void(double)>& cb) {
	on_price = cb;
}

void BinanceWebsocketClient::start() {
	if (running.exchange(true)) return;
	ws_thread = std::thread(&BinanceWebsocketClient::run, this);
}

void BinanceWebsocketClient::stop() {
	if (!running.exchange(false)) return;

	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		if (impl->connection_valid) {
			websocketpp::lib::error_code ec;
			impl->client.close(impl->connection, websocketpp::close::status::going_away, "shutdown", ec);
		}
	}
	impl->client.stop_perpetual();
	impl->client.stop();

	if (ws_thread.joinable() && ws_thread.get_id() != std::this_thread::get_id()) {
		ws_thread.join();
	}
}

void BinanceWebsocketClient::run() {
	while (running.load()) {
		try {
			impl = std::make_unique<Impl>();
			impl->client.init_asio();
			impl->client.start_perpetual();
			impl->client.set_tls_init_handler([](websocketpp::connection_hdl) {
				auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(
					boost::asio::ssl::context::tls_client);
				ctx->set_options(boost::asio::ssl::context::default_workarounds |
					boost::asio::ssl::context::no_sslv2 |
					boost::asio::ssl::context::no_sslv3);
				return ctx;
			});
			impl->client.clear_access_channels(websocketpp::log::alevel::all);
			impl->client.clear_error_channels(websocketpp::log::elevel::all);

			std::string lower = symbol;
			std::transform(lower.begin(), lower.end(), lower.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			const std::string url = "wss://stream.binance.com:9443/ws/" + lower + "@trade";

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
				try {
					const auto j = json::parse(msg->get_payload());
					if (running.load() && j.contains("p") && on_price) {
						on_price(std::stod(j["p"].get<std::string>()));
					}
				} catch (const std::exception& e) {
					std::cerr << "[WS] Error parsing message: " << e.what() << '\n';
				}
			});

			websocketpp::lib::error_code ec;
			auto con = impl->client.get_connection(url, ec);
			if (ec) throw std::runtime_error("Connection failed: " + ec.message());
			impl->client.connect(con);
			impl->client.run();
		} catch (const std::exception& e) {
			if (running.load()) std::cerr << "[WS] Connection error: " << e.what() << '\n';
		}

		if (running.load()) {
			for (int i = 0; i < 50 && running.load(); ++i) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		}
	}
}
