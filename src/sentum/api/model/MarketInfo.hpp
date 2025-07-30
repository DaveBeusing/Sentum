/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * 
 * MIT License - https://opensource.org/license/mit/
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the “Software”), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is furnished 
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all 
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A 
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION 
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <algorithm>

struct MarketInfo {
	std::string symbol;
	std::string base_asset;
	int base_asset_precision;
	std::string quote_asset;
	int quote_asset_precision;

	static std::optional<MarketInfo> find_by_symbol(const std::vector<MarketInfo>& markets, const std::string& target_symbol) {
		auto it = std::find_if(markets.begin(), markets.end(), [&target_symbol](const MarketInfo& m) {
			return m.symbol == target_symbol;
		});
		if (it != markets.end()) return *it;
		return std::nullopt;
	}

	static std::vector<std::string> get_symbol_list(const std::vector<MarketInfo>& markets) {
		std::vector<std::string> symbols;
		symbols.reserve(markets.size());
		for (const auto& m : markets) {
			symbols.push_back(m.symbol);
		}
		return symbols;
	}
};