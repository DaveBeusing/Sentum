#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>
#include <sentum/trader/order/ConfirmedPositionLedger.hpp>

namespace sentum::execution {

struct ExecutionRecoveryState {
    std::string symbol;
    std::vector<order::Snapshot> orders;
    std::vector<order::Snapshot> replay;
    std::optional<order::ConfirmedPosition> position;
    std::chrono::system_clock::time_point entry_time{};
    std::chrono::system_clock::time_point last_exit{};
};

} // namespace sentum::execution
