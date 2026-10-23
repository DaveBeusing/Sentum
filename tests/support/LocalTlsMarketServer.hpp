#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <websocketpp/config/asio.hpp>
#include <websocketpp/server.hpp>

namespace sentum::test {

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename Predicate>
void eventually(Predicate predicate, const std::string& message,
                std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

class EnvironmentOverride {
public:
    EnvironmentOverride(std::string key, const std::string& value) : key_(std::move(key)) {
        if (const auto existing = std::getenv(key_.c_str())) previous_ = existing;
        require(::setenv(key_.c_str(), value.c_str(), 1) == 0, "Cannot configure local TLS trust store");
    }
    ~EnvironmentOverride() {
        if (previous_) ::setenv(key_.c_str(), previous_->c_str(), 1);
        else ::unsetenv(key_.c_str());
    }
private:
    std::string key_;
    std::optional<std::string> previous_;
};

class LocalTlsIdentity {
public:
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;

    LocalTlsIdentity() : ca_key_(key()), ca_(certificate(ca_key_.get(), nullptr, nullptr)),
                         server_key_(key()), server_(certificate(server_key_.get(), ca_.get(), ca_key_.get())) {
        std::string pattern = (std::filesystem::temp_directory_path() / "sentum-market-tls-XXXXXX").string();
        const char* path = ::mkdtemp(pattern.data());
        require(path != nullptr, "Cannot create local TLS fixture directory");
        directory_ = path;
        std::filesystem::create_directory(directory_ / "empty");
        const auto trust_file = directory_ / "ca.pem";
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(trust_file.c_str(), "w"), BIO_free);
        require(output && PEM_write_bio_X509(output.get(), ca_.get()) == 1 && BIO_flush(output.get()) == 1,
                "Cannot write local TLS fixture CA");
        trust_file_ = std::make_unique<EnvironmentOverride>("SSL_CERT_FILE", trust_file.string());
        trust_directory_ = std::make_unique<EnvironmentOverride>("SSL_CERT_DIR", (directory_ / "empty").string());
    }

    ~LocalTlsIdentity() {
        trust_directory_.reset();
        trust_file_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    std::shared_ptr<boost::asio::ssl::context> context() const {
        auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
        require(SSL_CTX_use_certificate(context->native_handle(), server_.get()) == 1 &&
                SSL_CTX_use_PrivateKey(context->native_handle(), server_key_.get()) == 1,
                "Cannot initialize local TLS server");
        return context;
    }

private:
    static Key key() {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
        require(context && EVP_PKEY_keygen_init(context.get()) == 1 &&
                EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) == 1,
                "Cannot initialize local TLS key");
        EVP_PKEY* result = nullptr;
        require(EVP_PKEY_keygen(context.get(), &result) == 1, "Cannot generate local TLS key");
        return Key(result, EVP_PKEY_free);
    }

    static void extension(X509* certificate, X509* issuer, int nid, const char* value) {
        X509V3_CTX context{};
        X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> ext(
            X509V3_EXT_conf_nid(nullptr, &context, nid, value), X509_EXTENSION_free);
        require(ext && X509_add_ext(certificate, ext.get(), -1) == 1, "Cannot initialize local TLS certificate extension");
    }

    static Certificate certificate(EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key) {
        Certificate result(X509_new(), X509_free);
        require(result && X509_set_version(result.get(), 2) == 1 &&
                ASN1_INTEGER_set(X509_get_serialNumber(result.get()), issuer ? 2 : 1) == 1 &&
                X509_set_pubkey(result.get(), key) == 1, "Cannot initialize local TLS certificate");
        // Generated at runtime; the server certificate is always current for
        // the test process and no private identity is committed to the tree.
        require(X509_gmtime_adj(X509_getm_notBefore(result.get()), -3600) &&
                X509_gmtime_adj(X509_getm_notAfter(result.get()), 86400), "Cannot set TLS validity");
        auto subject = X509_get_subject_name(result.get());
        const std::string name = issuer ? "localhost" : "Sentum local market fixture";
        require(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                    reinterpret_cast<const unsigned char*>(name.c_str()), -1, -1, 0) == 1 &&
                X509_set_issuer_name(result.get(), issuer ? X509_get_subject_name(issuer) : subject) == 1,
                "Cannot set TLS subject");
        auto signing = issuer ? issuer : result.get();
        extension(result.get(), signing, NID_basic_constraints, issuer ? "critical,CA:FALSE" : "critical,CA:TRUE");
        extension(result.get(), signing, NID_key_usage, issuer ? "critical,digitalSignature" : "critical,keyCertSign,cRLSign");
        if (issuer) {
            extension(result.get(), issuer, NID_ext_key_usage, "serverAuth");
            extension(result.get(), issuer, NID_subject_alt_name, "DNS:localhost");
        }
        require(X509_sign(result.get(), issuer_key ? issuer_key : key, EVP_sha256()) > 0, "Cannot sign local TLS certificate");
        return result;
    }

    Key ca_key_;
    Certificate ca_;
    Key server_key_;
    Certificate server_;
    std::filesystem::path directory_;
    std::unique_ptr<EnvironmentOverride> trust_file_;
    std::unique_ptr<EnvironmentOverride> trust_directory_;
};

class LocalTlsMarketServer {
public:
    explicit LocalTlsMarketServer(const LocalTlsIdentity& identity, std::uint16_t port = 0) {
        server_.init_asio();
        server_.clear_access_channels(websocketpp::log::alevel::all);
        server_.clear_error_channels(websocketpp::log::elevel::all);
        server_.set_reuse_addr(true);
        server_.set_tls_init_handler([&identity](websocketpp::connection_hdl) { return identity.context(); });
        server_.set_open_handler([this](websocketpp::connection_hdl handle) {
            clients_.insert(handle);
            accepted_.fetch_add(1, std::memory_order_release);
        });
        server_.set_close_handler([this](websocketpp::connection_hdl handle) { clients_.erase(handle); });
        server_.set_fail_handler([this](websocketpp::connection_hdl handle) { clients_.erase(handle); });
        server_.listen(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
        boost::system::error_code error;
        port_ = server_.get_local_endpoint(error).port();
        require(!error, "Cannot read local TLS listener port");
        server_.start_accept();
        worker_ = std::thread([this] { server_.run(); });
    }
    ~LocalTlsMarketServer() { stop(); }

    std::string url() const { return "wss://localhost:" + std::to_string(port_) + "/market"; }
    std::uint16_t port() const { return port_; }
    std::size_t accepted() const { return accepted_.load(std::memory_order_acquire); }

    void send(const std::string& payload) {
        server_.get_io_service().post([this, payload] {
            for (const auto& handle : clients_) {
                websocketpp::lib::error_code error;
                server_.send(handle, payload, websocketpp::frame::opcode::text, error);
            }
        });
    }

    void drop_connections() {
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

    void stop() {
        if (!worker_.joinable()) return;
        server_.stop();
        worker_.join();
    }

private:
    websocketpp::server<websocketpp::config::asio_tls> server_;
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> clients_;
    std::atomic<std::size_t> accepted_{0};
    std::uint16_t port_ = 0;
    std::thread worker_;
};

inline std::int64_t unix_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline std::string trade_message(double price = 100.0, std::int64_t timestamp = 0) {
    if (timestamp == 0) timestamp = unix_milliseconds();
    return "{\"e\":\"trade\",\"s\":\"BTCUSDT\",\"p\":\"" + std::to_string(price) + "\",\"T\":" + std::to_string(timestamp) + "}";
}

inline std::string kline_message(std::int64_t timestamp = 0) {
    if (timestamp == 0) timestamp = unix_milliseconds();
    return "{\"stream\":\"btcusdt@kline_1s\",\"data\":{\"E\":" + std::to_string(unix_milliseconds()) +
        ",\"k\":{\"s\":\"BTCUSDT\",\"t\":" + std::to_string(timestamp) +
        ",\"o\":\"100\",\"h\":\"101\",\"l\":\"99\",\"c\":\"100.5\",\"v\":\"1\",\"x\":true}}}";
}

} // namespace sentum::test
