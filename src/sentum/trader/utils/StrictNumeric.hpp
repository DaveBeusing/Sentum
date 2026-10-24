#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace sentum::numeric {

inline double finite_number(std::string_view text, const std::string& field) {
    if (text.empty()) throw std::invalid_argument("Invalid finite number for " + field);
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value,
                                        std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        !std::isfinite(value))
        throw std::invalid_argument("Invalid finite number for " + field);
    return value;
}

inline std::int64_t integer(std::string_view text, const std::string& field) {
    if (text.empty()) throw std::invalid_argument("Invalid integer for " + field);
    std::int64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("Invalid integer for " + field);
    return value;
}

inline std::string decimal_text(double value) {
    if (!std::isfinite(value) || value < 0.0)
        throw std::invalid_argument("A decimal quantity must be finite and nonnegative");
    // to_chars uses the shortest round-trip representation and never consults
    // the process locale. Fixed form avoids exponent syntax in signed orders.
    char buffer[768];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed);
    if (result.ec != std::errc{}) throw std::invalid_argument("Cannot represent decimal quantity");
    return std::string(buffer, result.ptr);
}

} // namespace sentum::numeric
