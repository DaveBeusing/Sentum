#pragma once

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <sentum/trader/order/OrderTypes.hpp>
#include <sentum/api/BinanceEnvironment.hpp>

class BinanceRequestError : public std::runtime_error {
public:
    enum class Kind { Rejected, Unknown, NotFound };
    BinanceRequestError(Kind kind, std::string message, int code = 0)
        : std::runtime_error(std::move(message)), kind_(kind), code_(code) {}
    Kind kind() const noexcept { return kind_; }
    int code() const noexcept { return code_; }
private:
    Kind kind_;
    int code_;
};

class BinanceSpotExecutionClient {
public:
    BinanceSpotExecutionClient(std::string api_key, std::string api_secret)
        : api_key_(std::move(api_key)), api_secret_(std::move(api_secret)) {
        if (api_key_.empty() || api_secret_.empty()) throw std::invalid_argument("Binance testnet credentials are required");
    }

    virtual ~BinanceSpotExecutionClient() = default;

    virtual nlohmann::json place_market_order(const sentum::order::Request& request) const {
        if (!valid_identifier(request.symbol) || !valid_identifier(request.client_order_id) ||
            !std::isfinite(request.quantity) || request.quantity <= 0.0 ||
            (request.side != sentum::order::Side::Buy && request.side != sentum::order::Side::Sell))
            throw std::invalid_argument("Invalid Testnet order request");
        const auto& quantity = request.quantity_text;
        if (quantity.empty() || !std::all_of(quantity.begin(), quantity.end(),
            [](unsigned char value) { return (value >= '0' && value <= '9') || value == '.'; }))
            throw std::invalid_argument("Testnet order requires a validated decimal quantity");
        std::istringstream decimal(quantity);
        decimal.imbue(std::locale::classic());
        double parsed = 0.0;
        decimal >> parsed;
        if (!decimal || !decimal.eof() || !std::isfinite(parsed) || parsed != request.quantity)
            throw std::invalid_argument("Testnet decimal quantity does not match approved size");
        const std::string side = request.side == sentum::order::Side::Buy ? "BUY" : "SELL";
        return signed_request("POST", "/api/v3/order", "symbol="+request.symbol+"&side="+side+"&type=MARKET&quantity="+quantity+"&newClientOrderId="+request.client_order_id+"&newOrderRespType=ACK");
    }
    virtual nlohmann::json cancel_order(const std::string& symbol,const std::string& client_order_id) const { return signed_request("DELETE","/api/v3/order","symbol="+symbol+"&origClientOrderId="+client_order_id); }
    virtual nlohmann::json query_order(const std::string& symbol,const std::string& client_order_id) const { return signed_request("GET","/api/v3/order","symbol="+symbol+"&origClientOrderId="+client_order_id); }
    virtual nlohmann::json open_orders() const { return signed_request("GET","/api/v3/openOrders",""); }
    virtual nlohmann::json account() const { return signed_request("GET","/api/v3/account","omitZeroBalances=false"); }
    virtual nlohmann::json exchange_info(const std::string& symbol) const { return request("GET","/api/v3/exchangeInfo","symbol="+symbol,false); }
    virtual nlohmann::json account_trades(const std::string& symbol, std::int64_t from_id, int limit = 1000) const {
        if (!valid_identifier(symbol) || from_id < 0 || limit <= 0 || limit > 1000)
            throw std::invalid_argument("Invalid Testnet trade-history cursor");
        return signed_request("GET", "/api/v3/myTrades", "symbol=" + symbol + "&fromId=" +
            std::to_string(from_id) + "&limit=" + std::to_string(limit));
    }
    virtual nlohmann::json reference_price(const std::string& symbol) const {
        return request("GET", "/api/v3/referencePrice", "symbol=" + symbol, false);
    }
    virtual nlohmann::json average_price(const std::string& symbol) const {
        return request("GET", "/api/v3/avgPrice", "symbol=" + symbol, false);
    }
    virtual nlohmann::json ticker_price(const std::string& symbol) const {
        return request("GET", "/api/v3/ticker/price", "symbol=" + symbol, false);
    }

    virtual nlohmann::json user_data_subscription(const std::string& request_id) const {
        if (!valid_identifier(request_id) || request_id.size() > 36)
            throw std::invalid_argument("Invalid user-stream request identity");
        const auto time = timestamp();
        const auto payload = "apiKey=" + api_key_ + "&recvWindow=5000&timestamp=" + time;
        return {{"id", request_id}, {"method", "userDataStream.subscribe.signature"},
            {"params", {{"apiKey", api_key_}, {"recvWindow", 5000},
                {"timestamp", std::stoll(time)}, {"signature", sign(payload)}}}};
    }

    static constexpr const char* rest_base_url() noexcept { return "https://testnet.binance.vision"; }
    static constexpr const char* websocket_base_url() noexcept { return "wss://ws-api.testnet.binance.vision/ws-api/v3"; }
    static constexpr sentum::api::BinanceEnvironment environment() noexcept {
        return sentum::api::BinanceEnvironment::SpotTestnet;
    }

    static nlohmann::json decode_response(long status, const std::string& response) {
        nlohmann::json parsed;
        try { parsed = nlohmann::json::parse(response); }
        catch (...) {
            throw BinanceRequestError(BinanceRequestError::Kind::Unknown,
                                      "Binance Testnet returned an unreadable response");
        }
        if (status >= 200 && status < 300) return parsed;
        int code = 0;
        if (parsed.is_object() && parsed.contains("code") && parsed.at("code").is_number_integer()) {
            const auto& value = parsed.at("code");
            if (value.is_number_unsigned()) {
                const auto raw = value.get<std::uint64_t>();
                if (raw <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) code = static_cast<int>(raw);
            } else {
                const auto raw = value.get<std::int64_t>();
                if (raw >= std::numeric_limits<int>::min() && raw <= std::numeric_limits<int>::max())
                    code = static_cast<int>(raw);
            }
        }
        const std::string message = "Binance Testnet request failed (HTTP " + std::to_string(status) +
                                    ", code " + std::to_string(code) + ")";
        if (status >= 500 || code == -1000 || code == -1006 || code == -1007)
            throw BinanceRequestError(BinanceRequestError::Kind::Unknown, message, code);
        if (code == -2013)
            throw BinanceRequestError(BinanceRequestError::Kind::NotFound, message, code);
        // Only documented pre-execution/authentication/validation failures and
        // an explicit matching-engine rejection establish that no order ran.
        const bool rejected = status >= 400 && status < 500 &&
            (code == -1002 || code == -1003 || code == -1013 || code == -1015 ||
             code == -1020 || code == -1021 || code == -1022 ||
             (code <= -1100 && code >= -1199) || code == -2010 ||
             code == -2014 || code == -2015);
        throw BinanceRequestError(rejected ? BinanceRequestError::Kind::Rejected :
                                            BinanceRequestError::Kind::Unknown, message, code);
    }
private:
    static bool valid_identifier(const std::string& value) {
        return !value.empty() && value.size() <= 128 &&
            std::all_of(value.begin(), value.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
            });
    }
    static size_t write_callback(void* data,size_t size,size_t count,void* target){static_cast<std::string*>(target)->append(static_cast<char*>(data),size*count);return size*count;}
    std::string timestamp() const {return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
    std::string sign(const std::string& value) const {unsigned int length=EVP_MAX_MD_SIZE;unsigned char digest[EVP_MAX_MD_SIZE];HMAC(EVP_sha256(),api_secret_.data(),static_cast<int>(api_secret_.size()),reinterpret_cast<const unsigned char*>(value.data()),value.size(),digest,&length);std::ostringstream out;for(unsigned int i=0;i<length;++i)out<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<int>(digest[i]);return out.str();}
    nlohmann::json signed_request(const std::string&m,const std::string&p,const std::string&params) const {std::string q=params;if(!q.empty())q+='&';q+="recvWindow=5000&timestamp="+timestamp();q+="&signature="+sign(q);return request(m,p,q,true);}
    nlohmann::json request(const std::string& method,const std::string& path,const std::string& query,bool authenticated) const {
        CURL* curl=curl_easy_init();if(!curl)throw std::runtime_error("Failed to initialize curl");std::string response;std::string url=std::string(rest_base_url())+path;if(!query.empty())url+="?"+query;struct curl_slist* headers=nullptr;if(authenticated)headers=curl_slist_append(headers,("X-MBX-APIKEY: "+api_key_).c_str());curl_easy_setopt(curl,CURLOPT_URL,url.c_str());curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,&write_callback);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&response);curl_easy_setopt(curl,CURLOPT_TIMEOUT,15L);if(method=="POST")curl_easy_setopt(curl,CURLOPT_POST,1L);else if(method!="GET")curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,method.c_str());const CURLcode rc=curl_easy_perform(curl);long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);if(headers)curl_slist_free_all(headers);curl_easy_cleanup(curl);if(rc!=CURLE_OK)throw BinanceRequestError(BinanceRequestError::Kind::Unknown,std::string("Binance Testnet transport error: ")+curl_easy_strerror(rc));return decode_response(status,response);
    }
    std::string api_key_; std::string api_secret_;
};
