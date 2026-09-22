#include <sentum/dashboard/DashboardServer.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include <sentum/dashboard/DashboardAssets.hpp>
#include <sentum/dashboard/DashboardOperationsOverlay.hpp>
#include <sentum/dashboard/DashboardRepository.hpp>
#include <sentum/dashboard/DashboardState.hpp>
#include <sentum/operations/GovernedIncidentLifecycleRepository.hpp>
#include <sentum/operations/NotificationDeliveryEvidenceRepository.hpp>
#include <sentum/ui/CrossSurfaceOperationsView.hpp>

namespace sentum::dashboard {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

struct DashboardServer::Impl {
    std::mutex lifecycle_mutex;
    std::unique_ptr<asio::io_context> io;
    std::unique_ptr<tcp::acceptor> acceptor;
    DashboardRepository repository;
};

namespace {
bool starts_with(beast::string_view value, beast::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string query_value(beast::string_view target, beast::string_view key) {
    std::string needle(key);
    needle += '=';
    const auto pos = target.find(needle);
    if (pos == beast::string_view::npos) return {};
    auto value = target.substr(pos + needle.size());
    const auto amp = value.find('&');
    if (amp != beast::string_view::npos) value = value.substr(0, amp);
    std::string out(value.data(), value.size());
    if (out.size() > 160) return {};
    for (char c : out) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return {};
    }
    return out;
}

int query_limit(beast::string_view target, int fallback, int maximum) {
    const auto text = query_value(target, "limit");
    if (text.empty()) return fallback;
    int parsed = fallback;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return result.ec == std::errc{} ? std::clamp(parsed, 1, maximum) : fallback;
}

nlohmann::json runtime_status_file() {
    std::ifstream file("log/status.json");
    if (!file) return nlohmann::json::object();
    try {
        nlohmann::json value;
        file >> value;
        return value.is_object() ? value : nlohmann::json::object();
    } catch (...) {
        return nlohmann::json::object();
    }
}

nlohmann::json merged_runtime_state(const std::string& host, std::uint16_t port) {
    auto state = runtime_status_file();
    const auto live = DashboardState::global().snapshot();
    for (auto it = live.begin(); it != live.end(); ++it) {
        if (!it.value().is_null() && !(it.key() == "mode" && it.value() == "idle")) state[it.key()] = it.value();
    }
    state["dashboard_host"] = host;
    state["dashboard_port"] = port;
    return state;
}

nlohmann::json operations_view_from_durable_notification_evidence(const nlohmann::json& state) {
    auto unavailable_state = state;
    if (unavailable_state.contains("operations_control_plane") &&
        unavailable_state["operations_control_plane"].is_object()) {
        unavailable_state["operations_control_plane"].erase("notification_delivery_evidence");
    }

    const auto database_path = state.value("db_path", std::string("log/klines.sqlite3"));
    try {
        sentum::operations::NotificationDeliveryEvidenceRepository notification_evidence(
            database_path,
            sentum::operations::NotificationDeliveryEvidenceOpenMode::ReadOnly);
        try {
            sentum::operations::GovernedIncidentLifecycleRepository incident_lifecycle(
                database_path,
                sentum::operations::GovernedIncidentLifecycleOpenMode::ReadOnly);
            return sentum::ui::derive_cross_surface_operations_view(
                state, notification_evidence, incident_lifecycle);
        } catch (...) {
            auto stale_state = sentum::operations::notification_delivery_snapshot_from_repository(
                state, notification_evidence);
            auto& control_plane = stale_state["operations_control_plane"];
            control_plane["evidence_status"] = "STALE";
            control_plane["evidence_stale"] = true;
            control_plane["incident_state"] = "UNAVAILABLE";
            control_plane["recovery_state"] = "UNAVAILABLE";
            control_plane["pending_approvals"] = 0;
            control_plane["approval_queue"] = nlohmann::json::array();
            control_plane["audit_timeline"] = nlohmann::json::array();
            control_plane["incident_workflow"] = nlohmann::json::object();
            control_plane["recovery_workflow"] = nlohmann::json::object();
            return sentum::ui::derive_cross_surface_operations_view(stale_state);
        }
    } catch (...) {
        return sentum::ui::derive_cross_surface_operations_view(unavailable_state);
    }
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

http::response<http::string_body> build_response(const http::request<http::string_body>& request,
                                                  DashboardRepository& repository,
                                                  const std::string& host,
                                                  std::uint16_t port) {
    const auto target = request.target();
    try {
        if (request.method() != http::verb::get)
            return text_response(http::status::method_not_allowed, "read-only dashboard", "text/plain", request.version());
        if (target == "/" || target == "/index.html")
            return text_response(http::status::ok, dashboard_html_with_operations(kDashboardHtml), "text/html; charset=utf-8", request.version());
        if (starts_with(target, "/api/status"))
            return json_response(merged_runtime_state(host, port), request.version());
        if (starts_with(target, "/api/operations")) {
            const auto state = merged_runtime_state(host, port);
            return json_response(operations_view_from_durable_notification_evidence(state), request.version());
        }
        if (starts_with(target, "/api/trades"))
            return json_response(repository.recent_trades(query_limit(target, 100, 1000)), request.version());
        if (starts_with(target, "/api/orders"))
            return json_response(repository.recent_orders(query_limit(target, 100, 1000)), request.version());
        if (starts_with(target, "/api/equity"))
            return json_response(repository.equity_curve(query_limit(target, 500, 5000)), request.version());
        if (starts_with(target, "/api/replay")) return json_response(repository.replay_metrics(), request.version());
        if (starts_with(target, "/api/research")) return json_response(repository.research_results(), request.version());
        if (starts_with(target, "/api/models"))
            return json_response(repository.models(query_limit(target, 100, 1000)), request.version());
        if (starts_with(target, "/api/model")) {
            const auto id = query_value(target, "model_id");
            return id.empty() ? json_response(nlohmann::json::object(), request.version())
                              : json_response(repository.model_detail(id), request.version());
        }
        if (starts_with(target, "/api/experiments"))
            return json_response(repository.experiment_runs(query_limit(target, 100, 1000)), request.version());
        if (starts_with(target, "/api/experiment/trials")) {
            const auto run = query_value(target, "run_id");
            return run.empty() ? json_response(nlohmann::json::array(), request.version())
                               : json_response(repository.experiment_trials(run, query_limit(target, 5000, 10000)), request.version());
        }
        if (starts_with(target, "/api/experiment")) {
            const auto run = query_value(target, "run_id");
            return run.empty() ? json_response(nlohmann::json::object(), request.version())
                               : json_response(repository.experiment_detail(run), request.version());
        }
        if (target == "/api/health") {
            return json_response({{"status", "ok"}, {"read_only", true}, {"bind", host},
                                  {"research_dashboard", true}, {"model_promotion_dashboard", true},
                                  {"operations_dashboard", true}},
                                 request.version());
        }
        return text_response(http::status::not_found, "not found", "text/plain", request.version());
    } catch (const std::exception& error) {
        auto response = json_response({{"error", error.what()}}, request.version());
        response.result(http::status::internal_server_error);
        return response;
    }
}

class DashboardSession : public std::enable_shared_from_this<DashboardSession> {
public:
    DashboardSession(tcp::socket socket, DashboardRepository& repository, std::string host, std::uint16_t port)
        : socket_(std::move(socket)), repository_(repository), host_(std::move(host)), port_(port) {}

    void start() { read_request(); }

private:
    tcp::socket socket_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    http::response<http::string_body> response_;
    DashboardRepository& repository_;
    std::string host_;
    std::uint16_t port_;

    void read_request() {
        auto self = shared_from_this();
        http::async_read(socket_, buffer_, request_, [self](beast::error_code ec, std::size_t) {
            if (ec) return;
            self->response_ = build_response(self->request_, self->repository_, self->host_, self->port_);
            self->response_.keep_alive(false);
            self->write_response();
        });
    }

    void write_response() {
        auto self = shared_from_this();
        http::async_write(socket_, response_, [self](beast::error_code, std::size_t) {
            beast::error_code ignored;
            self->socket_.shutdown(tcp::socket::shutdown_both, ignored);
            self->socket_.close(ignored);
        });
    }
};
} // namespace

DashboardServer::DashboardServer() : DashboardServer("127.0.0.1", 8080) {}
DashboardServer::DashboardServer(std::uint16_t port) : DashboardServer("127.0.0.1", port) {}
DashboardServer::DashboardServer(std::string host, std::uint16_t port)
    : impl_(std::make_unique<Impl>()), host_(std::move(host)), port_(port) {}
DashboardServer::~DashboardServer() { stop(); }

void DashboardServer::start() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    if (running_.load(std::memory_order_acquire)) return;

    if (thread_.joinable()) thread_.join();
    impl_->acceptor.reset();
    impl_->io.reset();

    try {
        impl_->io = std::make_unique<asio::io_context>(1);
        const tcp::endpoint endpoint{asio::ip::make_address(host_), port_};
        impl_->acceptor = std::make_unique<tcp::acceptor>(*impl_->io);
        impl_->acceptor->open(endpoint.protocol());
        impl_->acceptor->set_option(asio::socket_base::reuse_address(true));
        impl_->acceptor->bind(endpoint);
        impl_->acceptor->listen(asio::socket_base::max_listen_connections);
        running_.store(true, std::memory_order_release);
        accept_next();
        thread_ = std::thread(&DashboardServer::run, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        impl_->acceptor.reset();
        impl_->io.reset();
        throw;
    }
}

void DashboardServer::stop() noexcept {
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    running_.store(false, std::memory_order_release);
    if (impl_->io) impl_->io->stop();

    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) return;
        thread_.join();
    }

    if (impl_->acceptor) {
        boost::system::error_code ec;
        impl_->acceptor->cancel(ec);
        impl_->acceptor->close(ec);
    }
    impl_->acceptor.reset();
    impl_->io.reset();
}

void DashboardServer::accept_next() {
    if (!running_.load(std::memory_order_acquire) || !impl_->acceptor) return;
    impl_->acceptor->async_accept([this](boost::system::error_code ec, tcp::socket socket) {
        if (!running_.load(std::memory_order_acquire)) return;
        if (!ec) {
            std::make_shared<DashboardSession>(std::move(socket), impl_->repository, host_, port_)->start();
        }
        if (running_.load(std::memory_order_acquire) && impl_->acceptor && impl_->acceptor->is_open()) accept_next();
    });
}

void DashboardServer::run() noexcept {
    try {
        if (impl_->io) impl_->io->run();
    } catch (const std::exception& error) {
        std::cerr << "[dashboard] server loop error: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "[dashboard] server loop error: unknown exception\n";
    }
    running_.store(false, std::memory_order_release);
}

} // namespace sentum::dashboard
