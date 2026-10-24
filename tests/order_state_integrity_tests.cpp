#include <sentum/trader/order/OrderManager.hpp>

#include <atomic>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
using namespace sentum::order;
using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Request request(const std::string& id = "owned") { return {"BTCUSDT", Side::Buy, 1.0, id, "BTC", "USDT"}; }

Json rest(const std::string& state, double quantity, double quote, const std::string& id = "owned", std::int64_t exchange_id = 42) {
    return {{"symbol","BTCUSDT"},{"clientOrderId",id},{"orderId",exchange_id},{"side","BUY"},
            {"origQty","1"},{"executedQty",std::to_string(quantity)},
            {"cummulativeQuoteQty",std::to_string(quote)},{"status",state},{"updateTime",2000}};
}
Json event(const std::string& state, double quantity, double quote, std::int64_t time = 2000,
           const std::string& id = "owned", std::int64_t exchange_id = 42) {
    Json result = {{"e","executionReport"},{"s","BTCUSDT"},{"c",id},{"i",exchange_id},{"S","BUY"},
            {"q","1"},{"z",std::to_string(quantity)},{"Z",std::to_string(quote)},
            {"X",state},{"E",time},{"T",time},{"r","NONE"}};
    if (quantity > 0.0 && (state == "FILLED" || state == "PARTIALLY_FILLED")) {
        result["t"]=time; result["l"]=std::to_string(quantity); result["n"]="0"; result["N"]=nullptr;
    }
    return result;
}

class Exchange final : public BinanceSpotExecutionClient {
public:
    Exchange() : BinanceSpotExecutionClient("fixture-key", "fixture-secret") {}
    Json open_orders() const override { return open; }
    Json place_market_order(const Request& value) const override {
        ++placements;
        return place ? place(value) : Json{{"symbol",value.symbol},{"clientOrderId",value.client_order_id},{"orderId",42}};
    }
    Json cancel_order(const std::string& symbol, const std::string& id) const override {
        ++cancellations;
        return cancellation ? cancellation(symbol, id) : rest("CANCELED",0,0,id);
    }
    Json query_order(const std::string& symbol, const std::string& id) const override {
        ++queries;
        queried_id = id;
        if (query) return query(symbol, id);
        throw BinanceRequestError(BinanceRequestError::Kind::NotFound, "Order not found", -2013);
    }
    Json exchange_info(const std::string& symbol) const override {
        return {{"symbols",Json::array({{{"symbol",symbol},{"baseAsset","BTC"},{"quoteAsset","USDT"}}})}};
    }
    Json open = Json::array();
    std::function<Json(const Request&)> place;
    std::function<Json(const std::string&,const std::string&)> cancellation, query;
    mutable std::atomic<int> placements{0}, cancellations{0}, queries{0};
    mutable std::string queried_id;
};

class Database {
public:
    Database() {
        std::string pattern = (std::filesystem::temp_directory_path() / "sentum-order-state-XXXXXX").string();
        const char* path = ::mkdtemp(pattern.data());
        if (!path) throw std::runtime_error("Cannot create execution fixture");
        directory = path;
    }
    ~Database() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    std::string path() const { return (directory / "orders.sqlite3").string(); }
    std::string account() const { return "integrity-" + directory.filename().string(); }
private:
    std::filesystem::path directory;
};

void monotonic_and_fee_reports() {
    Exchange exchange;
    OrderManager manager(exchange);
    manager.reconcile_startup();
    int filled_updates = 0;
    manager.set_update_handler([&](const Snapshot& update) {
        require(manager.get(update.client_order_id).has_value(), "Observer cannot inspect canonical order");
        if (update.state == State::Filled) ++filled_updates;
    });
    exchange.place = [&](const Request&) {
        auto filled = event("FILLED",1,100,3000);
        filled["t"] = 2; filled["l"]="0.6"; filled["n"] = "0.01"; filled["N"] = "USDT";
        manager.on_user_data_event(filled);
        return Json{{"orderId",42},{"symbol","BTCUSDT"},{"clientOrderId","owned"}};
    };
    const auto result = manager.submit_market(request());
    require(result.state == State::Filled && result.executed_quantity == 1, "Late ACK regressed confirmed fill");
    require(filled_updates == 1, "ACK duplicated terminal publication");
    manager.on_user_data_event(event("NEW",0,0,1000));
    require(manager.get("owned")->state == State::Filled && manager.get("owned")->executed_quantity == 1,
            "Stale NEW erased confirmed execution");
    auto old_partial = event("PARTIALLY_FILLED",.4,40,2000);
    old_partial["t"] = 1; old_partial["n"] = ".004"; old_partial["N"] = "BTC";
    manager.on_user_data_event({{"subscriptionId",0},{"event",old_partial}});
    manager.on_user_data_event(old_partial);
    const auto with_fees = manager.get("owned").value();
    require(with_fees.state == State::Filled && with_fees.executed_quantity == 1 && with_fees.commissions.size() == 2,
            "Stale cumulative report lost distinct trade fee or regressed execution");
    require(with_fees.commissions.at(1).amount == .004 && with_fees.commissions.at(2).amount == .01,
            "Trade commissions were not retained exactly once");
}

void pending_observer_cancellation() {
    Exchange exchange;
    OrderManager manager(exchange);
    manager.reconcile_startup();
    manager.set_update_handler([&](const Snapshot& update) {
        if (update.state == State::Pending) manager.cancel(update.client_order_id);
    });
    const auto result = manager.submit_market(request());
    require(result.state == State::Rejected && exchange.placements == 0 && exchange.cancellations == 0,
            "Synchronous cancellation of an unsent intent still dispatched it");
}

template<class Future>
void completed(Future& future, const char* description) {
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        std::cerr << "order_state_integrity timeout: " << description << '\n';
        std::_Exit(2);
    }
}

void concurrent_placement_reports_and_kill() {
    for (int scenario = 0; scenario != 3; ++scenario) {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup();
        std::promise<void> entered, release;
        auto entry = entered.get_future();
        auto unblock = release.get_future().share();
        std::atomic<int> filled_updates{0};
        manager.set_update_handler([&](const Snapshot& snapshot) {
            if (snapshot.state == State::Filled) ++filled_updates;
            require(manager.get(snapshot.client_order_id).has_value(), "Concurrent observer cannot reenter order manager");
        });
        exchange.place = [&](const Request&) {
            entered.set_value();
            unblock.wait();
            return Json{{"orderId",42},{"symbol","BTCUSDT"},{"clientOrderId","owned"}};
        };
        auto placement = std::async(std::launch::async,[&]{return manager.submit_market(request());});
        completed(entry,"placement did not enter transport"); entry.get();
        auto concurrent = std::async(std::launch::async,[&] {
            if (scenario == 0) manager.on_user_data_event(event("FILLED",1,100,3000));
            else if (scenario == 1) require(manager.activate_kill_switch("Concurrent transport kill"),"Kill failed");
            else require(manager.cancel("owned").state == State::Cancelled,
                         "Dispatched Pending was incorrectly rejected locally");
        });
        completed(concurrent,"report/cancel/kill blocked on placement"); concurrent.get();
        release.set_value();
        completed(placement,"placement failed to return after concurrent update");
        const auto result = placement.get();
        require(exchange.placements == 1,"Concurrent control placed more than one order");
        if (scenario == 0) {
            require(result.state == State::Filled && result.executed_quantity == 1 && filled_updates == 1,
                    "Concurrent stream fill was regressed or duplicated by ACK");
        } else {
            require(result.state == State::Cancelled && exchange.cancellations == 1,
                    "A kill or cancellation racing dispatch left an acknowledged order live");
            if (scenario == 1) require(manager.kill_switch_active(),"Concurrent kill was cleared by ACK");
        }
    }
}

void explicit_rejection_and_unknown_outcomes() {
    {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup();
        exchange.place = [](const Request&) -> Json {
            throw BinanceRequestError(BinanceRequestError::Kind::Rejected, "Matching engine rejected request", -2010);
        };
        require(manager.submit_market(request()).state == State::Rejected, "Explicit rejection was not terminal");
        require(!manager.kill_switch_active() && exchange.queries == 0, "Definitive rejection became ambiguous");
    }
    for (bool accepted : {false,true}) {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup();
        exchange.place = [](const Request&) -> Json { throw std::runtime_error("Connection lost during placement"); };
        if (accepted) exchange.query = [](const std::string&, const std::string&) { return rest("FILLED",1,100); };
        const auto result = manager.submit_market(request());
        require(manager.kill_switch_active() && exchange.queries == 1 && exchange.queried_id == "owned",
                "Unknown outcome did not latch and query its original identity");
        require(result.state == (accepted ? State::Filled : State::Unknown), "Unknown outcome fabricated rejection");
        bool blocked = false;
        try { manager.submit_market(request("new-id")); } catch (const std::logic_error&) { blocked = true; }
        require(blocked && exchange.placements == 1, "Ambiguous order allowed another placement");
        manager.reconcile_unresolved();
        require(exchange.placements == 1, "Reconciliation retried placement");
        if (!accepted) {
            require(exchange.queries == 2 && manager.get("owned")->state == State::Unknown,
                    "NotFound resolved unknown outcome without authoritative evidence");
            exchange.query = [](const std::string&,const std::string&) { return rest("CANCELED",0,0); };
            manager.reconcile_unresolved();
        }
        require(manager.kill_switch_active(), "Authoritative query silently cleared kill latch");
        if (accepted) {
            bool incomplete=false;
            try { manager.clear_kill_switch_for_reconciled_resume(); } catch (const std::logic_error&) { incomplete=true; }
            require(incomplete && manager.get("owned")->requires_reconciliation,
                    "REST fill summary silently assumed complete commission history");
        } else {
            manager.clear_kill_switch_for_reconciled_resume();
            require(!manager.kill_switch_active(), "Terminal reconciled order could not explicitly resume");
        }
    }
    {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup();
        exchange.place = [](const Request&) { return Json::array({1}); };
        require(manager.submit_market(request()).state == State::Unknown && manager.kill_switch_active(),
                "Malformed acknowledgement was treated as a rejection or success");
    }
}

void cancellation_aliases_and_races() {
    for (bool stream_first : {false,true}) {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup();
        manager.submit_market(request());
        manager.on_user_data_event(event("PARTIALLY_FILLED",.4,40,1000));
        auto cancelled = event("CANCELED",.4,40,2000,"cancel-alias"); cancelled["C"] = "owned";
        exchange.cancellation = [&](const std::string&, const std::string&) {
            if (stream_first) manager.on_user_data_event(cancelled);
            auto response = rest("CANCELED",.4,40,"cancel-alias"); response["origClientOrderId"] = "owned";
            return response;
        };
        require(manager.activate_kill_switch("Fixture cancellation"), "Ephemeral kill activation failed");
        if (!stream_first) manager.on_user_data_event(cancelled);
        const auto result = manager.get("owned").value();
        require(result.state == State::Cancelled && result.executed_quantity == .4 &&
                result.cumulative_quote_quantity == 40 && result.client_order_id == "owned",
                "Cancellation did not preserve canonical identity and partial execution");
        require(!manager.get("cancel-alias").has_value(), "Cancellation created a duplicate canonical order");
        manager.clear_kill_switch_for_reconciled_resume();
        require(!manager.kill_switch_active(), "Confirmed cancellation blocked explicit resume");
    }
    {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup(); manager.submit_market(request());
        exchange.cancellation = [&](const std::string&,const std::string&) {
            manager.on_user_data_event(event("FILLED",1,100,3000));
            return rest("CANCELED",0,0);
        };
        const auto result = manager.cancel("owned");
        require(result.state == State::Filled && result.executed_quantity == 1, "Cancellation overwrote a racing fill");
    }
}

void malformed_and_conflicting_evidence() {
    const std::vector<std::function<void(Json&)>> mutations = {
        [](Json& value){value["s"]="ETHUSDT";},
        [](Json& value){value["i"]=999;},
        [](Json& value){value["S"]="SELL";},
        [](Json& value){value["z"]="nan";},
        [](Json& value){value["Z"]="100junk";},
        [](Json& value){value["T"]=std::numeric_limits<std::int64_t>::max();},
        [](Json& value){value["q"]="2";},
        [](Json& value){value["n"]="0.1";value["N"]="BTC";value["t"]=1.5;},
        [](Json& value){value["n"]="0.1";value["N"]="BTC";value["t"]=std::numeric_limits<std::uint64_t>::max();},
        [](Json& value){value["n"]="0.1";value["N"]=nullptr;value["t"]=7;}
    };
    for (const auto& mutate : mutations) {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup(); manager.submit_market(request());
        auto invalid = event("FILLED",1,100);
        mutate(invalid);
        manager.on_user_data_event(invalid);
        const auto preserved = manager.get("owned").value();
        require(manager.kill_switch_active() && preserved.state == State::Acknowledged && preserved.executed_quantity == 0,
                "Invalid report mutated canonical execution or failed open");
    }
    for (bool mixed : {false,true}) {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup(); manager.submit_market(request());
        manager.on_user_data_event(event("PARTIALLY_FILLED",.4,40));
        manager.on_user_data_event(event("PARTIALLY_FILLED",mixed ? .3 : .4,50,3000));
        const auto preserved = manager.get("owned").value();
        require(manager.kill_switch_active() && preserved.executed_quantity == .4 && preserved.cumulative_quote_quantity == 40,
                "Contradictory cumulative counters were accepted");
    }
    {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup(); manager.submit_market(request());
        manager.on_user_data_event(event("PARTIALLY_FILLED",.4,40,3000));
        manager.on_user_data_event(event("CANCELED",.4,40,2000));
        require(manager.kill_switch_active() && manager.get("owned")->state == State::PartiallyFilled,
                "Stale terminal report displaced a newer state");
    }
    {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup(); manager.submit_market(request());
        auto valid = event("FILLED",1,100); valid["n"]="0";valid["N"]=nullptr;valid["t"]=7;
        manager.on_user_data_event(valid);
        require(manager.get("owned")->state == State::Filled && manager.get("owned")->commissions.size()==1 &&
                manager.get("owned")->commissions.at(7).amount==0 && manager.get("owned")->commissions.at(7).asset.empty(),
                "Zero commission with null asset invalidated a real fill");
        valid["n"]="0.1";valid["N"]="BTC";
        manager.on_user_data_event(valid);
        require(manager.kill_switch_active() && manager.get("owned")->commissions.at(7).amount==0,
                "A later report changed an explicitly zero trade commission");
    }
}

void durable_unknown_restart_and_publication() {
    Database database;
    Exchange exchange;
    exchange.place = [](const Request&) -> Json { throw std::runtime_error("Timeout after dispatch"); };
    {
        OrderEventRepository repository(database.path(),database.account());
        OrderManager manager(exchange,&repository); manager.reconcile_startup();
        manager.set_update_handler([&](const Snapshot& update) {
            const auto stored = repository.load_latest();
            require(stored.size() == 1 && stored.front().state == update.state,
                    "Observer ran before canonical persistence");
        });
        const auto result = manager.submit_market(request());
        require(result.state == State::Unknown && repository.load_kill_switch(), "Unknown intent was not durably blocked");
    }
    exchange.query = [](const std::string&, const std::string&) { return rest("FILLED",1,100); };
    {
        OrderEventRepository repository(database.path(),database.account());
        OrderManager manager(exchange,&repository); manager.reconcile_startup();
        const auto recovered = manager.get("owned").value();
        require(recovered.state == State::Filled && recovered.locally_owned && recovered.base_asset == "BTC" &&
                recovered.quote_asset == "USDT" && exchange.placements == 1,
                "Restart lost order ownership, assets, or replayed placement");
        require(manager.kill_switch_active(), "Restart cleared the durable safety latch");
        bool blocked=false;
        try { manager.clear_kill_switch_for_reconciled_resume(); } catch (const std::logic_error&) { blocked=true; }
        require(blocked && recovered.requires_reconciliation && repository.load_kill_switch(),
                "Recovered fill summary bypassed missing commission evidence");
        repository.finish_session();
    }
}

void durable_pending_crash_and_storage_failure() {
    {
        Database database;
        {
            OrderEventRepository repository(database.path(),database.account());
            Snapshot pending;
            pending.symbol="BTCUSDT";pending.client_order_id="owned";pending.requested_quantity=1;
            pending.locally_owned=true;pending.base_asset="BTC";pending.quote_asset="USDT";
            pending.updated_at=std::chrono::system_clock::now();
            repository.save_order(pending,"intent");
        }
        Exchange exchange;
        OrderEventRepository repository(database.path(),database.account());
        OrderManager manager(exchange,&repository); manager.reconcile_startup();
        require(manager.get("owned")->state == State::Unknown && manager.kill_switch_active(),
                "Crashed Pending reservation was assumed to be unsent");
        manager.cancel("owned");
        require(exchange.cancellations == 1, "Restored Pending was locally rejected instead of reconciled cancellation");
        require(exchange.placements == 0, "Crashed intent was placed a second time");
    }
    {
        Database database;
        Exchange exchange;
        OrderEventRepository repository(database.path(),database.account());
        OrderManager manager(exchange,&repository); manager.reconcile_startup();
        sqlite3* db=nullptr;
        require(sqlite3_open(database.path().c_str(),&db)==SQLITE_OK,"Cannot open journal failure fixture");
        require(sqlite3_exec(db,"CREATE TRIGGER deny_execution BEFORE INSERT ON execution_order_history BEGIN SELECT RAISE(ABORT,'fixture failure'); END;",nullptr,nullptr,nullptr)==SQLITE_OK,
                "Cannot install journal failure trigger");
        sqlite3_close(db);
        bool failed=false;
        try { manager.submit_market(request()); } catch (const std::exception&) { failed=true; }
        require(failed && exchange.placements==0 && manager.kill_switch_active() && !manager.storage_healthy(),
                "Failed durable intent crossed the placement boundary");
    }
}

void execution_proof_and_resume_generation() {
    {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup(); manager.submit_market(request());
        auto first=event("PARTIALLY_FILLED",.4,40,2000); first["t"]=1;
        manager.on_user_data_event(first);
        auto reused=event("PARTIALLY_FILLED",.8,80,3000); reused["t"]=1; reused["l"]="0.4";
        manager.on_user_data_event(reused);
        const auto result=manager.get("owned").value();
        require(result.executed_quantity==.8 && result.execution_trade_quantities.size()==1 &&
                result.requires_reconciliation && manager.kill_switch_active(),
                "Reused trade identity proved a second cumulative execution");
    }
    {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup(); manager.submit_market(request());
        auto missing=event("FILLED",1,100); missing.erase("n");missing.erase("N");missing.erase("l");missing.erase("t");
        manager.on_user_data_event(missing);
        require(manager.get("owned")->requires_reconciliation,"Missing trade/fee evidence was assumed complete");
        manager.on_user_data_event(event("FILLED",1,100,3000));
        bool blocked=false;try{manager.clear_kill_switch_for_reconciled_resume();}catch(const std::logic_error&){blocked=true;}
        require(blocked && manager.get("owned")->requires_reconciliation,
                "Late partial evidence automatically cleared a full reconciliation requirement");
    }
    {
        Exchange exchange;
        OrderManager manager(exchange); manager.reconcile_startup();
        auto large=request();large.quantity=20000;manager.submit_market(large);
        auto first=event("PARTIALLY_FILLED",10000,1000000,2000);
        first["q"]=20000.0;first["t"]=1;first["l"]=10000.0;
        manager.on_user_data_event(first);
        auto tiny=event("PARTIALLY_FILLED",10000,1000000,3000);
        tiny["q"]=20000.0;tiny["z"]=10000.00000001;tiny["Z"]=1000000.000001;tiny["l"]=.00000001;tiny["t"]=2;
        manager.on_user_data_event(tiny);
        const auto result=manager.get("owned").value();
        require(result.executed_quantity>10000 && result.cumulative_quote_quantity>1000000 &&
                result.execution_trade_quantities.size()==2 && !result.requires_reconciliation,
                "A representable tiny execution was lost to a relative comparison tolerance");
    }
    {
        Exchange exchange;
        OrderManager manager(exchange);manager.reconcile_startup();
        manager.activate_kill_switch("First kill");
        const auto before=manager.kill_generation();
        manager.reconcile_startup();
        manager.activate_kill_switch("Newer operator kill");
        bool blocked=false;
        try{manager.clear_kill_switch_for_reconciled_resume(before);}catch(const std::logic_error&){blocked=true;}
        require(blocked && manager.kill_switch_active(),"In-flight reconciled resume cleared a newer kill request");
        manager.clear_kill_switch_for_reconciled_resume(manager.kill_generation());
        require(!manager.kill_switch_active(),"Current-generation explicit resume failed without unresolved state");
    }
}

void response_contracts_and_observer_failure() {
    const auto classify = [](long status,const std::string& body) {
        try { (void)BinanceSpotExecutionClient::decode_response(status,body); }
        catch (const BinanceRequestError& error) { return error.kind(); }
        throw std::runtime_error("Expected classified request failure");
    };
    require(classify(504,"{\"code\":-1007}")==BinanceRequestError::Kind::Unknown,"Timeout became explicit rejection");
    require(classify(500,"{\"code\":-2010}")==BinanceRequestError::Kind::Unknown,"HTTP5xx became explicit rejection");
    require(classify(400,"{\"code\":-1006}")==BinanceRequestError::Kind::Unknown,"Unknown execution code became rejection");
    require(classify(400,"{\"code\":-2010}")==BinanceRequestError::Kind::Rejected,"Matching engine rejection became unknown");
    require(classify(400,"{\"code\":-2013}")==BinanceRequestError::Kind::NotFound,"Order NotFound lost distinction");
    require(classify(200,"not json")==BinanceRequestError::Kind::Unknown,"Malformed success became rejection");
    require(classify(400,"{\"code\":4294965286}")==BinanceRequestError::Kind::Unknown,
            "Oversized unsigned error code narrowed into a definitive rejection");
    require(classify(400,"{\"code\":-4294969306}")==BinanceRequestError::Kind::Unknown,
            "Oversized signed error code narrowed into a definitive rejection");
    Exchange exchange;
    OrderManager manager(exchange);manager.reconcile_startup();
    manager.set_update_handler([](const Snapshot& update){if(update.state==State::Acknowledged)throw std::runtime_error("Observer failed");});
    bool failed=false;
    try{manager.submit_market(request());}catch(const std::runtime_error&){failed=true;}
    require(failed && manager.get("owned")->state==State::Acknowledged,"Observer failure was classified as exchange rejection");
    for (const double quantity : {0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid=request("invalid");invalid.quantity=quantity;
        bool rejected=false;try{manager.submit_market(invalid);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"Invalid quantity reached placement boundary");
    }
    auto invalid=request("invalid-side");invalid.side=static_cast<Side>(77);
    bool rejected=false;try{manager.submit_market(invalid);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected && exchange.placements==1,"Invalid side reached placement boundary");
}
}

int main() {
    try {
        monotonic_and_fee_reports();
        pending_observer_cancellation();
        concurrent_placement_reports_and_kill();
        explicit_rejection_and_unknown_outcomes();
        cancellation_aliases_and_races();
        malformed_and_conflicting_evidence();
        durable_unknown_restart_and_publication();
        durable_pending_crash_and_storage_failure();
        execution_proof_and_resume_generation();
        response_contracts_and_observer_failure();
        std::cout << "order_state_integrity: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "order_state_integrity: " << error.what() << '\n';
        return 1;
    }
}
