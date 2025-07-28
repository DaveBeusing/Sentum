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

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <thread>
#include <iostream>

#include <sentum/trader/TradeExecutor.hpp>

TradeExecutor::TradeExecutor(const std::string& api_key, const std::string& api_secret) : api_key_(api_key), api_secret_(api_secret) {}

std::string TradeExecutor::get_timestamp() const {
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	return std::to_string(ms);
}

std::string TradeExecutor::hmac_sha256(const std::string& data, const std::string& key) const {
	unsigned char* digest;
	digest = HMAC(EVP_sha256(), key.c_str(), key.length(), (unsigned char*)data.c_str(), data.length(), NULL, NULL);
	std::ostringstream oss;
	for (int i = 0; i < 32; ++i)
		oss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
	return oss.str();
}

std::string TradeExecutor::execute_order(const std::string& query) {
	std::string signature = hmac_sha256(query, api_secret_);
	std::string full_url = base_url_ + "/api/v3/order?" + query + "&signature=" + signature;
	return post(full_url);
}

std::string TradeExecutor::post(const std::string& url) {
	for (int attempt = 0; attempt < max_retries_; ++attempt) {
		CURL* curl = curl_easy_init();
		std::string response;
		if (curl) {
			struct curl_slist* headers = nullptr;
			headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key_).c_str());
			curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds_);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, [](void* contents, size_t size, size_t nmemb, std::string* output) {
				output->append((char*)contents, size * nmemb);
				return size * nmemb;
			});
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
			CURLcode res = curl_easy_perform(curl);
			long http_code = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
			curl_easy_cleanup(curl);
			curl_slist_free_all(headers);
			if (res == CURLE_OK && http_code == 200){
				return response;
			}
			std::cerr << "[Retry " << (attempt + 1) << "] Error: HTTP " << http_code << ", cURL: " << curl_easy_strerror(res) << "\n";
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	return "{\"error\": \"Order failed after retries\"}";
}
