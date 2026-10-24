#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/multiprecision/cpp_int.hpp>
#include <nlohmann/json.hpp>
#include <sentum/trader/utils/StrictNumeric.hpp>

namespace sentum::execution {

// Filter values are finite decimals. Integer coefficients avoid binary
// rounding accepting quantities that are off either exchange lot grid.
class ExchangeDecimal {
public:
    using Integer = boost::multiprecision::cpp_int;
    ExchangeDecimal() = default;
    ExchangeDecimal(Integer units, unsigned scale) : units_(std::move(units)), scale_(scale) { trim(); }

    static ExchangeDecimal parse(const std::string& text, unsigned maximum_scale = 18) {
        if (text.empty() || text.size() > 768) throw std::invalid_argument("Invalid exchange decimal");
        Integer units = 0;
        unsigned scale = 0;
        bool point = false, digit = false;
        for (char value : text) {
            if (value == '.' && !point && digit) { point = true; continue; }
            if (value < '0' || value > '9') throw std::invalid_argument("Invalid exchange decimal token");
            units *= 10;
            units += value - '0';
            digit = true;
            if (point) ++scale;
        }
        if (!digit || text.back() == '.' || scale > maximum_scale)
            throw std::invalid_argument("Invalid exchange decimal precision");
        return ExchangeDecimal(std::move(units), scale);
    }
    static ExchangeDecimal from_double(double value) { return parse(numeric::decimal_text(value), 768); }
    bool zero() const noexcept { return units_ == 0; }
    unsigned scale() const noexcept { return scale_; }
    Integer units_at(unsigned scale) const {
        if (scale < scale_) throw std::logic_error("Cannot discard exchange decimal precision");
        return units_ * power10(scale - scale_);
    }
    std::string text() const {
        std::string digits = units_.convert_to<std::string>();
        if (scale_ == 0) return digits;
        if (digits.size() <= scale_) digits.insert(0, scale_ + 1 - digits.size(), '0');
        digits.insert(digits.size() - scale_, 1, '.');
        return digits;
    }
    double value() const { return numeric::finite_number(text(), "exchange decimal"); }
    static int compare(const ExchangeDecimal& left, const ExchangeDecimal& right) {
        const auto scale = std::max(left.scale_, right.scale_);
        const auto a = left.units_at(scale), b = right.units_at(scale);
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    static ExchangeDecimal grid_intersection(const ExchangeDecimal& left, const ExchangeDecimal& right) {
        if (left.zero()) return right;
        if (right.zero()) return left;
        const auto scale = std::max(left.scale_, right.scale_);
        const Integer a = left.units_at(scale), b = right.units_at(scale);
        Integer x = a, y = b;
        while (y != 0) {
            const Integer remainder = x % y;
            x = y;
            y = remainder;
        }
        return ExchangeDecimal((a / x) * b, scale);
    }
    static ExchangeDecimal floor_to_grid(const ExchangeDecimal& value, const ExchangeDecimal& step) {
        if (step.zero()) throw std::logic_error("Invalid exchange quantity step");
        const auto scale = std::max(value.scale_, step.scale_);
        const Integer step_units = step.units_at(scale);
        return ExchangeDecimal((value.units_at(scale) / step_units) * step_units, scale);
    }
    static bool on_grid(const ExchangeDecimal& value, const ExchangeDecimal& step) {
        if (step.zero()) return true;
        const auto scale = std::max(value.scale_, step.scale_);
        return value.units_at(scale) % step.units_at(scale) == 0;
    }
    static ExchangeDecimal product(const ExchangeDecimal& left, const ExchangeDecimal& right) {
        return ExchangeDecimal(left.units_ * right.units_, left.scale_ + right.scale_);
    }
private:
    static Integer power10(unsigned count) {
        Integer value = 1;
        while (count--) value *= 10;
        return value;
    }
    void trim() {
        while (scale_ > 0 && units_ % 10 == 0) { units_ /= 10; --scale_; }
    }
    Integer units_ = 0;
    unsigned scale_ = 0;
};

struct ExchangeRules {
    struct MarketQuantity { double quantity = 0.0; std::string text; };
    double min_quantity = 0.0;
    double max_quantity = 0.0;
    double step_size = 0.0;
    double min_notional = 0.0;
    double max_notional = 0.0;
    int base_precision = 8;
    int quote_precision = 8;
    std::string base_asset;
    std::string quote_asset;

    MarketQuantity normalize_market_quantity(double value, double additional_step = 0.0) const {
        if (!std::isfinite(value) || value <= 0.0) throw std::invalid_argument("Invalid market quantity");
        if (!std::isfinite(additional_step) || additional_step < 0.0)
            throw std::invalid_argument("Invalid additional quantity step");
        const auto grid = additional_step > 0.0
            ? ExchangeDecimal::grid_intersection(quantity_step_, ExchangeDecimal::from_double(additional_step))
            : quantity_step_;
        const auto normalized = ExchangeDecimal::floor_to_grid(ExchangeDecimal::from_double(value), grid);
        validate_quantity_decimal(normalized);
        const double result = normalized.value();
        if (!std::isfinite(result) || result <= 0.0 || result > value ||
            ExchangeDecimal::compare(ExchangeDecimal::from_double(result), normalized) != 0)
            throw std::runtime_error("Quantity normalization cannot increase approved size");
        return {result, normalized.text()};
    }
    double normalize_quantity(double value) const { return normalize_market_quantity(value).quantity; }
    void validate_market_quantity(const std::string& text) const {
        const auto value = ExchangeDecimal::parse(text);
        if (!ExchangeDecimal::on_grid(value, quantity_step_))
            throw std::runtime_error("Quantity does not satisfy exchange lot increments");
        validate_quantity_decimal(value);
    }
    void validate_permissions(const std::set<std::string>& account_permissions) const {
        if (permission_sets_.empty()) throw std::logic_error("Exchange permissions have not been validated");
        for (const auto& alternatives : permission_sets_) {
            const bool permitted = std::any_of(alternatives.begin(), alternatives.end(),
                [&](const std::string& permission) { return account_permissions.count(permission) != 0; });
            if (!permitted) throw std::runtime_error("Account does not satisfy symbol permission sets");
        }
    }
    std::set<int> notional_price_windows() const {
        std::set<int> result;
        for (const auto& rule : notional_rules_) result.insert(rule.average_minutes);
        return result;
    }

    // A non-null exchange reference price takes precedence. Otherwise each
    // filter needs its exact average window (zero means last). The caller must
    // validate that all supplied prices are fresh evidence from Testnet.
    void validate_market_notional(double quantity, const std::map<int, double>& window_prices,
                                  std::optional<double> reference_price = std::nullopt) const {
        if (quantity_step_.zero()) throw std::logic_error("Exchange rules have not been loaded");
        if (!std::isfinite(quantity) || quantity <= 0.0) throw std::invalid_argument("Invalid notional quantity");
        if (reference_price && (!std::isfinite(*reference_price) || *reference_price <= 0.0))
            throw std::invalid_argument("Invalid exchange reference price");
        const auto decimal_quantity = ExchangeDecimal::from_double(quantity);
        for (const auto& rule : notional_rules_) {
            const auto found = window_prices.find(rule.average_minutes);
            if (!reference_price && found == window_prices.end())
                throw std::runtime_error("Missing matching market-notional price window");
            const double price = reference_price ? *reference_price : found->second;
            if (!std::isfinite(price) || price <= 0.0 ||
                !std::isfinite(quantity * price) || quantity * price <= 0.0)
                throw std::invalid_argument("Market notional must be finite and positive");
            const auto notional = ExchangeDecimal::product(decimal_quantity, ExchangeDecimal::from_double(price));
            if (!rule.minimum.zero() && ExchangeDecimal::compare(notional, rule.minimum) < 0)
                throw std::runtime_error("Order notional below exchange minimum");
            if (!rule.maximum.zero() && ExchangeDecimal::compare(notional, rule.maximum) > 0)
                throw std::runtime_error("Order notional above exchange maximum");
        }
    }
    void validate_notional(double quantity, double price) const { validate_market_notional(quantity, {}, price); }

    static ExchangeRules from_exchange_info(const nlohmann::json& info, const std::string& symbol) {
        if (!info.is_object() || !info.contains("symbols") || !info.at("symbols").is_array() || symbol.empty())
            throw std::runtime_error("Invalid exchangeInfo response");
        const nlohmann::json* selected = nullptr;
        for (const auto& item : info.at("symbols")) {
            if (!item.is_object() || !item.contains("symbol") || !item.at("symbol").is_string())
                throw std::runtime_error("Invalid exchangeInfo symbol entry");
            if (item.at("symbol").get<std::string>() != symbol) continue;
            if (selected) throw std::runtime_error("Duplicate symbol in exchangeInfo");
            selected = &item;
        }
        if (!selected) throw std::runtime_error("Symbol missing from exchangeInfo: " + symbol);
        const auto& item = *selected;
        if (required_string(item, "status") != "TRADING") throw std::runtime_error("Symbol is not trading");
        if (!required_bool(item, "isSpotTradingAllowed")) throw std::runtime_error("Symbol does not support Spot trading");
        if (!string_set(item.at("orderTypes"), "orderTypes").count("MARKET"))
            throw std::runtime_error("Symbol does not support MARKET orders");
        ExchangeRules rules;
        rules.base_asset = required_string(item, "baseAsset");
        rules.quote_asset = required_string(item, "quoteAsset");
        if (rules.base_asset == rules.quote_asset) throw std::runtime_error("Exchange base and quote assets must differ");
        rules.base_precision = required_integer(item, "baseAssetPrecision", 18);
        rules.quote_precision = required_integer(item, "quoteAssetPrecision", 18);
        rules.quantity_step_ = ExchangeDecimal(1, static_cast<unsigned>(rules.base_precision));
        if (item.contains("permissionSets")) {
            const auto& sets = item.at("permissionSets");
            if (!sets.is_array() || sets.empty()) throw std::runtime_error("Missing symbol permission sets");
            for (const auto& permissions : sets) rules.permission_sets_.push_back(string_set(permissions, "permissionSets"));
        } else {
            rules.permission_sets_.push_back(string_set(item.at("permissions"), "permissions"));
        }
        if (!item.contains("filters") || !item.at("filters").is_array()) throw std::runtime_error("Invalid exchange symbol filters");
        std::set<std::string> filter_types;
        for (const auto& filter : item.at("filters")) {
            const auto type = required_string(filter, "filterType");
            if (!filter_types.insert(type).second) throw std::runtime_error("Duplicate exchange filter: " + type);
            if (type == "LOT_SIZE" || type == "MARKET_LOT_SIZE") {
                const auto minimum = required_decimal(filter, "minQty");
                const auto maximum = required_decimal(filter, "maxQty");
                const auto step = required_decimal(filter, "stepSize");
                if (ExchangeDecimal::compare(minimum, rules.quantity_minimum_) > 0) rules.quantity_minimum_ = minimum;
                if (!maximum.zero() && (rules.quantity_maximum_.zero() ||
                    ExchangeDecimal::compare(maximum, rules.quantity_maximum_) < 0)) rules.quantity_maximum_ = maximum;
                rules.quantity_step_ = ExchangeDecimal::grid_intersection(rules.quantity_step_, step);
            } else if (type == "MIN_NOTIONAL" || type == "NOTIONAL") {
                const auto minimum = required_decimal(filter, "minNotional");
                const auto maximum = type == "NOTIONAL" ? required_decimal(filter, "maxNotional") : ExchangeDecimal{};
                const bool apply_minimum = required_bool(filter, type == "MIN_NOTIONAL" ? "applyToMarket" : "applyMinToMarket");
                const bool apply_maximum = type == "NOTIONAL" && required_bool(filter, "applyMaxToMarket");
                const int minutes = required_integer(filter, "avgPriceMins", std::numeric_limits<int>::max());
                if (!minimum.zero() && !maximum.zero() && ExchangeDecimal::compare(minimum, maximum) > 0)
                    throw std::runtime_error("Contradictory exchange notional bounds");
                NotionalRule rule{apply_minimum ? minimum : ExchangeDecimal{}, apply_maximum ? maximum : ExchangeDecimal{}, minutes};
                if (!rule.minimum.zero() || !rule.maximum.zero()) rules.notional_rules_.push_back(std::move(rule));
            }
        }
        if (!rules.quantity_maximum_.zero() &&
            (ExchangeDecimal::compare(rules.quantity_minimum_, rules.quantity_maximum_) > 0 ||
             ExchangeDecimal::floor_to_grid(rules.quantity_maximum_, rules.quantity_step_).zero() ||
             ExchangeDecimal::compare(ExchangeDecimal::floor_to_grid(rules.quantity_maximum_, rules.quantity_step_),
                                      rules.quantity_minimum_) < 0))
            throw std::runtime_error("No quantity satisfies the combined exchange filters");
        rules.min_quantity = rules.quantity_minimum_.value();
        rules.max_quantity = rules.quantity_maximum_.value();
        rules.step_size = rules.quantity_step_.value();
        if (rules.step_size <= 0.0) throw std::runtime_error("Exchange quantity step is not representable");
        for (const auto& rule : rules.notional_rules_) {
            for (const auto& other : rules.notional_rules_)
                if (rule.average_minutes == other.average_minutes && !rule.minimum.zero() && !other.maximum.zero() &&
                    ExchangeDecimal::compare(rule.minimum, other.maximum) > 0)
                    throw std::runtime_error("No notional satisfies the combined exchange filters");
            rules.min_notional = std::max(rules.min_notional, rule.minimum.value());
            const double maximum = rule.maximum.value();
            if (maximum > 0.0 && (rules.max_notional == 0.0 || maximum < rules.max_notional)) rules.max_notional = maximum;
        }
        return rules;
    }
private:
    struct NotionalRule { ExchangeDecimal minimum; ExchangeDecimal maximum; int average_minutes; };
    static std::string required_string(const nlohmann::json& value, const char* key) {
        if (!value.is_object() || !value.contains(key) || !value.at(key).is_string() || value.at(key).get_ref<const std::string&>().empty())
            throw std::runtime_error(std::string("Missing exchange string: ") + key);
        return value.at(key).get<std::string>();
    }
    static bool required_bool(const nlohmann::json& value, const char* key) {
        if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean())
            throw std::runtime_error(std::string("Missing exchange boolean: ") + key);
        return value.at(key).get<bool>();
    }
    static int required_integer(const nlohmann::json& value, const char* key, int maximum) {
        if (!value.contains(key) || !value.at(key).is_number_integer())
            throw std::runtime_error(std::string("Missing exchange integer: ") + key);
        const auto& number = value.at(key);
        if ((number.is_number_unsigned() && number.get<std::uint64_t>() > static_cast<std::uint64_t>(maximum)) ||
            (!number.is_number_unsigned() && (number.get<std::int64_t>() < 0 || number.get<std::int64_t>() > maximum)))
            throw std::runtime_error(std::string("Exchange integer outside supported range: ") + key);
        return number.get<int>();
    }
    static ExchangeDecimal required_decimal(const nlohmann::json& value, const char* key) {
        const auto text = required_string(value, key);
        if (text.size() > 128) throw std::runtime_error("Exchange decimal is too large");
        const auto result = ExchangeDecimal::parse(text);
        (void)result.value();
        return result;
    }
    static std::set<std::string> string_set(const nlohmann::json& value, const char* field) {
        if (!value.is_array() || value.empty()) throw std::runtime_error(std::string("Invalid exchange ") + field);
        std::set<std::string> result;
        for (const auto& item : value)
            if (!item.is_string() || item.get_ref<const std::string&>().empty() || !result.insert(item.get<std::string>()).second)
                throw std::runtime_error(std::string("Invalid exchange ") + field);
        return result;
    }
    void validate_quantity_decimal(const ExchangeDecimal& quantity) const {
        if (quantity_step_.zero()) throw std::logic_error("Exchange rules have not been loaded");
        if (quantity.zero() || quantity.scale() > static_cast<unsigned>(base_precision))
            throw std::runtime_error("Quantity has invalid exchange precision");
        if (ExchangeDecimal::compare(quantity, quantity_minimum_) < 0) throw std::runtime_error("Quantity below exchange minimum");
        if (!quantity_maximum_.zero() && ExchangeDecimal::compare(quantity, quantity_maximum_) > 0)
            throw std::runtime_error("Quantity above exchange maximum");
    }
    ExchangeDecimal quantity_minimum_;
    ExchangeDecimal quantity_maximum_;
    ExchangeDecimal quantity_step_;
    std::vector<NotionalRule> notional_rules_;
    std::vector<std::set<std::string>> permission_sets_;
};

} // namespace sentum::execution
