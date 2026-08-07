#include <sentum/dashboard/DashboardServer.hpp>

#include <algorithm>
#include <charconv>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include <sentum/dashboard/DashboardAssets.hpp>
#include <sentum/dashboard/DashboardRepository.hpp>
#include <sentum/dashboard/DashboardState.hpp>

namespace sentum::dashboard {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

struct DashboardServer::Impl {
    asio::io_context io;
    std::unique_ptr<tcp::acceptor> acceptor;
    DashboardRepository repository;
};

namespace {
int query_limit(beast::string_view target, int fallback, int maximum) {
    const auto pos = target.find("limit=");
    if (pos == beast::string_view::npos) return fallback;
    auto value = target.substr(pos + 6);
    const auto amp = value.find('&');
    if (amp != beast::string_view::npos) value = value.substr(0, amp);
    int parsed = fallback;
    const char* first = value.data();
    const char* last = value.data() + value.size();
    auto result = std::from_chars(first, last, parsed);
    return result.ec == std::errc{} ? std::clamp(parsed, 1, maximum) : fallback;
}

http::response<http::string_body> json_response(const nlohmann::json& value, unsigned version) {
    http::response<http::string_body> response{http::status::ok, version};
    response.set(http::field::content_type, "application/json; charset=utf-8");
    response.set(http::field::cache_control, "no-store");
    response.body() = value.dump();
    response.prepare_payload();
    return response;
}

http::response<http::string_body> text_response(http::status status, std::string body,
                                                 const char* content_type, unsigned version) {
    http::response<http::string_body> response{status, version};
    response.set(http::field::content_type, content_type);
    response.set(http::field::cache_control, "no-store");
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}
}

DashboardServer::DashboardServer(std::uint16_t port)
    : impl_(std::make_unique<Impl>()), port_(port) {}

DashboardServer::~DashboardServer() { stop(); }

void DashboardServer::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    try {
        tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1"), port_};
        impl_->acceptor = std::make_unique<tcp::acceptor>(impl_->io);
        impl_->acceptor->open(endpoint.protocol());
        impl_->acceptor->set_option(asio::socket_base::reuse_address(true));
        impl_->acceptor->bind(endpoint);
        impl_->acceptor->listen(asio::socket_base::max_listen_connections);
        thread_ = std::thread(&DashboardServer::run, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        impl_->acceptor.reset();
        throw;
    }
}

void DashboardServer::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (impl_->acceptor) {
        boost::system::error_code ec;
        impl_->acceptor->cancel(ec);
        impl_->acceptor->close(ec);
    }
    impl_->io.stop();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

void DashboardServer::run() {
    while (running_.load(std::memory_order_acquire)) {
        boost::system::error_code ec;
        tcp::socket socket{impl_->io};
        impl_->acceptor->accept(socket, ec);
        if (ec) {
            if (!running_.load(std::memory_order_acquire)) break;
            continue;
        }

        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::read(socket, buffer, request, ec);
        if (ec) continue;

        http::response<http::string_body> response;
        const auto target = request.target();
        try {
            if (request.method() != http::verb::get) {
                response = text_response(http::status::method_not_allowed, "read-only dashboard", "text/plain", request.version());
            } else if (target == "/" || target == "/index.html") {
                response = text_response(http::status::ok, kDashboardHtml, "text/html; charset=utf-8", request.version());
            } else if (target.starts_with("/api/status")) {
                auto state = DashboardState::global().snapshot();
                state["dashboard_port"] = port_;
                response = json_response(state, request.version());
            } else if (target.starts_with("/api/trades")) {
                response = json_response(impl_->repository.recent_trades(query_limit(target, 100, 1000)), request.version());
            } else if (target.starts_with("/api/orders")) {
                response = json_response(impl_->repository.recent_orders(query_limit(target, 100, 1000)), request.version());
            } else if (target.starts_with("/api/equity")) {
                response = json_response(impl_->repository.equity_curve(query_limit(target, 500, 5000)), request.version());
            } else if (target.starts_with("/api/replay")) {
                response = json_response(impl_->repository.replay_metrics(), request.version());
            } else if (target == "/api/health") {
                response = json_response({{"status", "ok"}, {"read_only", true}}, request.version());
            } else {
                response = text_response(http::status::not_found, "not found", "text/plain", request.version());
            }
        } catch (const std::exception& error) {
            response = json_response({{"error", error.what()}}, request.version());
            response.result(http::status::internal_server_error);
        }

        response.keep_alive(false);
        http::write(socket, response, ec);
        socket.shutdown(tcp::socket::shutdown_both, ec);
    }
}

} // namespace sentum::dashboard
