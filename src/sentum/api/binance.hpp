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

struct Kline {
	double open;
	double high;
	double low;
	double close;
	double volume;
};

class Binance {
	public:
		Binance(const std::string& api_key, const std::string& api_secret);
		double get_current_price(const std::string& symbol);
		std::string send_signed_order(const std::string& symbol, const std::string& quantity);
		double get_coin_balance(const std::string& asset_symbol);
		std::vector<Kline> get_historical_klines(const std::string& symbol, const std::string& interval, int limit);

	private:
		std::string api_key_;
		std::string api_secret_;
		std::string get_timestamp() const;
		std::string hmac_sha256(const std::string& data, const std::string& key) const;
};