/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */

#include <chrono>
#include <mutex>
#include <stdexcept>

#include <boost/asio/ssl/context.hpp>
#include <nlohmann/json.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include <sentum/collector/Collector.hpp>
#include <sentum/utils/helper.hpp>

using json = nlohmann::json;
using client = websocketpp::client<websocketpp::config::asio_tls_client>;

struct Collector::Impl {
	client websocket;
	websocketpp::connection_hdl connection;
	std::mutex mutex;
	bool connection_valid = false;
};

Collector::Collector(Database& db, const std::vector<MarketInfo>& markets_)
	: db_ref(db), markets(markets_), running(false), logger("log/collector.log"), impl(std::make_unique<Impl>()) {}

Collector::~Collector() {
	stop();
}

void Collector::start() {
	if (running.exchange(true)) return;
	logger.start();
	ws_thread = std::thread(&Collector::run, this);
}

void Collector::stop() {
	running.store(false);
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		if (impl->connection_valid) {
			websocketpp::lib::error_code ec;
			impl->websocket.close(impl->connection, websocketpp::close::status::going_away, "shutdown", ec);
		}
	}
	impl->websocket.stop_perpetual();
	impl->websocket.stop();
	if (ws_thread.joinable() && ws_thread.get_id() != std::this_thread::get_id()) ws_thread.join();
	logger.stop();
}

void Collector::run() {
	try {
		impl->websocket.init_asio();
		impl->websocket.start_perpetual();
		impl->websocket.clear_access_channels(websocketpp::log::alevel::all);
		impl->websocket.clear_error_channels(websocketpp::log::elevel::all);
		impl->websocket.set_tls_init_handler([](websocketpp::connection_hdl) {
			auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
			ctx->set_options(boost::asio::ssl::context::default_workarounds |
				boost::asio::ssl::context::no_sslv2 |
				boost::asio::ssl::context::no_sslv3);
			return ctx;
		});
		impl->websocket.set_open_handler([this](websocketpp::connection_hdl hdl) {
			std::lock_guard<std::mutex> lock(impl->mutex);
			impl->connection = hdl;
			impl->connection_valid = true;
		});
		impl->websocket.set_close_handler([this](websocketpp::connection_hdl) {
			std::lock_guard<std::mutex> lock(impl->mutex);
			impl->connection_valid = false;
			logger.log("Websocket connection closed");
		});
		impl->websocket.set_fail_handler([this](websocketpp::connection_hdl hdl) {
			std::lock_guard<std::mutex> lock(impl->mutex);
			impl->connection_valid = false;
			auto con = impl->websocket.get_con_from_hdl(hdl);
			logger.log("Websocket failed: " + con->get_ec().message());
		});
		impl->websocket.set_message_handler([this](websocketpp::connection_hdl, client::message_ptr msg) {
			if (!running.load()) return;
			try {
				const auto payload = json::parse(msg->get_payload());
				if (!payload.contains("data") || !payload["data"].contains("k")) return;
				const auto& k = payload["data"]["k"];
				const std::string symbol = helper::to_lowercase(k["s"].get<std::string>());
				Kline entry;
				entry.timestamp = k["t"];
				entry.open = std::stod(k["o"].get<std::string>());
				entry.high = std::stod(k["h"].get<std::string>());
				entry.low = std::stod(k["l"].get<std::string>());
				entry.close = std::stod(k["c"].get<std::string>());
				entry.volume = std::stod(k["v"].get<std::string>());
				if (!db_ref.save_klines(symbol, {entry})) logger.log("save_klines() failed for " + symbol);
			} catch (const std::exception& e) {
				logger.log(std::string("parse error: ") + e.what());
			}
		});

		std::string url = "wss://stream.binance.com:443/stream?streams=";
		for (std::size_t i = 0; i < markets.size(); ++i) {
			url += helper::to_lowercase(markets[i].symbol) + "@kline_1s";
			if (i + 1 < markets.size()) url += "/";
		}

		websocketpp::lib::error_code ec;
		auto con = impl->websocket.get_connection(url, ec);
		if (ec) throw std::runtime_error("Connection error: " + ec.message());
		impl->websocket.connect(con);
		impl->websocket.run();
	} catch (const std::exception& e) {
		if (running.load()) logger.log(std::string("Collector run() error: ") + e.what());
	}
	running.store(false);
}
