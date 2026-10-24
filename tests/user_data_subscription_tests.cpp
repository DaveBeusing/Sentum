#include <sentum/api/BinanceUserDataStream.hpp>
#include <sentum/api/UserDataSubscriptionProtocol.hpp>

#include "support/LocalTlsMarketServer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct UserDataStreamTestAccess {
    static void configure(BinanceUserDataStream& stream, const std::string& endpoint,
                          std::chrono::milliseconds connect = std::chrono::seconds(2),
                          std::chrono::milliseconds subscribe = std::chrono::seconds(2),
                          std::chrono::milliseconds activity = std::chrono::seconds(60)) {
        std::lock_guard<std::mutex> lock(stream.lifecycle_mutex_);
        if (stream.thread_.joinable()) throw std::logic_error("Stream must be joined before configuration");
        stream.endpoint_url_ = endpoint;
        stream.connection_timeout_ = connect;
        stream.subscription_timeout_ = subscribe;
        stream.activity_timeout_ = activity;
    }

    static void late_message(BinanceUserDataStream& stream, const nlohmann::json& message) {
        stream.handle_message(message.dump());
    }
};

namespace {
using namespace std::chrono_literals;
using sentum::test::eventually;
using sentum::test::require;
using Json = nlohmann::json;
using Protocol = sentum::api::UserDataSubscriptionProtocol;

Json acknowledgement(const std::string& id, std::int64_t subscription = 7) {
    return {{"id", id}, {"status", 200}, {"result", {{"subscriptionId", subscription}}}};
}

Json envelope(const std::string& type, std::int64_t subscription = 7) {
    return {{"subscriptionId", subscription}, {"event", {{"e", type}, {"E", 1700000000000LL}}}};
}

Json subscription_request(const std::string& id) {
    return {{"id", id}, {"method", "userDataStream.subscribe.signature"},
            {"params", {{"apiKey", "transport-fixture"}, {"timestamp", 1700000000000LL},
                        {"signature", "fixture-signature"}}}};
}

void protocol_contract() {
    Protocol protocol;
    protocol.begin("subscription-1");
    require(!protocol.healthy(), "Request preparation authenticated a subscription");
    require(protocol.receive(acknowledgement("subscription-1")) == Protocol::Result::Acknowledged &&
            protocol.healthy(), "Correlated subscription acknowledgement was rejected");
    require(protocol.receive(envelope("executionReport")) == Protocol::Result::Event,
            "Authenticated event envelope was rejected");
    require(protocol.receive(envelope("eventStreamTerminated")) == Protocol::Result::Failed &&
            !protocol.healthy(), "Terminated subscription remained authenticated");
    require(protocol.receive(acknowledgement("subscription-1")) == Protocol::Result::Ignored &&
            !protocol.healthy(), "Late acknowledgement resurrected a terminated subscription");
    protocol.begin("subscription-2");
    require(!protocol.healthy(), "Restart inherited previous subscription health");
    require(protocol.receive(acknowledgement("subscription-1")) == Protocol::Result::Failed,
            "Previous connection acknowledgement authenticated a new connection");
    protocol.begin("subscription-3");
    require(protocol.receive(acknowledgement("subscription-3", 0)) == Protocol::Result::Acknowledged,
            "Valid zero subscription id was rejected");
    require(protocol.receive(envelope("balanceUpdate", 0)) == Protocol::Result::Event,
            "Zero subscription id was not correlated");
    protocol.fail();
    require(protocol.receive(acknowledgement("subscription-3", 0)) == Protocol::Result::Ignored,
            "Late acknowledgement resurrected a timed-out connection");

    const std::vector<Json> invalid_acknowledgements = {
        Json::array(), Json{}, {{"id", "wrong"}, {"status", 200}, {"result", {{"subscriptionId", 7}}}},
        {{"id", "request"}, {"status", 401}, {"error", {{"code", -2015}}}},
        {{"id", "request"}, {"status", 200.0}, {"result", {{"subscriptionId", 7}}}},
        {{"id", "request"}, {"status", 200}, {"result", Json::object()}},
        {{"id", "request"}, {"status", 200}, {"result", {{"subscriptionId", -1}}}},
        {{"id", "request"}, {"status", 200}, {"result", {{"subscriptionId", "7"}}}},
        {{"id", "request"}, {"status", 200}, {"result", {{"subscriptionId", true}}}},
        {{"id", "request"}, {"status", 200}, {"result", {{"subscriptionId", 7.1}}}},
        {{"id", "request"}, {"status", 200},
         {"result", {{"subscriptionId", std::numeric_limits<std::uint64_t>::max()}}}},
        envelope("executionReport")
    };
    for (const auto& message : invalid_acknowledgements) {
        protocol.begin("request");
        require(protocol.receive(message) == Protocol::Result::Failed && !protocol.healthy(),
                "Invalid acknowledgement or unauthenticated event was accepted");
    }

    const std::vector<Json> invalid_events = {
        envelope("executionReport", 8), {{"e", "executionReport"}},
        {{"subscriptionId", 7}, {"event", {{"e", false}}}},
        {{"subscriptionId", "7"}, {"event", {{"e", "executionReport"}}}},
        {{"event", {{"e", "executionReport"}}}},
        {{"event", {{"e", "serverShutdown"}}}},
        acknowledgement("request")
    };
    for (const auto& message : invalid_events) {
        protocol.begin("request");
        protocol.receive(acknowledgement("request"));
        require(protocol.receive(message) == Protocol::Result::Failed && !protocol.healthy(),
                "Malformed, duplicate or mismatched subscription message remained healthy");
    }
}

class SubscriptionServer {
public:
    enum class Reply { Accept, Reject, Ignore };

    explicit SubscriptionServer(const sentum::test::LocalTlsIdentity& identity) {
        server_.init_asio();
        server_.clear_access_channels(websocketpp::log::alevel::all);
        server_.clear_error_channels(websocketpp::log::elevel::all);
        server_.set_tls_init_handler([&identity](websocketpp::connection_hdl) { return identity.context(); });
        server_.set_open_handler([this](websocketpp::connection_hdl handle) { clients_.insert(handle); });
        server_.set_close_handler([this](websocketpp::connection_hdl handle) { clients_.erase(handle); });
        server_.set_fail_handler([this](websocketpp::connection_hdl handle) { clients_.erase(handle); });
        server_.set_pong_handler([this](websocketpp::connection_hdl, const std::string& payload) {
            if (payload == "subscription-health") pongs_.fetch_add(1);
            else failed_.store(true);
        });
        server_.set_message_handler([this](websocketpp::connection_hdl handle, Server::message_ptr message) {
            try {
                const auto request = Json::parse(message->get_payload());
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    requests_.push_back(request);
                }
                const auto mode = reply_.load();
                if (mode == Reply::Ignore) return;
                const auto response = mode == Reply::Accept ? acknowledgement(request.at("id").get<std::string>()) :
                    Json{{"id", request.at("id")}, {"status", 401}, {"error", {{"code", -2015}}}};
                websocketpp::lib::error_code error;
                server_.send(handle, response.dump(), websocketpp::frame::opcode::text, error);
                if (error) failed_.store(true);
            } catch (...) { failed_.store(true); }
        });
        server_.listen(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
        boost::system::error_code error;
        port_ = server_.get_local_endpoint(error).port();
        require(!error, "Cannot determine subscription fixture listener port");
        server_.start_accept();
        thread_ = std::thread([this] { server_.run(); });
    }

    ~SubscriptionServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    std::string url() const { return "wss://localhost:" + std::to_string(port_) + "/ws-api/v3"; }
    void reply(Reply value) { reply_.store(value); }
    bool failed() const { return failed_.load(); }
    int pongs() const { return pongs_.load(); }
    std::size_t requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }
    Json request(std::size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.at(index);
    }
    void send(const Json& message) { send_text(message.dump()); }
    void ping() {
        server_.get_io_service().post([this] {
            for (const auto& handle : clients_) {
                websocketpp::lib::error_code error;
                server_.ping(handle, "subscription-health", error);
                if (error) failed_.store(true);
            }
        });
    }
    void send_text(const std::string& payload) {
        server_.get_io_service().post([this, payload] {
            for (const auto& handle : clients_) {
                websocketpp::lib::error_code error;
                server_.send(handle, payload, websocketpp::frame::opcode::text, error);
            }
        });
    }
    void disconnect() {
        server_.get_io_service().post([this] {
            for (const auto& handle : clients_) {
                websocketpp::lib::error_code error;
                auto connection = server_.get_con_from_hdl(handle, error);
                if (!error) {
                    boost::system::error_code ignored;
                    connection->get_raw_socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
                    connection->get_raw_socket().close(ignored);
                }
            }
            clients_.clear();
        });
    }

private:
    using Server = websocketpp::server<websocketpp::config::asio_tls>;
    Server server_;
    std::thread thread_;
    std::uint16_t port_ = 0;
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> clients_;
    mutable std::mutex mutex_;
    std::vector<Json> requests_;
    std::atomic<Reply> reply_{Reply::Accept};
    std::atomic<bool> failed_{false};
    std::atomic<int> pongs_{0};
};

void verified_transport_and_reconnect(const sentum::test::LocalTlsIdentity& identity) {
    SubscriptionServer server(identity);
    std::atomic<int> events{0};
    std::atomic<int> unhealthy{0};
    std::atomic<bool> correctly_unwrapped{false};
    BinanceUserDataStream stream(subscription_request, [&](const Json& event) {
        correctly_unwrapped.store(event.at("e") == "executionReport" && !event.contains("subscriptionId"));
        events.fetch_add(1);
    }, [&](bool healthy) { if (!healthy) unhealthy.fetch_add(1); });
    UserDataStreamTestAccess::configure(stream, server.url());
    stream.start();
    require(stream.wait_until_healthy(3s), "Verified TLS subscription did not become healthy after acknowledgement");
    require(server.requests() == 1 && !server.failed(), "Subscription request was not sent exactly once");
    const auto first_request = server.request(0);
    require(first_request.at("method") == "userDataStream.subscribe.signature" && first_request.contains("params"),
            "Transport did not send the authenticated subscription method");
    server.send(envelope("executionReport"));
    eventually([&] { return events.load() == 1; }, "Authenticated event was not delivered");
    require(correctly_unwrapped.load(), "Event envelope was not unwrapped before order processing");
    server.disconnect();
    eventually([&] { return !stream.healthy() && !stream.running() && unhealthy.load() == 1; },
               "Disconnected stream remained authenticated or did not notify its owner");
    require(server.requests() == 1, "Stream reconnected without owner reconciliation");

    server.reply(SubscriptionServer::Reply::Ignore);
    stream.start();
    eventually([&] { return server.requests() == 2; }, "Explicit restart did not send a fresh subscription");
    require(stream.running() && !stream.healthy(), "New connection inherited authenticated readiness");
    const auto second_request = server.request(1);
    require(first_request.at("id") != second_request.at("id"), "Restart reused a prior request correlation id");
    server.send(acknowledgement(second_request.at("id").get<std::string>()));
    require(stream.wait_until_healthy(3s), "Fresh reconnect acknowledgement did not restore stream health");
    stream.stop();
    require(!stream.running() && !stream.healthy(), "Stopped stream retained authenticated readiness");
}

void rejection_timeout_and_late_ack(const sentum::test::LocalTlsIdentity& identity) {
    for (const auto mode : {SubscriptionServer::Reply::Reject, SubscriptionServer::Reply::Ignore}) {
        SubscriptionServer server(identity);
        server.reply(mode);
        std::atomic<int> unhealthy{0};
        BinanceUserDataStream stream(subscription_request, [](const Json&) {},
                                     [&](bool healthy) { if (!healthy) unhealthy.fetch_add(1); });
        UserDataStreamTestAccess::configure(stream, server.url(), 2s, 75ms);
        stream.start();
        require(!stream.wait_until_healthy(3s), "Rejected or missing subscription acknowledgement became healthy");
        eventually([&] { return !stream.running() && unhealthy.load() == 1; },
                   "Subscription rejection/timeout did not stop and notify");
        require(server.requests() == 1, "Failed subscription request was silently retried");
        UserDataStreamTestAccess::late_message(stream, acknowledgement(server.request(0).at("id").get<std::string>()));
        require(!stream.healthy(), "Late acknowledgement resurrected a rejected or timed-out subscription");
        stream.stop();
    }
}

void termination_and_malformed_messages(const sentum::test::LocalTlsIdentity& identity) {
    const std::vector<std::string> messages = {
        envelope("eventStreamTerminated").dump(), Json{{"event", {{"e", "serverShutdown"}}}}.dump(),
        envelope("executionReport", 99).dump(), "{malformed", Json{{"e", "executionReport"}}.dump()
    };
    for (const auto& message : messages) {
        SubscriptionServer server(identity);
        std::atomic<int> events{0};
        std::atomic<bool> stopped_in_callback{false};
        BinanceUserDataStream* current = nullptr;
        BinanceUserDataStream stream(subscription_request, [&](const Json&) { events.fetch_add(1); },
            [&](bool healthy) {
                if (!healthy) {
                    require(!current->healthy(), "Unhealthy notification preceded the health latch");
                    current->stop();
                    stopped_in_callback.store(true);
                }
            });
        current = &stream;
        UserDataStreamTestAccess::configure(stream, server.url());
        stream.start();
        require(stream.wait_until_healthy(3s), "Subscription setup failed before failure-path test");
        server.send_text(message);
        eventually([&] { return stopped_in_callback.load(); }, "Unhealthy callback could not stop its own worker");
        require(!stream.healthy() && !stream.running() && events.load() == 0,
                "Terminated or malformed envelope reached order processing");
        stream.stop();
    }
}

void connection_deadline_and_tls_verification(const sentum::test::LocalTlsIdentity& identity) {
    boost::asio::io_context context;
    boost::asio::ip::tcp::acceptor stalled(context,
        boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    BinanceUserDataStream stream(subscription_request, [](const Json&) {});
    UserDataStreamTestAccess::configure(stream,
        "wss://localhost:" + std::to_string(stalled.local_endpoint().port()) + "/ws-api/v3", 75ms, 75ms);
    const auto started = std::chrono::steady_clock::now();
    stream.start();
    require(!stream.wait_until_healthy(3s), "Stalled TLS connection became authenticated");
    eventually([&] { return !stream.running(); }, "Connection timeout left a worker running");
    require(std::chrono::steady_clock::now() - started < 2s, "Connection timeout was not bounded");
    stream.stop();

    SubscriptionServer server(identity);
    auto wrong_hostname = server.url();
    wrong_hostname.replace(wrong_hostname.find("localhost"), std::string("localhost").size(), "127.0.0.1");
    UserDataStreamTestAccess::configure(stream, wrong_hostname);
    stream.start();
    require(!stream.wait_until_healthy(3s), "Mismatched TLS hostname authenticated a subscription");
    eventually([&] { return !stream.running(); }, "TLS verification failure left a worker running");
    require(server.requests() == 0, "Credentials were sent before TLS peer verification succeeded");
    stream.stop();
}

void authenticated_transport_activity_deadline(const sentum::test::LocalTlsIdentity& identity) {
    SubscriptionServer server(identity);
    std::atomic<int> unhealthy{0};
    BinanceUserDataStream stream(subscription_request, [](const Json&) {},
                                 [&](bool healthy) { if (!healthy) unhealthy.fetch_add(1); });
    UserDataStreamTestAccess::configure(stream, server.url(), 2s, 2s, 500ms);
    stream.start();
    require(stream.wait_until_healthy(3s), "Activity watchdog setup did not authenticate");
    // An idle account has no order events. Server ping frames must keep the
    // authenticated subscription healthy and receive matching automatic pongs.
    for (int ping = 1; ping <= 6; ++ping) {
        std::this_thread::sleep_for(100ms);
        server.ping();
        eventually([&] { return server.pongs() >= ping; }, "Subscription did not answer server ping");
        require(stream.healthy() && !server.failed(), "Idle account lost health despite transport activity");
    }
    eventually([&] { return !stream.healthy() && !stream.running() && unhealthy.load() == 1; },
               "Silent authenticated transport remained healthy indefinitely", 3s);
    stream.stop();
}

void cancellation_during_subscription_preparation(const sentum::test::LocalTlsIdentity& identity) {
    SubscriptionServer server(identity);
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    BinanceUserDataStream stream([&](const std::string& id) {
        entered.set_value();
        released.wait();
        return subscription_request(id);
    }, [](const Json&) {});
    UserDataStreamTestAccess::configure(stream, server.url());
    stream.start();
    const bool reached_signing = entered_future.wait_for(3s) == std::future_status::ready;
    std::thread stopper([&] { stream.stop(); });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (stream.running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool cancelled = !stream.running();
    release.set_value();
    stopper.join();
    require(reached_signing && cancelled && !stream.healthy(), "Cancellation did not invalidate pending subscription work");
    require(server.requests() == 0, "Cancelled subscription preparation sent a late authenticated request");
}

} // namespace

int main() {
    try {
        protocol_contract();
        sentum::test::LocalTlsIdentity identity;
        verified_transport_and_reconnect(identity);
        rejection_timeout_and_late_ack(identity);
        termination_and_malformed_messages(identity);
        connection_deadline_and_tls_verification(identity);
        authenticated_transport_activity_deadline(identity);
        cancellation_during_subscription_preparation(identity);
        std::cout << "User data subscription tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "User data subscription test failure: " << error.what() << '\n';
        return 1;
    }
}
