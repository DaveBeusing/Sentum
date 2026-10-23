#include <sentum/api/TlsClientContext.hpp>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace {

using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
using Ssl = std::unique_ptr<SSL, decltype(&SSL_free)>;
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto pattern = (std::filesystem::temp_directory_path() / "sentum-tls-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const auto directory = ::mkdtemp(buffer.data());
        require(directory != nullptr, "cannot create temporary TLS directory");
        path = directory;
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

class ScopedEnvironment {
public:
    ScopedEnvironment(std::string name, const std::string& value) : name_(std::move(name)) {
        if (const auto previous = std::getenv(name_.c_str())) previous_ = previous;
        require(::setenv(name_.c_str(), value.c_str(), 1) == 0, "cannot configure test trust store");
    }

    ~ScopedEnvironment() {
        if (previous_) ::setenv(name_.c_str(), previous_->c_str(), 1);
        else ::unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

Key make_key() {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    require(context && EVP_PKEY_keygen_init(context.get()) == 1, "cannot initialize test key");
    require(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) == 1,
            "cannot select test key curve");
    EVP_PKEY* key = nullptr;
    require(EVP_PKEY_keygen(context.get(), &key) == 1, "cannot generate test key");
    return Key(key, EVP_PKEY_free);
}

void add_extension(X509* certificate, X509* issuer, int nid, const std::string& value) {
    X509V3_CTX context{};
    X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
        X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str()), X509_EXTENSION_free);
    require(extension && X509_add_ext(certificate, extension.get(), -1) == 1,
            "cannot add test certificate extension");
}

Certificate make_certificate(EVP_PKEY* key, const std::string& name, long serial,
                             X509* issuer = nullptr, EVP_PKEY* issuer_key = nullptr,
                             bool expired = false) {
    Certificate certificate(X509_new(), X509_free);
    require(certificate && X509_set_version(certificate.get(), 2) == 1,
            "cannot initialize test certificate");
    require(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial) == 1,
            "cannot set certificate serial");
    require(X509_set_pubkey(certificate.get(), key) == 1, "cannot set certificate key");
    require(ASN1_TIME_set_string(X509_getm_notBefore(certificate.get()), "20200101000000Z") == 1 &&
            ASN1_TIME_set_string(X509_getm_notAfter(certificate.get()),
                                 expired ? "20210101000000Z" : "20400101000000Z") == 1,
            "cannot set certificate validity");
    auto subject = X509_get_subject_name(certificate.get());
    require(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                      reinterpret_cast<const unsigned char*>(name.c_str()),
                                      -1, -1, 0) == 1, "cannot set certificate subject");
    require(X509_set_issuer_name(certificate.get(), issuer ? X509_get_subject_name(issuer) : subject) == 1,
            "cannot set certificate issuer");

    auto signing_certificate = issuer ? issuer : certificate.get();
    add_extension(certificate.get(), signing_certificate, NID_basic_constraints,
                  issuer ? "critical,CA:FALSE" : "critical,CA:TRUE");
    add_extension(certificate.get(), signing_certificate, NID_key_usage,
                  issuer ? "critical,digitalSignature" : "critical,keyCertSign,cRLSign");
    if (issuer) {
        add_extension(certificate.get(), issuer, NID_ext_key_usage, "serverAuth");
        add_extension(certificate.get(), issuer, NID_subject_alt_name, "DNS:" + name);
    }
    require(X509_sign(certificate.get(), issuer_key ? issuer_key : key, EVP_sha256()) > 0,
            "cannot sign test certificate");
    return certificate;
}

void write_ca(const std::filesystem::path& path, X509* certificate) {
    Bio output(BIO_new_file(path.string().c_str(), "w"), BIO_free);
    require(output && PEM_write_bio_X509(output.get(), certificate) == 1 && BIO_flush(output.get()) == 1,
            "cannot write test trust store");
}

struct HandshakeResult {
    bool accepted;
    long verification_error;
};

HandshakeResult handshake(const std::string& hostname, X509* certificate, EVP_PKEY* key) {
    auto client_context = sentum::api::make_tls_client_context(hostname);
    require((SSL_CTX_get_verify_mode(client_context->native_handle()) & SSL_VERIFY_PEER) != 0,
            "client context must require peer verification");
    boost::asio::ssl::context server_context(boost::asio::ssl::context::tls_server);
    require(SSL_CTX_use_certificate(server_context.native_handle(), certificate) == 1 &&
            SSL_CTX_use_PrivateKey(server_context.native_handle(), key) == 1,
            "cannot configure test server identity");

    Ssl client(SSL_new(client_context->native_handle()), SSL_free);
    Ssl server(SSL_new(server_context.native_handle()), SSL_free);
    require(client && server, "cannot create TLS handshake peers");
    // The certificate checks use a fixed date, so these fixtures do not expire
    // with the machine's clock. Production contexts always use the real clock.
    X509_VERIFY_PARAM_set_time(SSL_get0_param(client.get()), std::time_t{1893456000}); // 2030-01-01 UTC
    require(SSL_set_tlsext_host_name(client.get(), hostname.c_str()) == 1, "cannot set test SNI hostname");

    BIO* client_transport = nullptr;
    BIO* server_transport = nullptr;
    require(BIO_new_bio_pair(&client_transport, 0, &server_transport, 0) == 1,
            "cannot create in-memory TLS transport");
    SSL_set_bio(client.get(), client_transport, client_transport);
    SSL_set_bio(server.get(), server_transport, server_transport);
    SSL_set_connect_state(client.get());
    SSL_set_accept_state(server.get());

    // Drive real TLS negotiation over memory BIOs instead of a network socket.
    bool client_done = false;
    bool server_done = false;
    for (int step = 0; step < 128; ++step) {
        if (!client_done) {
            ERR_clear_error();
            const int result = SSL_do_handshake(client.get());
            if (result == 1) client_done = true;
            else {
                const int error = SSL_get_error(client.get(), result);
                if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                    return {false, SSL_get_verify_result(client.get())};
                }
            }
        }
        if (!server_done) {
            ERR_clear_error();
            const int result = SSL_do_handshake(server.get());
            if (result == 1) server_done = true;
            else {
                const int error = SSL_get_error(server.get(), result);
                require(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE,
                        "test server failed before certificate verification completed");
            }
        }
        if (client_done && server_done) return {true, SSL_get_verify_result(client.get())};
    }
    throw std::runtime_error("TLS handshake did not finish within the bounded test loop");
}

void require_rejected(const HandshakeResult& result, const std::string& reason) {
    require(!result.accepted && result.verification_error != X509_V_OK,
            reason + " must fail certificate verification");
}

} // namespace

int main() {
    try {
        TemporaryDirectory directory;
        const auto trust_file = directory.path / "ca.pem";
        const auto trust_directory = directory.path / "empty-ca-directory";
        std::filesystem::create_directory(trust_directory);
        auto ca_key = make_key();
        auto ca = make_certificate(ca_key.get(), "Sentum TLS test CA", 1);
        write_ca(trust_file, ca.get());
        // Exercise the production factory's default trust loading. No test CA
        // or verification override is added to the returned client context.
        ScopedEnvironment trust_file_environment(X509_get_default_cert_file_env(), trust_file.string());
        ScopedEnvironment trust_directory_environment(X509_get_default_cert_dir_env(), trust_directory.string());

        auto server_key = make_key();
        const std::vector<std::string> hosts{"stream.binance.com", "stream.testnet.binance.vision"};
        for (const auto& host : hosts) {
            auto certificate = make_certificate(server_key.get(), host, 2, ca.get(), ca_key.get());
            const auto result = handshake(host, certificate.get(), server_key.get());
            require(result.accepted && result.verification_error == X509_V_OK,
                    "trusted certificate for " + host + " must complete TLS");
        }

        auto certificate = make_certificate(server_key.get(), hosts.front(), 3, ca.get(), ca_key.get());
        require_rejected(handshake("wrong-host.example", certificate.get(), server_key.get()), "wrong hostname");

        auto expired = make_certificate(server_key.get(), hosts.front(), 4, ca.get(), ca_key.get(), true);
        const auto expired_result = handshake(hosts.front(), expired.get(), server_key.get());
        require_rejected(expired_result, "expired certificate");
        require(expired_result.verification_error == X509_V_ERR_CERT_HAS_EXPIRED,
                "expired certificate must fail its validity check");

        auto untrusted_ca_key = make_key();
        auto untrusted_ca = make_certificate(untrusted_ca_key.get(), "Untrusted TLS test CA", 5);
        auto untrusted = make_certificate(server_key.get(), hosts.front(), 6,
                                          untrusted_ca.get(), untrusted_ca_key.get());
        require_rejected(handshake(hosts.front(), untrusted.get(), server_key.get()), "untrusted issuer");

        {
            ScopedEnvironment missing_trust_file(X509_get_default_cert_file_env(),
                                                  (directory.path / "missing-ca.pem").string());
            bool rejected = false;
            try {
                const auto result = handshake(hosts.front(), certificate.get(), server_key.get());
                rejected = !result.accepted && result.verification_error != X509_V_OK;
            } catch (const boost::system::system_error&) {
                rejected = true; // Failing to initialize the trust store is also fail-closed.
            }
            require(rejected, "missing trust store must not disable verification");
        }

        for (const auto& invalid_host : {std::string{}, std::string("host\0suffix", 11)}) {
            bool rejected = false;
            try { (void)sentum::api::make_tls_client_context(invalid_host); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected, "missing or NUL-containing hostname must be rejected");
        }

        std::cout << "TLS client context tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TLS client context tests failed: " << error.what() << '\n';
        return 1;
    }
}
