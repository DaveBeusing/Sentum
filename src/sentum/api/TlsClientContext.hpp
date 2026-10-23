#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/verify_mode.hpp>

namespace sentum::api {

inline std::shared_ptr<boost::asio::ssl::context> make_tls_client_context(const std::string& hostname) {
    if (hostname.empty() || hostname.find('\0') != std::string::npos) {
        throw std::invalid_argument("TLS peer verification requires a valid hostname");
    }

    namespace ssl = boost::asio::ssl;
    auto context = std::make_shared<ssl::context>(ssl::context::tls_client);
    context->set_options(ssl::context::default_workarounds |
                         ssl::context::no_sslv2 |
                         ssl::context::no_sslv3);
    context->set_default_verify_paths();
    context->set_verify_mode(ssl::verify_peer);
    context->set_verify_callback(ssl::host_name_verification(hostname));
    return context;
}

} // namespace sentum::api
