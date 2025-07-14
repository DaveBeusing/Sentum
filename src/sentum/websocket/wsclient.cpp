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

#include "wsclient.hpp"
#include "nlohmann/json.hpp"
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <boost/asio/ssl/context.hpp>
#include <iostream>
#include <thread>
#include <algorithm>

using json = nlohmann::json;

typedef websocketpp::client<websocketpp::config::asio_tls_client> client;

// TLS-Konfiguration
std::shared_ptr<boost::asio::ssl::context> on_tls_init() {
	auto ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
	try {
		ctx->set_options(boost::asio::ssl::context::default_workarounds |
							boost::asio::ssl::context::no_sslv2 |
							boost::asio::ssl::context::no_sslv3 |
							boost::asio::ssl::context::single_dh_use);
	} catch (std::exception& e) {
		std::cerr << "TLS Init Error: " << e.what() << std::endl;
	}
	return ctx;
}

void start_ws_price_stream(const std::string& symbol, std::function<void(double)> on_price) {
	std::thread([symbol, on_price]() {
		std::string lower = symbol;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
		std::string uri = "wss://stream.binance.com:9443/ws/" + lower + "@trade";

		client c;
		c.init_asio();

		// TLS Handler setzen
		c.set_tls_init_handler([&](websocketpp::connection_hdl) {
			return on_tls_init();
		});

		// Nachrichtenhandler
		c.set_message_handler([&on_price](websocketpp::connection_hdl, client::message_ptr msg) {
			try {
				json data = json::parse(msg->get_payload());
				double price = std::stod(data["p"].get<std::string>());
				on_price(price);
			} catch (...) {
				std::cerr << "[WebSocket] Error parsing\n";
			}
		});

		// Verbindung aufbauen
		websocketpp::lib::error_code ec;
		client::connection_ptr con = c.get_connection(uri, ec);
		if (ec) {
			std::cerr << "Connection error: " << ec.message() << std::endl;
			return;
		}

		c.connect(con);
		c.run();
	}).detach();  // eigener Thread
}