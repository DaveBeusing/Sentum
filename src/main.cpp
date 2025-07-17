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

#include "sentum/utils/database.hpp"
#include "sentum/scanner/scanner.hpp"
#include "sentum/collector/collector.hpp"

#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>


Database db("log/klines.sqlite3");
std::vector<std::string> symbols = {"btcusdc", "ethusdc", "shibusdc"};

Collector collector(db, symbols);
SymbolScanner scanner(db);

std::atomic<bool> scanning = false;
std::thread scanner_thread;


// Clear Terminal (ANSI)
void clear_terminal() {
	std::cout << "\033[2J\033[1;1H";
}

void scanner_loop() {
	std::cout << "\n⏳ Scanner startet in 60 Sekunden...\n";
	// Countdown anzeigen
	for (int i = 60; i > 0; --i) {
		clear_terminal();
		std::cout << "\r⌛ Starte in " << i << "s..." << std::flush;
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	std::cout << "\n🚀 Scanner gestartet!\n";
	while (scanning) {
		auto top = scanner.fetch_top_performers(1, 5);  // 1 Min Lookback, max 5 Symbole
		std::cout << "\n🔍 Top-Performer:\n";
		for (const auto& s : top) {
			std::cout << " 🔼 " << s.symbol << " → " << (s.cum_return * 100) << " %\n";
		}
		std::this_thread::sleep_for(std::chrono::seconds(30));  // alle 30s neu
	}
	std::cout << "🛑 Scanner beendet.\n";
}

int main() {
	bool running = true;
	while (running) {
		std::cout << "\n🧠 Trading Bot Menü\n";
		std::cout << "1. Collector starten\n";
		std::cout << "2. Collector stoppen\n";
		std::cout << "3. Scanner starten\n";
		std::cout << "4. Scanner stoppen\n";
		std::cout << "0. Beenden\n> ";

		int choice;
		std::cin >> choice;

		switch (choice) {
			case 1:
				std::cout << "🚀 Starte Collector...\n";
				collector.start();
				break;
			case 2:
				std::cout << "🛑 Stoppe Collector...\n";
				collector.stop();
				break;
			case 3:
				if (!scanning) {
					scanning = true;
					scanner_thread = std::thread(scanner_loop);
					std::cout << "🔄 Scanner gestartet.\n";
				}
				break;
			case 4:
				scanning = false;
				if (scanner_thread.joinable()) scanner_thread.join();
				std::cout << "⏹️ Scanner gestoppt.\n";
				break;
			case 0:
				std::cout << "👋 Beende...\n";
				running = false;
				scanning = false;
				collector.stop();
				if (scanner_thread.joinable()) scanner_thread.join();
				break;
			default:
				std::cout << "❌ Ungültige Auswahl\n";
		}
	}
	return 0;
}
