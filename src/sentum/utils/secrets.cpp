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

#include "secrets.hpp"
#include "nlohmann/json.hpp"
#include <fstream>
#include <iostream>

Secrets load_secrets(const std::string& path) {
	Secrets s;
	std::ifstream file(path);
	if (!file.is_open()) {
		std::cerr << " ⚠️  Error can't open secrets.json: " << path << std::endl;
		return s;
	}
	try {
		nlohmann::json json;
		file >> json;
		s.api_key = json.value("api_key", "");
		s.api_secret = json.value("api_secret", "");
	} catch (const std::exception& e) {
		std::cerr << " ⚠️  Error parsing secrets.json: " << e.what() << std::endl;
	}
	return s;
}