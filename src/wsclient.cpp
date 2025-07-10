#include "../include/wsclient.hpp"
#include "../lib/json.hpp"
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
				std::cerr << "[WebSocket] Fehler beim Parsen\n";
			}
		});

		// Verbindung aufbauen
		websocketpp::lib::error_code ec;
		client::connection_ptr con = c.get_connection(uri, ec);
		if (ec) {
			std::cerr << "Verbindungsfehler: " << ec.message() << std::endl;
			return;
		}

		c.connect(con);
		c.run();
	}).detach();  // eigener Thread
}