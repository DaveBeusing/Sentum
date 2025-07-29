/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * 
 * MIT License - https://opensource.org/license/mit/
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the “Software”), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is furnished 
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all 
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A 
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION 
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 */

#include <iostream>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <thread>
#include <atomic>
#include <sstream>

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <boost/asio/ssl/context.hpp>

#include <nlohmann/json.hpp>

#include <sentum/utils/database.hpp>
#include <sentum/utils/helper.hpp>
#include <sentum/collector/Collector.hpp>
#include <sentum/utils/AsyncLogger.hpp>

using json = nlohmann::json;
using client = websocketpp::client<websocketpp::config::asio_tls_client>;

Collector::Collector(Database& db, const std::vector<std::string>& sym) : db_ref(db), symbols(sym), running(false), logger("log/collector.log") {}

Collector::~Collector() {
	stop();
}

void Collector::start() {
	running = true;
	logger.start();
	ws_thread = std::thread(&Collector::run, this);
}

void Collector::stop() {
	running = false;
	if (ws_thread.joinable()) ws_thread.join();
	logger.stop();
}

void Collector::run() {
	while(running){
		try {
			client c;
			c.init_asio();
			c.clear_access_channels(websocketpp::log::alevel::all);
			c.clear_error_channels(websocketpp::log::elevel::all);
			c.set_tls_init_handler([](websocketpp::connection_hdl) {
				return websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);//sslv23
			});
			c.set_message_handler([this](websocketpp::connection_hdl, client::message_ptr msg) {
				try {
					auto payload = json::parse(msg->get_payload());
					if (!payload.contains("data")) return;
					const auto& k = payload["data"]["k"];
					const std::string& symbol = helper::to_lowercase(k["s"].get<std::string>());
					Kline entry;
					entry.timestamp = k["t"];
					entry.open      = std::stod(k["o"].get<std::string>());
					entry.high      = std::stod(k["h"].get<std::string>());
					entry.low       = std::stod(k["l"].get<std::string>());
					entry.close     = std::stod(k["c"].get<std::string>());
					entry.volume    = std::stod(k["v"].get<std::string>());
					if (!db_ref.save_klines(symbol, {entry})) {
						logger.log( "save_klines() failed for " + symbol);
					}
				} catch (const std::exception& e) {
					logger.log( std::string("parse error: ") + e.what());
				}
			});
			c.set_fail_handler([this, &c](websocketpp::connection_hdl hdl) {
				auto con = c.get_con_from_hdl(hdl);
				logger.log( "Websocket failed: " + con->get_ec().message());
				//if (running) {
				//	std::this_thread::sleep_for(std::chrono::seconds(5));
				//	run();
				//}
			});
			c.set_close_handler([this](websocketpp::connection_hdl) {
 				logger.log( "Websocket connection closed");
				//if (running) {
				//	std::this_thread::sleep_for(std::chrono::seconds(5));
				//	run();
				//}
			});
			// URL zusammenbauen
			// https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams#klinecandlestick-streams-for-utc
			// 1s 1m 3m 5m 15m 30m 1h
			std::string url = "wss://stream.binance.com:443/stream?streams=";
			for (size_t i = 0; i < symbols.size(); ++i) {
				url += helper::to_lowercase( symbols[i] ) + "@kline_1s";//1m
				if (i < symbols.size() - 1) url += "/";
			}
			websocketpp::lib::error_code ec;
			client::connection_ptr con = c.get_connection(url, ec);
			if (ec) {
				logger.log( "Connection error: " + ec.message());
				std::this_thread::sleep_for(std::chrono::seconds(5));
				continue;
			}
			c.connect(con);
			c.run();  // blockiert → läuft im eigenen Thread

		}
		catch( const std::exception& e ){
			logger.log( std::string("Collector run() error: ") + e.what());
		}
		if (running) {
			std::this_thread::sleep_for(std::chrono::seconds(5));
			logger.log("Reconnecting...");
		}
	}
}