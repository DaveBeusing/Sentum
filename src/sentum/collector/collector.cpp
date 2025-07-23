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

#include "sentum/collector/collector.hpp"
#include "sentum/utils/database.hpp"
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <boost/asio/ssl/context.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <thread>

using json = nlohmann::json;
using client = websocketpp::client<websocketpp::config::asio_tls_client>;

Collector::Collector(Database& db, const std::vector<std::string>& sym)
	: db_ref(db), symbols(sym), running(false) {}

Collector::~Collector() {
	stop();
}

void Collector::start() {
	running = true;
	ws_thread = std::thread(&Collector::run, this);
}

void Collector::stop() {
	running = false;
	if (ws_thread.joinable()) ws_thread.join();
}

void log_message(std::ofstream& log, const std::string& message) {
	auto t = std::time(nullptr);
	auto tm = *std::localtime(&t);
	log << "[" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "] " << message << "\n";
	log.flush();
}

void Collector::run() {
	client c;
	c.init_asio();
	// Disable websocketpp internal logging
	c.clear_access_channels(websocketpp::log::alevel::all);
	c.clear_error_channels(websocketpp::log::elevel::all);
	// TLS-Kontext
	c.set_tls_init_handler([](websocketpp::connection_hdl) {
		return websocketpp::lib::make_shared<boost::asio::ssl::context>(
			boost::asio::ssl::context::sslv23
		);
	});
	//Logfile stream
	std::ofstream log("log/collector.log", std::ios::app);
	// Nachricht empfangen
	c.set_message_handler([this, &log](websocketpp::connection_hdl, client::message_ptr msg) {
		try {
			auto payload = json::parse(msg->get_payload());
			if (!payload.contains("data")) return;
			auto k = payload["data"]["k"];
			std::string symbol = k["s"];
			Kline entry;
			entry.timestamp = k["t"];
			entry.open      = std::stod(k["o"].get<std::string>());
			entry.high      = std::stod(k["h"].get<std::string>());
			entry.low       = std::stod(k["l"].get<std::string>());
			entry.close     = std::stod(k["c"].get<std::string>());
			entry.volume    = std::stod(k["v"].get<std::string>());
			db_ref.save_klines(symbol, {entry});
			//std::cout << "[" << symbol << "] @ " << entry.timestamp << "\n";
		} catch (const std::exception& e) {
			std::string msg = std::string("Collector::run().set_message_handler parsing error: ") + e.what();
			std::cerr << msg << "\n";
			log_message(log, msg);
		}
	});
	//  Fehlerbehandlung bei Verbindungsabbruch
	c.set_fail_handler([this, &c, &log](websocketpp::connection_hdl hdl) {
		auto con = c.get_con_from_hdl(hdl);
		std::string msg = "Collector::run() websocket failed " + con->get_ec().message();
		std::cerr << msg << "\n";
		log_message(log, msg);
		if (running) {
			std::this_thread::sleep_for(std::chrono::seconds(5));
			run();
		}
	});
	c.set_close_handler([this, &log](websocketpp::connection_hdl) {
		std::string msg = "Collector::run() websocket connection closed";
		std::cerr << msg << "\n";
		log_message(log, msg);
		if (running) {
			std::this_thread::sleep_for(std::chrono::seconds(5));
			run();
		}
	});

	// URL zusammenbauen
	std::string url = "wss://stream.binance.com:9443/stream?streams=";
	for (size_t i = 0; i < symbols.size(); ++i) {
		url += symbols[i] + "@kline_1m";
		if (i < symbols.size() - 1) url += "/";
	}
	websocketpp::lib::error_code ec;
	auto con = c.get_connection(url, ec);
	if (ec) {
		std::string msg = "Collector::run() websocket connection not established: " + ec.message();
		std::cerr << msg << "\n";
		log_message(log, msg);
		return;
	}
	c.connect(con);
	c.run();  // blockiert → läuft im eigenen Thread
}