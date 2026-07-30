#pragma once

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <sentum/trader/order/OrderTypes.hpp>

class BinanceSpotExecutionClient {
public:
    BinanceSpotExecutionClient(std::string api_key, std::string api_secret)
        : api_key_(std::move(api_key)), api_secret_(std::move(api_secret)) {
        if (api_key_.empty() || api_secret_.empty()) throw std::invalid_argument("Binance testnet credentials are required");
    }

    nlohmann::json place_market_order(const sentum::order::Request& request) const {
        const std::string side = request.side == sentum::order::Side::Buy ? "BUY" : "SELL";
        std::ostringstream quantity;
        quantity << std::setprecision(16) << request.quantity;
        return signed_request("POST", "/api/v3/order",
            "symbol=" + request.symbol + "&side=" + side + "&type=MARKET&quantity=" + quantity.str() +
            "&newClientOrderId=" + request.client_order_id + "&newOrderRespType=ACK");
    }

    nlohmann::json cancel_order(const std::string& symbol, const std::string& client_order_id) const {
        return signed_request("DELETE", "/api/v3/order", "symbol=" + symbol + "&origClientOrderId=" + client_order_id);
    }

    nlohmann::json query_order(const std::string& symbol, const std::string& client_order_id) const {
        return signed_request("GET", "/api/v3/order", "symbol=" + symbol + "&origClientOrderId=" + client_order_id);
    }

    nlohmann::json open_orders() const { return signed_request("GET", "/api/v3/openOrders", ""); }

    std::string create_listen_key() const {
        const auto response = api_key_request("POST", "/api/v3/userDataStream", "");
        if (!response.contains("listenKey")) throw std::runtime_error("Testnet did not return a listen key");
        return response.at("listenKey").get<std::string>();
    }

    void keepalive_listen_key(const std::string& listen_key) const {
        api_key_request("PUT", "/api/v3/userDataStream", "listenKey=" + listen_key);
    }

    void close_listen_key(const std::string& listen_key) const {
        api_key_request("DELETE", "/api/v3/userDataStream", "listenKey=" + listen_key);
    }

    static constexpr const char* rest_base_url() noexcept { return "https://testnet.binance.vision"; }
    static constexpr const char* websocket_base_url() noexcept { return "wss://stream.testnet.binance.vision/ws/"; }

private:
    static size_t write_callback(void* data, size_t size, size_t count, void* target) {
        static_cast<std::string*>(target)->append(static_cast<char*>(data), size * count);
        return size * count;
    }

    std::string timestamp() const {
        return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    std::string sign(const std::string& value) const {
        unsigned int length = EVP_MAX_MD_SIZE;
        unsigned char digest[EVP_MAX_MD_SIZE];
        HMAC(EVP_sha256(), api_secret_.data(), static_cast<int>(api_secret_.size()),
             reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest, &length);
        std::ostringstream out;
        for (unsigned int i = 0; i < length; ++i) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
        return out.str();
    }

    nlohmann::json signed_request(const std::string& method, const std::string& path, const std::string& parameters) const {
        std::string query = parameters;
        if (!query.empty()) query += '&';
        query += "recvWindow=5000&timestamp=" + timestamp();
        query += "&signature=" + sign(query);
        return request(method, path, query, true);
    }

    nlohmann::json api_key_request(const std::string& method, const std::string& path, const std::string& query) const {
        return request(method, path, query, true);
    }

    nlohmann::json request(const std::string& method, const std::string& path, const std::string& query, bool authenticated) const {
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("Failed to initialize curl");
        std::string response;
        std::string url = std::string(rest_base_url()) + path;
        if (!query.empty()) url += "?" + query;
        struct curl_slist* headers = nullptr;
        if (authenticated) headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key_).c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        if (method == "POST") curl_easy_setopt(curl, CURLOPT_POST, 1L);
        else if (method != "GET") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        const CURLcode rc = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK) throw std::runtime_error(std::string("Binance Testnet transport error: ") + curl_easy_strerror(rc));
        auto parsed = nlohmann::json::parse(response.empty() ? "{}" : response);
        if (status < 200 || status >= 300) throw std::runtime_error("Binance Testnet rejected request: " + parsed.dump());
        return parsed;
    }

    std::string api_key_;
    std::string api_secret_;
};
