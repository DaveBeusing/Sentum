#pragma once

#include <functional>
#include <string>

#include <sentum/trader/order/OrderTypes.hpp>

namespace sentum::execution {

class IExecutionVenue {
public:
    using UpdateHandler = std::function<void(const order::Snapshot&)>;
    virtual ~IExecutionVenue() = default;
    virtual void start(UpdateHandler handler) = 0;
    virtual void stop() noexcept = 0;
    virtual order::Snapshot submit(const order::Request& request) = 0;
    virtual order::Snapshot cancel(const std::string& client_order_id) = 0;
    virtual void kill() noexcept = 0;
    virtual bool ready() const noexcept = 0;
    virtual bool killed() const noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

} // namespace sentum::execution
