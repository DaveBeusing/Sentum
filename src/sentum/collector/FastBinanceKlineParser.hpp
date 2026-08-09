#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace sentum::collector {

struct ParsedKline {
    std::string_view symbol;
    std::int64_t timestamp = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    bool closed = false;
};

class FastBinanceKlineParser {
public:
    static bool parse(std::string_view payload, ParsedKline& out) noexcept {
        const auto kline = payload.find("\"k\":{");
        if (kline == std::string_view::npos) return false;
        const auto body = payload.substr(kline + 5);
        return string_field(body, "s", out.symbol) &&
               integer_field(body, "t", out.timestamp) &&
               decimal_field(body, "o", out.open) &&
               decimal_field(body, "h", out.high) &&
               decimal_field(body, "l", out.low) &&
               decimal_field(body, "c", out.close) &&
               decimal_field(body, "v", out.volume) &&
               bool_field(body, "x", out.closed);
    }

private:
    static std::size_t value_pos(std::string_view body, std::string_view key) noexcept {
        char pattern[8] = {'\"', 0, '\"', ':', 0, 0, 0, 0};
        if (key.size() != 1) return std::string_view::npos;
        pattern[1] = key[0];
        const auto pos = body.find(std::string_view(pattern, 4));
        return pos == std::string_view::npos ? pos : pos + 4;
    }

    static bool string_field(std::string_view body, std::string_view key, std::string_view& value) noexcept {
        auto pos = value_pos(body, key);
        if (pos == std::string_view::npos || pos >= body.size() || body[pos] != '\"') return false;
        ++pos;
        const auto end = body.find('\"', pos);
        if (end == std::string_view::npos) return false;
        value = body.substr(pos, end - pos);
        return true;
    }

    static bool integer_field(std::string_view body, std::string_view key, std::int64_t& value) noexcept {
        auto pos = value_pos(body, key);
        if (pos == std::string_view::npos) return false;
        bool negative = false;
        if (pos < body.size() && body[pos] == '-') { negative = true; ++pos; }
        std::int64_t result = 0;
        bool any = false;
        while (pos < body.size() && body[pos] >= '0' && body[pos] <= '9') {
            any = true;
            result = result * 10 + (body[pos++] - '0');
        }
        if (!any) return false;
        value = negative ? -result : result;
        return true;
    }

    static bool decimal_field(std::string_view body, std::string_view key, double& value) noexcept {
        auto pos = value_pos(body, key);
        if (pos == std::string_view::npos) return false;
        if (pos < body.size() && body[pos] == '\"') ++pos;
        bool negative = false;
        if (pos < body.size() && body[pos] == '-') { negative = true; ++pos; }
        double result = 0.0;
        bool any = false;
        while (pos < body.size() && body[pos] >= '0' && body[pos] <= '9') {
            any = true;
            result = result * 10.0 + static_cast<double>(body[pos++] - '0');
        }
        if (pos < body.size() && body[pos] == '.') {
            ++pos;
            double scale = 0.1;
            while (pos < body.size() && body[pos] >= '0' && body[pos] <= '9') {
                any = true;
                result += static_cast<double>(body[pos++] - '0') * scale;
                scale *= 0.1;
            }
        }
        if (!any) return false;
        value = negative ? -result : result;
        return true;
    }

    static bool bool_field(std::string_view body, std::string_view key, bool& value) noexcept {
        const auto pos = value_pos(body, key);
        if (pos == std::string_view::npos) return false;
        if (body.substr(pos, 4) == "true") { value = true; return true; }
        if (body.substr(pos, 5) == "false") { value = false; return true; }
        return false;
    }
};

} // namespace sentum::collector
