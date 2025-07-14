#pragma once
#include <string>
#include <functional>

void start_ws_price_stream(const std::string& symbol, std::function<void(double)> on_price);
