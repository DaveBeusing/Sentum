#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>
#include <sentum/api/BinanceSpotExecutionClient.hpp>
#include <sentum/operations/OperationalEventRepository.hpp>

namespace sentum::execution {

struct ReconciliationReport {
    bool success=false;
    int open_orders=0;
    int balances_checked=0;
    int inconsistencies=0;
    std::string details;
};

class AccountReconciler {
public:
    AccountReconciler(BinanceSpotExecutionClient& client, operations::OperationalEventRepository& events)
        : client_(client), events_(events) {}

    ReconciliationReport run(const std::string& symbol, double expected_base_quantity, const std::string& base_asset) {
        const auto started=now_ms(); ReconciliationReport r;
        try {
            const auto orders=client_.open_orders(); r.open_orders=orders.is_array()?static_cast<int>(orders.size()):0;
            const auto account=client_.account();
            double free=0.0,locked=0.0;
            if(account.contains("balances")) for(const auto& b:account.at("balances")){++r.balances_checked;if(b.value("asset",std::string{})==base_asset){free=std::stod(b.value("free",std::string{"0"}));locked=std::stod(b.value("locked",std::string{"0"}));}}
            const double exchange_qty=free+locked;
            const double tolerance=1e-8;
            if(std::abs(exchange_qty-expected_base_quantity)>tolerance){++r.inconsistencies;r.details="balance mismatch for "+base_asset+": local="+std::to_string(expected_base_quantity)+" exchange="+std::to_string(exchange_qty);}
            if(r.open_orders>1){++r.inconsistencies;if(!r.details.empty())r.details+="; ";r.details+="more than one open order for single-position runtime";}
            r.success=r.inconsistencies==0; if(r.details.empty())r.details="consistent";
        } catch(const std::exception& e){r.success=false;++r.inconsistencies;r.details=e.what();}
        events_.reconciliation(started,now_ms(),r.success,r.open_orders,r.balances_checked,r.inconsistencies,r.details);
        return r;
    }
private:
    static std::int64_t now_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    BinanceSpotExecutionClient& client_; operations::OperationalEventRepository& events_;
};

} // namespace sentum::execution
