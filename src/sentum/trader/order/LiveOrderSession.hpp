#pragma once
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/api/BinanceUserDataStream.hpp>
#include <sentum/trader/order/ConfirmedPositionLedger.hpp>
#include <sentum/trader/order/OrderManager.hpp>
#include <sentum/trader/execution/ExchangeRules.hpp>

namespace sentum::order {
class LiveOrderSession {
public:
 using UpdateHandler=OrderManager::UpdateHandler;
 static std::unique_ptr<LiveOrderSession> from_environment(){const char*key=std::getenv("SENTUM_BINANCE_TESTNET_API_KEY");const char*secret=std::getenv("SENTUM_BINANCE_TESTNET_API_SECRET");const char*enabled=std::getenv("SENTUM_ENABLE_SPOT_TESTNET");if(!enabled||std::string(enabled)!="I_UNDERSTAND_TESTNET_ONLY")throw std::runtime_error("Spot Testnet execution requires SENTUM_ENABLE_SPOT_TESTNET=I_UNDERSTAND_TESTNET_ONLY");if(!key||!secret)throw std::runtime_error("Binance Spot Testnet credentials are missing");return std::unique_ptr<LiveOrderSession>(new LiveOrderSession(key,secret));}
 ~LiveOrderSession(){stop();}
 void set_update_handler(UpdateHandler h){std::lock_guard<std::mutex>l(handler_mutex_);external_handler_=std::move(h);}
 void start(){if(started_.exchange(true))return;manager_.set_update_handler([this](const Snapshot&u){ledger_.on_order_update(u);UpdateHandler h;{std::lock_guard<std::mutex>l(handler_mutex_);h=external_handler_;}if(h)h(u);});recover_stream();supervisor_running_.store(true);supervisor_thread_=std::thread([this]{supervisor_loop();});}
 void stop(){if(!started_.exchange(false))return;manager_.activate_kill_switch();ready_.store(false);supervisor_running_.store(false);if(supervisor_thread_.joinable())supervisor_thread_.join();std::lock_guard<std::mutex>l(stream_mutex_);if(stream_)stream_->stop();if(!listen_key_.empty())try{exchange_.close_listen_key(listen_key_);}catch(...){} }
 Snapshot submit(const Request&r){if(!started_.load()||!ready())throw std::logic_error("Live order session is not reconciled and ready");return manager_.submit_market(r);}
 Snapshot cancel(const std::string&id){return manager_.cancel(id);} void kill(){manager_.activate_kill_switch();ready_.store(false);} bool killed()const noexcept{return manager_.kill_switch_active();} bool ready()const noexcept{return ready_.load()&&!killed();}
 std::optional<Snapshot> get(const std::string&id)const{return manager_.get(id);} std::optional<ConfirmedPosition> confirmed_position(const std::string&s)const{return ledger_.get(s);}
 nlohmann::json account_snapshot()const{return exchange_.account();} nlohmann::json open_orders_snapshot()const{return exchange_.open_orders();}
 sentum::execution::ExchangeRules exchange_rules(const std::string&symbol)const{return sentum::execution::ExchangeRules::from_exchange_info(exchange_.exchange_info(symbol),symbol);}
 bool resume_after_reconcile(const std::string&confirmation){if(confirmation!="I_CONFIRM_RECONCILED_TESTNET_RESUME")return false;if(!started_.load())return false;try{manager_.reconcile_startup();recover_stream();manager_.clear_kill_switch_for_reconciled_resume();ready_.store(true);return true;}catch(...){ready_.store(false);return false;}}
private:
 LiveOrderSession(std::string k,std::string s):exchange_(std::move(k),std::move(s)),manager_(exchange_){}
 void recover_stream(){ready_.store(false);manager_.reconcile_startup();std::lock_guard<std::mutex>l(stream_mutex_);if(stream_)stream_->stop();if(!listen_key_.empty())try{exchange_.close_listen_key(listen_key_);}catch(...){}listen_key_=exchange_.create_listen_key();stream_=std::make_unique<BinanceUserDataStream>(listen_key_,[this](const nlohmann::json&e){manager_.on_user_data_event(e);});stream_->start();last_keepalive_=std::chrono::steady_clock::now();ready_.store(!manager_.kill_switch_active());}
 void supervisor_loop(){while(supervisor_running_.load()){std::this_thread::sleep_for(std::chrono::seconds(5));if(!supervisor_running_.load())break;auto now=std::chrono::steady_clock::now();if(now-last_keepalive_<std::chrono::minutes(25))continue;try{std::lock_guard<std::mutex>l(stream_mutex_);exchange_.keepalive_listen_key(listen_key_);last_keepalive_=now;}catch(...){ready_.store(false);manager_.activate_kill_switch();try{recover_stream();}catch(...){}}}}
 BinanceSpotExecutionClient exchange_;OrderManager manager_;ConfirmedPositionLedger ledger_;std::unique_ptr<BinanceUserDataStream>stream_;std::string listen_key_;mutable std::mutex handler_mutex_;std::mutex stream_mutex_;UpdateHandler external_handler_;std::atomic<bool>started_{false},ready_{false},supervisor_running_{false};std::thread supervisor_thread_;std::chrono::steady_clock::time_point last_keepalive_{};
};
}
