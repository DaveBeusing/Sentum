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

#include <chrono>

#include <sentum/utils/AsyncLogger.hpp>

class RiskManager {

	public:
		RiskManager(double maxDailyLoss, double maxTradeLoss, int maxTradesPerDay);

		// Prüft, ob ein Trade basierend auf potenziellem Verlust erlaubt ist
		bool allow_trade(double potential_loss);

		// Nach Abschluss eines Trades aufrufen, um Verlust und Zähler zu erfassen
		void record_trade(double pnl);

		// Prüft, ob ein Tageswechsel stattgefunden hat (zurücksetzen der Limits)
		void reset_day_if_needed();

		// Statistiken für Anzeige / Logging
		double get_daily_loss() const;
		int get_trades_today() const;

	private:
		double max_daily_loss;
		double max_trade_loss;
		int max_trades_per_day;

		double daily_loss;
		int trades_today;
		std::chrono::system_clock::time_point last_reset;



};