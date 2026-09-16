#include <sentum/dashboard/DashboardServer.hpp>
#include <sentum/utils/AsyncLogger.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
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
        test_dashboard_repeated_start_stop();
        test_dashboard_partial_initialization();
        std::cout << "runtime lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime lifecycle test failure: " << error.what() << '\n';
        return 1;
    }
}
