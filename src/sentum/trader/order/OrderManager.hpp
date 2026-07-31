#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/trader/order/OrderTypes.hpp>
namespace sentum::order {
class OrderManager {
public:
 using UpdateHandler=std::function<void(const Snapshot&)>;
 explicit OrderManager(BinanceSpotExecutionClient&e):exchange_(e){}
 void set_update_handler(UpdateHandler h){std::lock_guard<std::mutex>l(mutex_);handler_=std::move(h);}
 void reconcile_startup(){const auto open=exchange_.open_orders();if(!open.is_array())throw std::runtime_error("Invalid open-orders reconciliation response");std::lock_guard<std::mutex>l(mutex_);orders_.clear();for(const auto&v:open){auto s=from_rest(v);orders_[s.client_order_id]=s;}reconciled_.store(true);}
 Snapshot submit_market(const Request&r){if(!reconciled_.load())throw std::logic_error("Orders cannot be submitted before startup reconciliation");if(kill_switch_.load())throw std::logic_error("Kill switch is active");if(r.quantity<=0||r.symbol.empty()||r.client_order_id.empty())throw std::invalid_argument("Invalid order request");Snapshot p;p.symbol=r.symbol;p.client_order_id=r.client_order_id;p.side=r.side;p.requested_quantity=r.quantity;p.state=State::Pending;p.updated_at=std::chrono::system_clock::now();{std::lock_guard<std::mutex>l(mutex_);if(orders_.count(r.client_order_id))throw std::logic_error("Duplicate client order id");orders_[r.client_order_id]=p;}publish(p);try{auto ack=exchange_.place_market_order(r);std::lock_guard<std::mutex>l(mutex_);auto&c=orders_.at(r.client_order_id);c.exchange_order_id=ack.value("orderId",std::int64_t{0});c.state=State::Acknowledged;c.updated_at=std::chrono::system_clock::now();auto copy=c;publish_unlocked(copy);return copy;}catch(const std::exception&e){std::lock_guard<std::mutex>l(mutex_);auto&c=orders_.at(r.client_order_id);c.state=State::Rejected;c.rejection_reason=e.what();c.updated_at=std::chrono::system_clock::now();auto copy=c;publish_unlocked(copy);return copy;}}
 Snapshot cancel(const std::string&id){Snapshot c;{std::lock_guard<std::mutex>l(mutex_);auto it=orders_.find(id);if(it==orders_.end())throw std::out_of_range("Unknown client order id");if(it->second.state==State::Filled||it->second.state==State::Cancelled||it->second.state==State::Rejected)return it->second;it->second.state=State::Cancelling;it->second.updated_at=std::chrono::system_clock::now();c=it->second;}publish(c);exchange_.cancel_order(c.symbol,c.client_order_id);return c;}
 void activate_kill_switch(){kill_switch_.store(true);std::vector<Snapshot>v;{std::lock_guard<std::mutex>l(mutex_);for(const auto&[id,o]:orders_)if(o.state==State::Pending||o.state==State::Acknowledged||o.state==State::PartiallyFilled)v.push_back(o);}for(const auto&o:v)try{cancel(o.client_order_id);}catch(...){}}
 void clear_kill_switch_for_reconciled_resume(){if(!reconciled_.load())throw std::logic_error("Cannot resume before reconciliation");std::lock_guard<std::mutex>l(mutex_);for(const auto&[id,o]:orders_)if(o.state==State::Pending||o.state==State::Acknowledged||o.state==State::PartiallyFilled||o.state==State::Cancelling)throw std::logic_error("Cannot resume with unresolved orders");kill_switch_.store(false);}
 bool kill_switch_active()const noexcept{return kill_switch_.load();}bool reconciled()const noexcept{return reconciled_.load();}
 std::optional<Snapshot>get(const std::string&id)const{std::lock_guard<std::mutex>l(mutex_);auto it=orders_.find(id);return it==orders_.end()?std::nullopt:std::optional<Snapshot>(it->second);}
 void on_user_data_event(const nlohmann::json&e){if(e.value("e",std::string{})!="executionReport")return;auto id=e.value("c",std::string{});if(id.empty())return;Snapshot u;{std::lock_guard<std::mutex>l(mutex_);auto&c=orders_[id];c.symbol=e.value("s",c.symbol);c.client_order_id=id;c.exchange_order_id=e.value("i",c.exchange_order_id);c.side=e.value("S",std::string{"BUY"})=="SELL"?Side::Sell:Side::Buy;c.requested_quantity=parse(e,"q",c.requested_quantity);c.executed_quantity=parse(e,"z",c.executed_quantity);c.cumulative_quote_quantity=parse(e,"Z",c.cumulative_quote_quantity);c.average_fill_price=c.executed_quantity>0?c.cumulative_quote_quantity/c.executed_quantity:0;c.state=map(e.value("X",std::string{}));c.rejection_reason=e.value("r",std::string{});c.updated_at=std::chrono::system_clock::time_point(std::chrono::milliseconds(e.value("E",std::int64_t{0})));u=c;}publish(u);}
private:
 static double parse(const nlohmann::json&v,const char*k,double f){if(!v.contains(k))return f;try{return v.at(k).is_string()?std::stod(v.at(k).get<std::string>()):v.at(k).get<double>();}catch(...){return f;}}
 static State map(const std::string&s){if(s=="NEW")return State::Acknowledged;if(s=="PARTIALLY_FILLED")return State::PartiallyFilled;if(s=="FILLED")return State::Filled;if(s=="PENDING_CANCEL")return State::Cancelling;if(s=="CANCELED"||s=="EXPIRED")return State::Cancelled;if(s=="REJECTED")return State::Rejected;return State::Pending;}
 static Snapshot from_rest(const nlohmann::json&v){Snapshot r;r.symbol=v.value("symbol",std::string{});r.client_order_id=v.value("clientOrderId",std::string{});r.exchange_order_id=v.value("orderId",std::int64_t{0});r.side=v.value("side",std::string{"BUY"})=="SELL"?Side::Sell:Side::Buy;r.requested_quantity=parse(v,"origQty",0);r.executed_quantity=parse(v,"executedQty",0);r.cumulative_quote_quantity=parse(v,"cummulativeQuoteQty",0);r.average_fill_price=r.executed_quantity>0?r.cumulative_quote_quantity/r.executed_quantity:0;r.state=map(v.value("status",std::string{}));r.updated_at=std::chrono::system_clock::now();return r;}
 void publish(const Snapshot&v){UpdateHandler h;{std::lock_guard<std::mutex>l(mutex_);h=handler_;}if(h)h(v);}void publish_unlocked(const Snapshot&v){auto h=handler_;if(h)h(v);}
 BinanceSpotExecutionClient&exchange_;mutable std::mutex mutex_;std::unordered_map<std::string,Snapshot>orders_;UpdateHandler handler_;std::atomic<bool>reconciled_{false},kill_switch_{false};
};
}
