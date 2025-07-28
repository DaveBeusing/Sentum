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

#include <sstream>
#include <unordered_map>

#include <sentum/trader/TradeValidator.hpp>

bool TradeValidator::validate_query_string(const std::string& query, std::vector<std::string>& errors) {
	std::unordered_map<std::string, std::string> params;
	std::istringstream stream(query);
	std::string token;

	while (std::getline(stream, token, '&')) {
		auto pos = token.find('=');
		if (pos != std::string::npos) {
			std::string key = token.substr(0, pos);
			std::string val = token.substr(pos + 1);
			params[key] = val;
		}
	}

	// Pflichtfelder prüfen
	if (!params.count("symbol")) errors.push_back("Symbol missing");
	if (!params.count("side")) errors.push_back("Side missing");
	if (!params.count("type")) errors.push_back("Order-Type missing");
	if (!params.count("quantity")) errors.push_back("QTY missing");
	if (!params.count("timestamp")) errors.push_back("Timestamp missing");

	// Logik prüfen
	if (params.count("quantity")) {
		try {
			double qty = std::stod(params["quantity"]);
			if (qty <= 0.0) errors.push_back("QTY must be positive");
		} catch (...) {
			errors.push_back("Quantity is not a valid numerical value");
		}
	}

	// Sonderfall: LIMIT-Order muss Preis enthalten
	if (params["type"] == "LIMIT" && !params.count("price")) {
		errors.push_back("LIMIT-Order without price specification");
	}

	return errors.empty();
}
