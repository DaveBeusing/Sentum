#include "../include/api.hpp"
#include "../include/config.hpp"
#include "../lib/json.hpp"
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <iostream>

using json = nlohmann::json;

std::string get_timestamp() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(ms);
}

std::string hmac_sha256(const std::string& data, const std::string& key) {
    unsigned char* digest;
    digest = HMAC(EVP_sha256(), key.c_str(), key.length(),
                  (unsigned char*)data.c_str(), data.length(), NULL, NULL);
    std::ostringstream oss;
    for (int i = 0; i < 32; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    return oss.str();
}

static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* output) {
    output->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string send_signed_order(const std::string& symbol, const std::string& quantity) {
    std::string base_url = "https://api.binance.com";
    std::string endpoint = "/api/v3/order";
    std::string params = "symbol=" + symbol + "&side=BUY&type=MARKET&quantity=" + quantity;
    std::string query = params + "&timestamp=" + get_timestamp();
    std::string signature = hmac_sha256(query, get_secret_key());
    query += "&signature=" + signature;

    std::string url = base_url + endpoint + "?" + query;
    std::string response_string;

    CURL* curl = curl_easy_init();
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + get_api_key()).c_str());

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }

    return response_string;
}

double get_current_price(const std::string& symbol) {
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
        auto json_data = nlohmann::json::parse(response);
        return std::stod(json_data["price"].get<std::string>());
    } catch (...) {
        std::cerr << "Fehler beim Parsen der Binance-Antwort!" << std::endl;
        return -1.0;
    }
}