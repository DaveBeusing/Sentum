#include <sentum/api/ReconnectingMarketStream.hpp>
#include <sentum/api/TlsClientContext.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <boost/asio/steady_timer.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

namespace sentum::api {
namespace {
using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
using SteadyClock = std::chrono::steady_clock;

std::int64_t steady_milliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count();
}
}

struct ReconnectingMarketStream::Impl {
    struct Attempt {
        Client client;
        std::unique_ptr<boost::asio::steady_timer> watchdog;
        SteadyClock::time_point started;
        SteadyClock::time_point opened;
        std::atomic<bool> ended{false};

        Attempt() {
            client.init_asio();
            client.clear_access_channels(websocketpp::log::alevel::all);
            client.clear_error_channels(websocketpp::log::elevel::all);
            watchdog = std::make_unique<boost::asio::steady_timer>(client.get_io_service());
        }

        void stop() noexcept { client.stop(); }
    };

    std::string url;
    std::string hostname;
    MarketStreamSettings settings;
    MessageHandler message_handler;
    HealthHandler health_handler;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::shared_ptr<Attempt> current;
    bool cancelled = false;
    std::atomic<bool> worker_running{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> connection_has_event{false};
    std::atomic<bool> backing_off{false};
    std::atomic<std::int64_t> last_event_received{0};
    std::atomic<std::int64_t> last_event_time{0};
    std::atomic<std::int64_t> last_event_initial_age{0};
    std::atomic<std::uint64_t> attempts{0};
    std::atomic<std::uint64_t> connections{0};

    Impl(std::string url_, std::string hostname_, MarketStreamSettings settings_,
         MessageHandler message_, HealthHandler health_)
        : url(std::move(url_)), hostname(std::move(hostname_)), settings(settings_),
          message_handler(std::move(message_)), health_handler(std::move(health_)) {
        if (url.rfind("wss://", 0) != 0 || hostname.empty() || !message_handler ||
            settings.initial_backoff.count() <= 0 || settings.maximum_backoff < settings.initial_backoff ||
            settings.connect_timeout.count() <= 0 || settings.maximum_event_age.count() <= 0 ||
            settings.maximum_event_age > std::chrono::hours(24) ||
            settings.idle_reconnect_timeout < settings.maximum_event_age) {
            throw std::invalid_argument("Invalid public market stream configuration");
        }
    }

    MarketStreamHealth health() const {
        MarketStreamHealth result;
        result.worker_running = worker_running.load(std::memory_order_acquire);
        result.connected = connected.load(std::memory_order_acquire);
        const auto received = last_event_received.load(std::memory_order_acquire);
        result.last_event_time_ms = last_event_time.load(std::memory_order_relaxed);
        if (received != 0) {
            result.last_event_age_ms = std::max<std::int64_t>(0, steady_milliseconds() - received) +
                last_event_initial_age.load(std::memory_order_relaxed);
        }
        result.fresh = result.worker_running && result.connected &&
            connection_has_event.load(std::memory_order_acquire) && result.last_event_age_ms >= 0 &&
            result.last_event_age_ms <= settings.maximum_event_age.count();
        result.connection_attempts = attempts.load(std::memory_order_relaxed);
        result.successful_connections = connections.load(std::memory_order_relaxed);
        result.reconnects = result.successful_connections > 0 ? result.successful_connections - 1 : 0;
        result.state = !result.worker_running ? "stopped" : result.connected ?
            (result.fresh ? "live" : connection_has_event.load() ? "stale" : "waiting_for_data") :
            backing_off.load() ? "backoff" : "connecting";
        return result;
    }

    void report_health() noexcept {
        if (!health_handler) return;
        // Observability must not abort the transport or its owning runtime.
        try { health_handler(health()); } catch (...) {}
    }

    void end_attempt(const std::shared_ptr<Attempt>& attempt) {
        if (attempt->ended.exchange(true)) return;
        connected.store(false, std::memory_order_release);
        attempt->stop();
        report_health();
    }

    void watch(const std::shared_ptr<Attempt>& attempt) {
        const auto interval = std::min(std::chrono::milliseconds(250), settings.maximum_event_age);
        attempt->watchdog->expires_after(interval);
        attempt->watchdog->async_wait([this, weak = std::weak_ptr<Attempt>(attempt)](const boost::system::error_code& error) {
            const auto attempt = weak.lock();
            if (error || !attempt || attempt->ended.load()) return;
            const auto now = SteadyClock::now();
            const bool is_connected = connected.load(std::memory_order_acquire);
            const auto received = last_event_received.load(std::memory_order_acquire);
            const auto idle = connection_has_event.load(std::memory_order_acquire) && received != 0
                ? std::chrono::milliseconds(std::max<std::int64_t>(0, steady_milliseconds() - received))
                : std::chrono::duration_cast<std::chrono::milliseconds>(now - attempt->opened);
            if ((!is_connected && now - attempt->started >= settings.connect_timeout) ||
                (is_connected && idle >= settings.idle_reconnect_timeout)) {
                end_attempt(attempt);
                return;
            }
            report_health();
            watch(attempt);
        });
    }

    void run() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (cancelled) return;
            worker_running.store(true, std::memory_order_release);
        }
        auto backoff = settings.initial_backoff;
        for (;;) {
            std::shared_ptr<Attempt> attempt;
            try {
                attempt = std::make_shared<Attempt>();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (cancelled) break;
                    current = attempt;
                }
                connection_has_event.store(false, std::memory_order_release);
                backing_off.store(false, std::memory_order_release);
                attempts.fetch_add(1, std::memory_order_relaxed);
                attempt->started = SteadyClock::now();
                attempt->opened = attempt->started;
                attempt->client.set_open_handshake_timeout(settings.connect_timeout.count());
                attempt->client.set_tls_init_handler([this](websocketpp::connection_hdl) {
                    return make_tls_client_context(hostname);
                });
                const auto weak = std::weak_ptr<Attempt>(attempt);
                attempt->client.set_open_handler([this, weak](websocketpp::connection_hdl) {
                    const auto attempt = weak.lock();
                    if (!attempt) return;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (cancelled) { attempt->stop(); return; }
                        attempt->opened = SteadyClock::now();
                        connected.store(true, std::memory_order_release);
                        connections.fetch_add(1, std::memory_order_relaxed);
                    }
                    report_health();
                });
                attempt->client.set_close_handler([this, weak](websocketpp::connection_hdl) {
                    if (const auto attempt = weak.lock()) end_attempt(attempt);
                });
                attempt->client.set_fail_handler([this, weak](websocketpp::connection_hdl) {
                    if (const auto attempt = weak.lock()) end_attempt(attempt);
                });
                attempt->client.set_message_handler([this](websocketpp::connection_hdl, Client::message_ptr message) {
                    message_handler(message->get_payload());
                });
                websocketpp::lib::error_code error;
                auto connection = attempt->client.get_connection(url, error);
                if (error) throw std::runtime_error("Cannot create market stream connection: " + error.message());
                attempt->client.connect(connection);
                watch(attempt);
                report_health();
                attempt->client.run();
            } catch (...) {
                // Failed connection attempts are retried; cancellation remains
                // authoritative and no price is fabricated from a failure.
            }
            const bool had_valid_event = connection_has_event.load(std::memory_order_acquire);
            connected.store(false, std::memory_order_release);
            connection_has_event.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(mutex);
                current.reset();
            }
            // Release the stopped endpoint and its handlers before waiting or
            // making another attempt. No work survives into the next endpoint.
            if (attempt) {
                attempt->client.set_open_handler({});
                attempt->client.set_close_handler({});
                attempt->client.set_fail_handler({});
                attempt->client.set_message_handler({});
                attempt->watchdog->cancel();
            }
            attempt.reset();
            if (had_valid_event) backoff = settings.initial_backoff;
            backing_off.store(true, std::memory_order_release);
            report_health();
            std::unique_lock<std::mutex> lock(mutex);
            if (wake.wait_for(lock, backoff, [this] { return cancelled; })) break;
            backoff = backoff >= settings.maximum_backoff / 2
                ? settings.maximum_backoff : std::min(settings.maximum_backoff, backoff * 2);
        }
        connected.store(false, std::memory_order_release);
        connection_has_event.store(false, std::memory_order_release);
        worker_running.store(false, std::memory_order_release);
        backing_off.store(false, std::memory_order_release);
        report_health();
    }

    void stop() noexcept {
        std::shared_ptr<Attempt> attempt;
        {
            std::lock_guard<std::mutex> lock(mutex);
            cancelled = true;
            attempt = current;
            connected.store(false, std::memory_order_release);
        }
        wake.notify_all();
        if (attempt) attempt->stop();
    }
};

ReconnectingMarketStream::ReconnectingMarketStream(std::string url, std::string hostname,
    MarketStreamSettings settings, MessageHandler on_message, HealthHandler on_health)
    : impl_(std::make_unique<Impl>(std::move(url), std::move(hostname), settings,
                                  std::move(on_message), std::move(on_health))) {}

ReconnectingMarketStream::~ReconnectingMarketStream() { stop(); }
void ReconnectingMarketStream::run() { impl_->run(); }
void ReconnectingMarketStream::stop() noexcept { impl_->stop(); }
MarketStreamHealth ReconnectingMarketStream::health() const { return impl_->health(); }

bool ReconnectingMarketStream::accept_event(std::int64_t event_time_ms) noexcept {
    if (!impl_->connected.load(std::memory_order_acquire)) return false;
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // Check the integer epoch before converting milliseconds to the platform's
    // finer system_clock duration. Untrusted extreme epochs must not overflow.
    if (event_time_ms <= 0 || event_time_ms > now_ms + 1000) return false;
    const auto age_ms = now_ms - event_time_ms;
    if (age_ms > impl_->settings.maximum_event_age.count()) return false;
    impl_->last_event_time.store(event_time_ms, std::memory_order_relaxed);
    impl_->last_event_initial_age.store(std::max<std::int64_t>(0, age_ms), std::memory_order_relaxed);
    impl_->last_event_received.store(steady_milliseconds(), std::memory_order_release);
    impl_->connection_has_event.store(true, std::memory_order_release);
    return true;
}

} // namespace sentum::api
