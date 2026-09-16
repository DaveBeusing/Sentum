#include <sentum/collector/Collector.hpp>
#include <sentum/dashboard/DashboardServer.hpp>
#include <sentum/utils/AsyncLogger.hpp>
#include <sentum/utils/Database.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

struct CollectorTestAccess {
    static void prepare_persistence(Collector& collector, bool producer_active) {
        collector.running.store(true, std::memory_order_release);
        collector.producer_stopped.store(!producer_active, std::memory_order_release);
        collector.logger.start();
    }

    static void start_writer(Collector& collector) {
        collector.writer_thread = std::thread(&Collector::writer_loop, &collector);
    }

    static bool enqueue(Collector& collector, Kline kline) {
        if (collector.canonical_symbols.empty()) return false;
        return collector.try_enqueue(&collector.canonical_symbols.front(), std::move(kline));
    }

    static bool running(const Collector& collector) {
        return collector.running.load(std::memory_order_acquire);
    }

    static void request_stop(Collector& collector) {
        collector.running.store(false, std::memory_order_release);
        collector.queue_cv.notify_all();
    }

    static void producer_done(Collector& collector) {
        collector.producer_stopped.store(true, std::memory_order_release);
        collector.queue_cv.notify_all();
    }

    static void join_writer(Collector& collector) {
        if (collector.writer_thread.joinable()) collector.writer_thread.join();
        collector.logger.stop();
    }
};

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void remove_database_files(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + "-wal", ignored);
    std::filesystem::remove(path.string() + "-shm", ignored);
}

Kline make_kline(std::int64_t timestamp) {
    return Kline{timestamp, 100.0, 101.0, 99.0, 100.5, 1.0};
}

std::vector<MarketInfo> test_markets() {
    return {MarketInfo{"BTCUSDT", "BTC", 8, "USDT", 8}};
}

std::uint16_t reserve_free_port() {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return acceptor.local_endpoint().port();
}

std::string http_get(std::uint16_t port, const std::string& target) {
    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    stream.expires_after(2s);
    const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
    stream.connect(endpoints);

    http::request<http::empty_body> request{http::verb::get, target, 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::user_agent, "sentum-lifecycle-test");
    http::write(stream, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    require(response.result() == http::status::ok, "dashboard returned non-200 response");

    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    return response.body();
}

void test_async_logger_concurrency() {
    std::filesystem::create_directories("log");
    const std::filesystem::path path = "log/lifecycle_async_logger.log";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    AsyncLogger logger(path.string());
    logger.start();

    constexpr int thread_count = 4;
    constexpr int messages_per_thread = 100;
    std::vector<std::thread> writers;
    writers.reserve(thread_count);
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        writers.emplace_back([&logger, thread_index] {
            for (int i = 0; i < messages_per_thread; ++i) {
                logger.log("writer=" + std::to_string(thread_index) + " message=" + std::to_string(i));
            }
        });
    }
    for (auto& writer : writers) writer.join();
    logger.stop();

    std::ifstream file(path);
    require(static_cast<bool>(file), "async logger output was not created");
    std::size_t lines = 0;
    for (std::string line; std::getline(file, line);) ++lines;
    require(lines == static_cast<std::size_t>(thread_count * messages_per_thread),
            "async logger lost messages during concurrent shutdown test");
}

void test_collector_drains_non_empty_persistence_queue() {
    std::filesystem::create_directories("log");
    const std::filesystem::path db_path = "log/lifecycle_collector_backlog.db";
    remove_database_files(db_path);

    constexpr std::size_t backlog_size = 1024;
    {
        Database db(db_path.string());
        Collector collector(db, test_markets());
        CollectorTestAccess::prepare_persistence(collector, false);

        for (std::size_t i = 0; i < backlog_size; ++i) {
            require(CollectorTestAccess::enqueue(collector, make_kline(static_cast<std::int64_t>(i + 1))),
                    "collector test backlog unexpectedly overflowed");
        }
        require(collector.queue_depth() == backlog_size, "collector test backlog was not fully queued");

        CollectorTestAccess::request_stop(collector);
        const auto stop_started = std::chrono::steady_clock::now();
        CollectorTestAccess::start_writer(collector);
        CollectorTestAccess::join_writer(collector);
        const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;

        require(stop_elapsed <= 2s, "collector backlog drain exceeded two-second lifecycle budget");
        require(collector.queue_depth() == 0, "collector left persistence items queued after shutdown");
        require(db.load_klines("btcusdt", static_cast<int>(backlog_size + 1)).size() == backlog_size,
                "collector did not persist the complete shutdown backlog");
    }

    remove_database_files(db_path);
}

void test_collector_shutdown_under_mock_traffic() {
    std::filesystem::create_directories("log");
    const std::filesystem::path db_path = "log/lifecycle_collector_traffic.db";
    remove_database_files(db_path);

    {
        Database db(db_path.string());
        Collector collector(db, test_markets());
        CollectorTestAccess::prepare_persistence(collector, true);
        CollectorTestAccess::start_writer(collector);

        std::atomic<std::uint64_t> produced{0};
        std::thread producer([&collector, &produced] {
            std::int64_t timestamp = 1;
            while (CollectorTestAccess::running(collector)) {
                if (CollectorTestAccess::enqueue(collector, make_kline(timestamp++))) {
                    produced.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::sleep_for(50us);
            }
            CollectorTestAccess::producer_done(collector);
        });

        std::this_thread::sleep_for(20ms);
        require(produced.load(std::memory_order_relaxed) > 0, "mock collector producer did not generate traffic");

        const auto stop_started = std::chrono::steady_clock::now();
        CollectorTestAccess::request_stop(collector);
        CollectorTestAccess::join_writer(collector);
        const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;
        producer.join();

        require(stop_elapsed <= 2s, "collector mock-traffic shutdown exceeded two-second lifecycle budget");
        require(collector.queue_depth() == 0, "collector left persistence items queued after mock-traffic shutdown");
        require(collector.dropped_count() == 0, "collector dropped mock traffic during lifecycle test");

        const auto accepted = collector.enqueued_count();
        require(accepted == produced.load(std::memory_order_relaxed), "collector enqueue accounting diverged under mock traffic");
        require(db.load_klines("btcusdt", static_cast<int>(accepted + 1)).size() == accepted,
                "collector did not persist all accepted mock traffic before shutdown");
    }

    remove_database_files(db_path);
}

void test_dashboard_repeated_start_stop() {
    const auto port = reserve_free_port();
    sentum::dashboard::DashboardServer server("127.0.0.1", port);

    for (int iteration = 0; iteration < 20; ++iteration) {
        server.start();
        require(server.running(), "dashboard did not enter running state");

        if (iteration % 2 == 0) {
            const auto body = http_get(port, "/api/health");
            require(body.find("\"read_only\":true") != std::string::npos,
                    "dashboard health response lost read-only contract");
        }

        const auto stop_started = std::chrono::steady_clock::now();
        server.stop();
        const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;
        require(!server.running(), "dashboard remained running after stop");
        require(stop_elapsed <= 2s, "dashboard stop exceeded two-second lifecycle budget");
    }
}

void test_dashboard_partial_initialization() {
    asio::io_context io;
    tcp::acceptor blocker(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    blocker.listen();
    const auto port = blocker.local_endpoint().port();

    sentum::dashboard::DashboardServer server("127.0.0.1", port);
    bool failed_as_expected = false;
    try {
        server.start();
    } catch (const std::exception&) {
        failed_as_expected = true;
    }
    require(failed_as_expected, "dashboard start unexpectedly succeeded on an occupied port");
    require(!server.running(), "dashboard remained running after partial initialization failure");
    server.stop();

    boost::system::error_code ec;
    blocker.close(ec);
    server.start();
    require(http_get(port, "/api/health").find("\"status\":\"ok\"") != std::string::npos,
            "dashboard did not recover after partial initialization failure");
    server.stop();
}
} // namespace

int main() {
    try {
        test_async_logger_concurrency();
        test_collector_drains_non_empty_persistence_queue();
        test_collector_shutdown_under_mock_traffic();
        test_dashboard_repeated_start_stop();
        test_dashboard_partial_initialization();
        std::cout << "runtime lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime lifecycle test failure: " << error.what() << '\n';
        return 1;
    }
}
