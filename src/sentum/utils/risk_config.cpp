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

#include "sentum/utils/risk_config.hpp"
#include "nlohmann/json.hpp"
#include <fstream>
#include <iostream>

RiskConfig load_risk_config(const std::string& path) {
	RiskConfig config;
	std::ifstream file(path);
	if (!file.is_open()) {
		std::cerr << " ⚠️  Error can't open risk.json : " << path << std::endl;
		return config;
	}
	try {
		nlohmann::json json;
		file >> json;
		config.max_total_usdt      = json.value("max_total_usdt", 1000.0);
		config.risk_per_trade      = json.value("risk_per_trade", 0.1);
		config.stop_loss_percent   = json.value("stop_loss_percent", 0.03);
		config.take_profit_percent = json.value("take_profit_percent", 0.05);
		config.fee_percent         = json.value("fee_percent", 0.001);
	} catch (const std::exception& e) {
		std::cerr << " ⚠️  Error can't parse risk.json: " << e.what() << std::endl;
	}
	return config;
}