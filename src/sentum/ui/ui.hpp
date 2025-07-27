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
#include <sstream>
#include <iomanip>
#include <chrono>

namespace ui {

	std::string reset();
	std::string bold();
	std::string red();
	std::string green();
	std::string yellow();
	std::string blue();
	std::string underline();
	std::string bg_red();
	std::string bold_red();
	std::string bold_green();
	std::string bold_yellow();

	std::string wrap(const std::string& text, const std::string& style_code);

	template<typename T>
	std::string wrap_value(T value, const std::string& style_code) {
		return style_code + std::to_string(value) + reset();
	}

	template<typename T>
	std::string wrap_value_fixed(T value, const std::string& style_code, int precision = 2) {
		std::ostringstream oss;
		oss << std::fixed << std::setprecision(precision) << value;
		return style_code + oss.str() + reset();
	}

	std::string format_duration(std::chrono::seconds s);
	std::string format_datetime(const std::chrono::system_clock::time_point& tp);

	void show_header();
	void clear_terminal();
	void countdown_progress(int seconds);
}