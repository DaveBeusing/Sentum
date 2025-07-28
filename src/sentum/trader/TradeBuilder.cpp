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

#include <sentum/trader/TradeBuilder.hpp>

TradeBuilder& TradeBuilder::symbol(const std::string& value) {
	params_["symbol"] = value; return *this;
}
TradeBuilder& TradeBuilder::side(const std::string& value) {
	params_["side"] = value; return *this;
}
TradeBuilder& TradeBuilder::type(const std::string& value) {
	params_["type"] = value; return *this;
}
TradeBuilder& TradeBuilder::quantity(const std::string& value) {
	params_["quantity"] = value; return *this;
}
TradeBuilder& TradeBuilder::price(const std::string& value) {
	params_["price"] = value; return *this;
}
TradeBuilder& TradeBuilder::time_in_force(const std::string& value) {
	params_["timeInForce"] = value; return *this;
}
TradeBuilder& TradeBuilder::timestamp(const std::string& value) {
	params_["timestamp"] = value; return *this;
}

std::string TradeBuilder::build() const {
	std::ostringstream oss;
	for (auto it = params_.begin(); it != params_.end(); ++it) {
		if (it != params_.begin()) oss << "&";
		oss << it->first << "=" << it->second;
	}
	return oss.str();
}
