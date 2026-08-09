#pragma once

#include <cstdint>
#include <string_view>

namespace sentum::market {

using SymbolId = std::uint32_t;
inline constexpr SymbolId kInvalidSymbolId = 0;

inline std::uint64_t symbol_hash(std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : value) {
        if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace sentum::market
