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
#include <sstream>
#include <iomanip>
#include <chrono>
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include "nlohmann/json.hpp"
#include "sentum/api/binance.hpp"

using json = nlohmann::json;

static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* output) {
	output->append((char*)contents, size * nmemb);
	return size * nmemb;
}

Binance::Binance(const std::string& api_key, const std::string& api_secret) : api_key_(api_key), api_secret_(api_secret) {}

std::string Binance::get_timestamp() const {
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	return std::to_string(ms);
}

std::string Binance::hmac_sha256(const std::string& data, const std::string& key) const {
	unsigned char* digest;
	digest = HMAC(EVP_sha256(), key.c_str(), key.length(), (unsigned char*)data.c_str(), data.length(), NULL, NULL);
	std::ostringstream oss;
	for (int i = 0; i < 32; ++i)
		oss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
	return oss.str();
}

double Binance::get_current_price(const std::string& symbol) {
	std::string url = "https://api.binance.com/api/v3/ticker/price?symbol=" + symbol;
	std::string response;
	CURL* curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
		curl_easy_perform(curl);
		curl_easy_cleanup(curl);
	}
	try {
		auto json_data = json::parse(response);
		return std::stod(json_data["price"].get<std::string>());
	} catch (...) {
		std::cerr << "Error parsing price reponse" << std::endl;
		return -1.0;
	}
}

std::string Binance::send_signed_order(const std::string& symbol, const std::string& quantity) {
	std::string base_url = "https://api.binance.com";
	std::string endpoint = "/api/v3/order";
	std::string params = "symbol=" + symbol + "&side=BUY&type=MARKET&quantity=" + quantity;
	std::string query = params + "&timestamp=" + get_timestamp();
	std::string signature = hmac_sha256(query, api_secret_);
	query += "&signature=" + signature;
	std::string url = base_url + endpoint + "?" + query;
	std::string response_string;
	CURL* curl = curl_easy_init();
	struct curl_slist* headers = NULL;
	headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key_).c_str());
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
		curl_easy_perform(curl);
		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);
	}
	return response_string;
}

double Binance::get_coin_balance(const std::string& asset_symbol) {
	std::string base_url = "https://api.binance.com";
	std::string endpoint = "/api/v3/account";
	std::string query = "recvWindow=5000&timestamp=" + get_timestamp();
	std::string signature = hmac_sha256(query, api_secret_);
	std::string url = base_url + endpoint + "?" + query + "&signature=" + signature;
	std::string response;
	CURL* curl = curl_easy_init();
	struct curl_slist* headers = NULL;
	headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key_).c_str());
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
		curl_easy_perform(curl);
		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);
	}
	try {
		auto json_data = json::parse(response);
		for (const auto& balance : json_data["balances"]) {
			if (balance["asset"] == asset_symbol) {
				return std::stod(balance["free"].get<std::string>());
			}
		}
	} catch (...) {
		std::cerr << "Error parsing balance response" << std::endl;
	}
	return 0.0;
}

std::vector<Kline> Binance::get_historical_klines(const std::string& symbol, const std::string& interval, int limit) {
	std::string url = "https://api.binance.com/api/v3/klines?symbol=" + symbol + "&interval=" + interval + "&limit=" + std::to_string(limit);
	std::string response;
	CURL* curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
		curl_easy_perform(curl);
		curl_easy_cleanup(curl);
	}
	std::vector<Kline> klines;
	try {
		auto json_data = json::parse(response);
		for (const auto& entry : json_data) {
			Kline kline{
				entry[0].get<int64_t>(),                // timestamp (Open Time)
				std::stod(entry[1].get<std::string>()), // open
				std::stod(entry[2].get<std::string>()), // high
				std::stod(entry[3].get<std::string>()), // low
				std::stod(entry[4].get<std::string>()), // close
				std::stod(entry[5].get<std::string>())  // volume
			};
			klines.push_back(kline);
		}
	} catch (const std::exception& e) {
		std::cerr << "Error parsing klines: " << e.what() << "\n";
	}
	return klines;
}

json Binance::get_exchange_info() {
	std::string url = "https://api.binance.com/api/v3/exchangeInfo";
	std::string response;
	CURL* curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
		curl_easy_perform(curl);
		curl_easy_cleanup(curl);
	}
	try {
		return json::parse(response);
	} catch (const std::exception& e) {
		std::cerr << "Error parsing exchange info: " << e.what() << "\n";
		return {};
	}
}

std::vector<std::string> Binance::get_markets_by_quote(const std::string& quote_asset) {
	std::vector<std::string> markets;
	json info = get_exchange_info();
	if (!info.contains("symbols")) {
		std::cerr << "Exchange info response invalid.\n";
		return markets;
	}
	for (const auto& symbol : info["symbols"]) {
		if (symbol["status"] == "TRADING" &&
			symbol["quoteAsset"] == quote_asset &&
			symbol["isSpotTradingAllowed"].get<bool>()) {
			markets.push_back(symbol["symbol"].get<std::string>());
		}
	}
	return markets;
}