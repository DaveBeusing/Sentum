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

#include "ui.hpp"
#include <iostream>
#include <chrono>
#include <thread>

namespace ui {

	std::string reset()			{ return "\033[0m"; }
	std::string bold()			{ return "\033[1m"; }
	std::string red()			{ return "\033[31m"; }
	std::string green()			{ return "\033[32m"; }
	std::string yellow()		{ return "\033[33m"; }
	std::string blue()			{ return "\033[34m"; }

	std::string underline()		{ return "\033[4m"; }
	std::string bg_red()		{ return "\033[41m"; }

	std::string bold_red()		{ return bold() + red(); }
	std::string bold_green()	{ return bold() + green(); }
	std::string bold_yellow()	{ return bold() + yellow(); }

	std::string wrap(const std::string& text, const std::string& style_code) {
		return style_code + text + reset();
	}

	void clear_terminal() {
		std::cout << "\033[2J\033[1;1H";
	}

	void countdown_progress(int seconds) {
		const int bar_width = 30;
		for (int i = seconds; i >= 0; --i) {
			float progress = (float)(seconds - i) / seconds;
			int filled = static_cast<int>(progress * bar_width);
			std::cout << "\r[";
			for (int j = 0; j < bar_width; ++j) {
				if (j < filled) std::cout << "█";
				else if (j == filled) std::cout << "▏";
				else std::cout << " ";
			}
			std::cout << "] " << i << "s remaining..." << std::flush;
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		std::cout << "\n";
	}

}