#include <sentum/api/BinanceRestClient.hpp>
#include <sentum/api/BinanceSpotExecutionClient.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
template<class Client, class = void>
struct exposes_legacy_order : std::false_type {};

template<class Client>
struct exposes_legacy_order<Client, std::void_t<decltype(std::declval<Client&>().send_signed_order(
    std::declval<const std::string&>(), std::declval<const std::string&>()))>> : std::true_type {};

static_assert(!exposes_legacy_order<BinanceRestClient>::value,
              "The shared read client must not expose the legacy production order API");
static_assert(BinanceSpotExecutionClient::environment() == sentum::api::BinanceEnvironment::SpotTestnet,
              "The concrete exchange execution client must remain Testnet-only");

std::vector<std::string> destinations;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Action>
void expect_intercepted_request(Action action) {
    const auto before = destinations.size();
    try {
        action();
        throw std::runtime_error("Intercepted transport failure was not propagated");
    } catch (const BinanceRequestError& error) {
        require(error.kind() == BinanceRequestError::Kind::Unknown,
                "Transport interruption must preserve unknown execution outcome");
    }
    require(destinations.size() == before + 1, "Expected exactly one intercepted HTTP request");
    require(destinations.back().rfind("https://testnet.binance.vision/api/v3/order?", 0) == 0,
            "Order transport selected an unsupported destination");
}
} // namespace

// The linker redirects transport here, so this test cannot submit an exchange request.
extern "C" __attribute__((used)) CURLcode __wrap_curl_easy_perform(CURL* handle) {
    char* url = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &url) != CURLE_OK || url == nullptr)
        return CURLE_FAILED_INIT;
    destinations.emplace_back(url);
    return CURLE_COULDNT_CONNECT;
}

int main() {
    try {
        BinanceSpotExecutionClient client("fixture-key", "fixture-secret");
        sentum::order::Request order;
        order.symbol = "BTCUSDT";
        order.client_order_id = "boundary-fixture";
        order.side = sentum::order::Side::Buy;
        order.quantity = 1.0;
        order.quantity_text = "1";
        expect_intercepted_request([&] { client.place_market_order(order); });
        order.side = sentum::order::Side::Sell;
        expect_intercepted_request([&] { client.place_market_order(order); });
        expect_intercepted_request([&] { client.cancel_order(order.symbol, order.client_order_id); });
        expect_intercepted_request([&] { client.query_order(order.symbol, order.client_order_id); });
        std::cout << "Execution boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
