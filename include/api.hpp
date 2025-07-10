#pragma once
#include <string>

std::string send_signed_order(const std::string& symbol, const std::string& quantity);
double get_current_price(const std::string& symbol);