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

#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <boost/asio/ssl/context.hpp>
#include <nlohmann/json.hpp>

#include <sentum/api/BinanceWebsocketClient.hpp>

using json = nlohmann::json;
using tls_client = websocketpp::client<websocketpp::config::asio_tls_client>;

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

BinanceWebsocketClient::BinanceWebsocketClient(const std::string& sym) : symbol(sym), running(false) {}

BinanceWebsocketClient::~BinanceWebsocketClient(){
	stop();
}

void BinanceWebsocketClient::set_on_price(const std::function<void(double)>& cb) {
	on_price = cb;
}

void BinanceWebsocketClient::start() {
	if (running) return;
	running = true;
	ws_thread = std::thread(&BinanceWebsocketClient::run, this);
}

void BinanceWebsocketClient::stop() {
	running = false;
	if (ws_thread.joinable()) ws_thread.join();
}

void BinanceWebsocketClient::run() {
	while (running) {
		try {
			tls_client client;
			client.init_asio();
			client.set_tls_init_handler([](websocketpp::connection_hdl) {
				return websocketpp::lib::make_shared<boost::asio::ssl::context>(
					boost::asio::ssl::context::sslv23);
			});
			client.clear_access_channels(websocketpp::log::alevel::all);
			client.clear_error_channels(websocketpp::log::elevel::all);
			std::string lower = symbol;
			std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
			std::string url = "wss://stream.binance.com:9443/ws/" + lower + "@trade";
			client.set_message_handler([this](websocketpp::connection_hdl, tls_client::message_ptr msg) {
				try {
					auto j = json::parse(msg->get_payload());
					if (j.contains("p") && on_price) {
						double price = std::stod(j["p"].get<std::string>());
						on_price(price);
					}
				} catch (...) {
					std::cerr << "[WS] Error parsing message\n";
				}
			});
			websocketpp::lib::error_code ec;
			auto con = client.get_connection(url, ec);
			if (ec) {
				std::cerr << "[WS] Connection failed: " << ec.message() << "\n";
				std::this_thread::sleep_for(std::chrono::seconds(5));
				continue;
			}
			client.connect(con);
			client.run();  // Blockiert bis zum Verbindungsende
		} catch (const std::exception& e) {
			std::cerr << "[WS] Connection error: " << e.what() << "\n";
		}
		if (running) {
			std::cerr << "[WS] Try reconnect in 5 seconds...\n";
			std::this_thread::sleep_for(std::chrono::seconds(5));
		}
	}
}