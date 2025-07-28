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

#include <memory>
#include <string>
#include <vector>
#include <algorithm>

#include <sentum/strategy/IStrategy.hpp>
#include <sentum/strategy/SmaStrategy.hpp>
#include <sentum/strategy/RsiStrategy.hpp>

class StrategyFactory {

	public:
		static std::unique_ptr<IStrategy> create(const std::string& type, const std::vector<int>& params = {}) {
			std::string t = to_lower(type);
			if (t == "sma") {
				int short_p = params.size() > 0 ? params[0] : 5;
				int long_p  = params.size() > 1 ? params[1] : 20;
				return std::make_unique<SmaStrategy>(short_p, long_p);
			}
			if (t == "rsi") {
				int period = params.size() > 0 ? params[0] : 14;
				return std::make_unique<RsiStrategy>(period);
			}
			return nullptr; // fallback
		}

	private:
		static std::string to_lower(const std::string& str) {
			std::string result = str;
			std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
			return result;
		}

};