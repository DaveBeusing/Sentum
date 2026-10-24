#pragma once

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <sentum/trader/order/OrderTypes.hpp>

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
        const std::string side = request.side == sentum::order::Side::Buy ? "BUY" : "SELL";
        std::ostringstream quantity; quantity << std::setprecision(16) << request.quantity;
        return signed_request("POST", "/api/v3/order", "symbol="+request.symbol+"&side="+side+"&type=MARKET&quantity="+quantity.str()+"&newClientOrderId="+request.client_order_id+"&newOrderRespType=ACK");
    }
    virtual nlohmann::json cancel_order(const std::string& symbol,const std::string& client_order_id) const { return signed_request("DELETE","/api/v3/order","symbol="+symbol+"&origClientOrderId="+client_order_id); }
    virtual nlohmann::json query_order(const std::string& symbol,const std::string& client_order_id) const { return signed_request("GET","/api/v3/order","symbol="+symbol+"&origClientOrderId="+client_order_id); }
    virtual nlohmann::json open_orders() const { return signed_request("GET","/api/v3/openOrders",""); }
    virtual nlohmann::json account() const { return signed_request("GET","/api/v3/account","omitZeroBalances=false"); }
    virtual nlohmann::json exchange_info(const std::string& symbol) const { return request("GET","/api/v3/exchangeInfo","symbol="+symbol,false); }

    virtual std::string create_listen_key() const { const auto r=api_key_request("POST","/api/v3/userDataStream",""); if(!r.contains("listenKey")) throw std::runtime_error("Testnet did not return a listen key"); return r.at("listenKey").get<std::string>(); }
    virtual void keepalive_listen_key(const std::string& key) const { api_key_request("PUT","/api/v3/userDataStream","listenKey="+key); }
    virtual void close_listen_key(const std::string& key) const { api_key_request("DELETE","/api/v3/userDataStream","listenKey="+key); }

    static constexpr const char* rest_base_url() noexcept { return "https://testnet.binance.vision"; }
    static constexpr const char* websocket_base_url() noexcept { return "wss://stream.testnet.binance.vision/ws/"; }

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
    static size_t write_callback(void* data,size_t size,size_t count,void* target){static_cast<std::string*>(target)->append(static_cast<char*>(data),size*count);return size*count;}
    std::string timestamp() const {return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
    std::string sign(const std::string& value) const {unsigned int length=EVP_MAX_MD_SIZE;unsigned char digest[EVP_MAX_MD_SIZE];HMAC(EVP_sha256(),api_secret_.data(),static_cast<int>(api_secret_.size()),reinterpret_cast<const unsigned char*>(value.data()),value.size(),digest,&length);std::ostringstream out;for(unsigned int i=0;i<length;++i)out<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<int>(digest[i]);return out.str();}
    nlohmann::json signed_request(const std::string&m,const std::string&p,const std::string&params) const {std::string q=params;if(!q.empty())q+='&';q+="recvWindow=5000&timestamp="+timestamp();q+="&signature="+sign(q);return request(m,p,q,true);}
    nlohmann::json api_key_request(const std::string&m,const std::string&p,const std::string&q) const {return request(m,p,q,true);}
    nlohmann::json request(const std::string& method,const std::string& path,const std::string& query,bool authenticated) const {
        CURL* curl=curl_easy_init();if(!curl)throw std::runtime_error("Failed to initialize curl");std::string response;std::string url=std::string(rest_base_url())+path;if(!query.empty())url+="?"+query;struct curl_slist* headers=nullptr;if(authenticated)headers=curl_slist_append(headers,("X-MBX-APIKEY: "+api_key_).c_str());curl_easy_setopt(curl,CURLOPT_URL,url.c_str());curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,&write_callback);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&response);curl_easy_setopt(curl,CURLOPT_TIMEOUT,15L);if(method=="POST")curl_easy_setopt(curl,CURLOPT_POST,1L);else if(method!="GET")curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,method.c_str());const CURLcode rc=curl_easy_perform(curl);long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);if(headers)curl_slist_free_all(headers);curl_easy_cleanup(curl);if(rc!=CURLE_OK)throw BinanceRequestError(BinanceRequestError::Kind::Unknown,std::string("Binance Testnet transport error: ")+curl_easy_strerror(rc));return decode_response(status,response);
    }
    std::string api_key_; std::string api_secret_;
};
