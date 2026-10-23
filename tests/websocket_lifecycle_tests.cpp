#include <sentum/api/BinanceUserDataStream.hpp>
#include <sentum/api/BinanceWebsocketClient.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct WebsocketLifecycleTestAccess {
    static void before_connect(BinanceWebsocketClient& client, std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(client.lifecycle_mutex);
        if (client.joining || client.ws_thread.joinable()) throw std::logic_error("worker must be joined");
        client.before_connect = std::move(callback);
    }

    static void before_connect(BinanceUserDataStream& client, std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(client.lifecycle_mutex_);
        if (client.joining_ || client.thread_.joinable()) throw std::logic_error("worker must be joined");
        client.before_connect_ = std::move(callback);
    }

    static bool running(const BinanceWebsocketClient& client) { return client.running.load(); }
    static bool running(const BinanceUserDataStream& client) { return client.running(); }

    static bool joining(BinanceWebsocketClient& client) {
        std::lock_guard<std::mutex> lock(client.lifecycle_mutex);
        return client.joining;
    }

    static bool joining(BinanceUserDataStream& client) {
        std::lock_guard<std::mutex> lock(client.lifecycle_mutex_);
        return client.joining_;
    }

    static bool released(BinanceWebsocketClient& client) {
        std::lock_guard<std::mutex> lock(client.lifecycle_mutex);
        return !client.joining && !client.ws_thread.joinable() && !client.impl && !client.running.load();
    }

    static bool released(BinanceUserDataStream& client) {
        std::lock_guard<std::mutex> lock(client.lifecycle_mutex_);
        return !client.joining_ && !client.thread_.joinable() && !client.impl_ && !client.running();
    }

    static void emit(BinanceWebsocketClient& client) { client.handle_message(R"({"p":"100"})", false); }
    static void emit(BinanceUserDataStream& client) { client.handle_message(R"({"e":"executionReport"})"); }
};

namespace {
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
bool wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

class Gate {
public:
    ~Gate() { release(); }

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        arrived_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
    }

    bool wait_for_arrival() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 3s, [this] { return arrived_; });
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool arrived_ = false;
    bool released_ = false;
};

struct MarketFixture {
    BinanceWebsocketClient client{"BTCUSDT"};
    void on_message(std::function<void()> callback) {
        client.set_on_price([callback = std::move(callback)](double price) {
            require(price == 100.0, "price callback received an unexpected payload");
            callback();
        });
    }
};

struct UserFixture {
    std::function<void()> callback;
    BinanceUserDataStream client{"lifecycle-fixture", [this](const nlohmann::json& event) {
        require(event.at("e") == "executionReport", "user callback received an unexpected payload");
        if (callback) callback();
    }};
    void on_message(std::function<void()> value) { callback = std::move(value); }
};

template <typename Fixture>
void stop_without_start() {
    Fixture fixture;
    fixture.client.stop();
    fixture.client.stop();
    require(WebsocketLifecycleTestAccess::released(fixture.client), "stop-before-start retained resources");
    // Destruction of both this stopped object and a never-started object must be safe.
    Fixture never_started;
}

template <typename Fixture>
void cancel_before_worker_preparation() {
    Fixture fixture;
    Gate preparation;
    WebsocketLifecycleTestAccess::before_connect(fixture.client, [&] { preparation.arrive_and_wait(); });
    fixture.client.start();
    const bool arrived = preparation.wait_for_arrival();
    std::thread stopper([&] { fixture.client.stop(); });
    const bool joining = wait_for([&] { return WebsocketLifecycleTestAccess::joining(fixture.client); });
    const bool cancelled = !WebsocketLifecycleTestAccess::running(fixture.client);
    preparation.release();
    stopper.join();
    require(arrived && joining && cancelled, "early stop did not cancel the paused worker before joining");
    require(WebsocketLifecycleTestAccess::released(fixture.client), "late worker preparation undid cancellation");
}

template <typename Fixture>
void destruction_after_start() {
    std::atomic<bool> worker_returned{false};
    {
        Fixture fixture;
        WebsocketLifecycleTestAccess::before_connect(fixture.client, [&] {
            fixture.client.stop();
            worker_returned.store(true);
        });
        fixture.client.start();
    }
    require(worker_returned.load(), "destruction returned before its worker finished");
}

template <typename Fixture>
void startup_failure_and_restart() {
    Fixture fixture;
    std::atomic<int> attempts{0};
    WebsocketLifecycleTestAccess::before_connect(fixture.client, [&] {
        attempts.fetch_add(1);
        throw std::runtime_error("injected startup preparation failure");
    });
    for (int i = 1; i <= 2; ++i) {
        fixture.client.start();
        require(wait_for([&] { return !WebsocketLifecycleTestAccess::running(fixture.client); }),
                "startup failure left the client running");
        require(attempts.load() == i, "restart did not prepare a fresh worker after a failed startup");
    }
    fixture.client.stop();
    fixture.client.stop();
    require(WebsocketLifecycleTestAccess::released(fixture.client), "failed startup retained resources");

    std::atomic<int> callbacks{0};
    fixture.on_message([&] { callbacks.fetch_add(1); fixture.client.stop(); });
    WebsocketLifecycleTestAccess::before_connect(fixture.client, [&] {
        WebsocketLifecycleTestAccess::emit(fixture.client);
        fixture.client.stop();
    });
    fixture.client.start();
    require(wait_for([&] { return callbacks.load() == 1; }), "restart after failure did not deliver a callback");
    fixture.client.stop();
    require(WebsocketLifecycleTestAccess::released(fixture.client), "successful restart did not stop cleanly");
}

template <typename Fixture>
void callback_self_stop_and_owner_join() {
    Fixture fixture;
    std::atomic<int> callbacks{0};
    std::atomic<bool> callback_returned{false};
    fixture.on_message([&] {
        fixture.client.stop();
        callbacks.fetch_add(1);
        callback_returned.store(true);
    });
    WebsocketLifecycleTestAccess::before_connect(fixture.client, [&] {
        WebsocketLifecycleTestAccess::emit(fixture.client);
        fixture.client.stop();
    });
    fixture.client.start();
    require(wait_for([&] { return callback_returned.load(); }), "callback stop attempted to join itself");
    require(callbacks.load() == 1 && !WebsocketLifecycleTestAccess::running(fixture.client),
            "callback stop did not cancel its worker");
    fixture.client.stop();
    require(WebsocketLifecycleTestAccess::released(fixture.client), "owner did not join the self-stopped worker");
}

template <typename Fixture>
void callback_stop_during_owner_join() {
    Fixture fixture;
    Gate callback;
    std::atomic<bool> callback_returned{false};
    fixture.on_message([&] {
        callback.arrive_and_wait();
        fixture.client.stop();
        callback_returned.store(true);
    });
    WebsocketLifecycleTestAccess::before_connect(fixture.client, [&] {
        WebsocketLifecycleTestAccess::emit(fixture.client);
        fixture.client.stop();
    });
    fixture.client.start();
    const bool arrived = callback.wait_for_arrival();
    std::thread stopper([&] { fixture.client.stop(); });
    const bool joining = wait_for([&] { return WebsocketLifecycleTestAccess::joining(fixture.client); });
    callback.release();
    stopper.join();
    require(arrived && joining && callback_returned.load(), "owner join blocked callback cancellation");
    require(WebsocketLifecycleTestAccess::released(fixture.client), "concurrent callback stop retained resources");
}

template <typename Fixture>
void immediate_and_concurrent_stop() {
    Fixture fixture;
    WebsocketLifecycleTestAccess::before_connect(fixture.client, [&] { fixture.client.stop(); });
    for (int i = 0; i < 64; ++i) {
        fixture.client.start();
        fixture.client.stop();
        require(WebsocketLifecycleTestAccess::released(fixture.client), "immediate stop retained a worker");
    }

    std::vector<std::thread> callers;
    for (int i = 0; i < 4; ++i) {
        callers.emplace_back([&] {
            for (int iteration = 0; iteration < 16; ++iteration) {
                fixture.client.start();
                fixture.client.stop();
            }
        });
    }
    for (auto& caller : callers) caller.join();
    fixture.client.stop();
    require(WebsocketLifecycleTestAccess::released(fixture.client), "concurrent lifecycle retained a worker");
}

template <typename Fixture>
void verify_client_lifecycle() {
    stop_without_start<Fixture>();
    cancel_before_worker_preparation<Fixture>();
    destruction_after_start<Fixture>();
    startup_failure_and_restart<Fixture>();
    callback_self_stop_and_owner_join<Fixture>();
    callback_stop_during_owner_join<Fixture>();
    immediate_and_concurrent_stop<Fixture>();
}
} // namespace

int main() {
    try {
        verify_client_lifecycle<MarketFixture>();
        verify_client_lifecycle<UserFixture>();
        std::cout << "WebSocket lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WebSocket lifecycle failure: " << error.what() << '\n';
        return 1;
    }
}
