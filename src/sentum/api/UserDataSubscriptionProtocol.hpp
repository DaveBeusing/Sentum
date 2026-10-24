#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace sentum::api {

// One connection owns exactly one signed subscription. No event is authoritative
// until its correlated acknowledgement identifies that subscription.
class UserDataSubscriptionProtocol {
public:
    enum class Result { Ignored, Acknowledged, Event, Failed };

    void begin(std::string request_id) {
        request_id_ = std::move(request_id);
        subscription_id_ = -1;
        phase_ = request_id_.empty() ? Phase::Failed : Phase::AwaitingAcknowledgement;
    }

    void fail() noexcept { phase_ = Phase::Failed; }
    bool healthy() const noexcept { return phase_ == Phase::Subscribed; }

    Result receive(const nlohmann::json& message) {
        if (phase_ == Phase::Failed) return Result::Ignored;
        if (!message.is_object()) return reject();

        if (message.contains("id")) {
            if (phase_ != Phase::AwaitingAcknowledgement ||
                !message.at("id").is_string() || message.at("id") != request_id_ ||
                !message.contains("status") || !message.at("status").is_number_integer() ||
                message.at("status") != 200 || message.contains("error") ||
                !message.contains("result") || !message.at("result").is_object() ||
                !message.at("result").contains("subscriptionId") ||
                !subscription_id(message.at("result").at("subscriptionId"), subscription_id_)) {
                return reject();
            }
            phase_ = Phase::Subscribed;
            return Result::Acknowledged;
        }

        if (!message.contains("event") || !message.at("event").is_object() ||
            !message.at("event").contains("e") || !message.at("event").at("e").is_string()) {
            return reject();
        }
        const auto& type = message.at("event").at("e");
        // Connection shutdown events deliberately have no subscription id.
        if (type == "serverShutdown") return reject();

        std::int64_t incoming = -1;
        if (phase_ != Phase::Subscribed || !message.contains("subscriptionId") ||
            !subscription_id(message.at("subscriptionId"), incoming) || incoming != subscription_id_) {
            return reject();
        }
        if (type == "eventStreamTerminated") return reject();
        return Result::Event;
    }

private:
    enum class Phase { AwaitingAcknowledgement, Subscribed, Failed };

    static bool subscription_id(const nlohmann::json& value, std::int64_t& result) {
        if (!value.is_number_integer()) return false;
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
            result = static_cast<std::int64_t>(number);
        } else {
            result = value.get<std::int64_t>();
        }
        return result >= 0;
    }

    Result reject() noexcept {
        fail();
        return Result::Failed;
    }

    Phase phase_ = Phase::Failed;
    std::string request_id_;
    std::int64_t subscription_id_ = -1;
};

} // namespace sentum::api
