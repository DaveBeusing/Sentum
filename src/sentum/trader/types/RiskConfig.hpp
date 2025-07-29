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

struct RiskConfig {

	double max_total_capital = 1000.0;		// z. B. Gesamtbudget
	double risk_per_trade = 0.1;			// z. B. 10 % pro Trade
	double stop_loss_percent = 0.02;		// z. B. 2 % SL
	double take_profit_percent = 0.04;		// z. B. 4 % TP 
	bool trailing_sl_enabled = false;		// activate trailing SL
	double trailing_sl_percent = 0.01;		// +1%
	bool trailing_tp_enabled = false;		// activate trailing TP
	double trailing_tp_percent = 0.02;		// +2%
	double buy_fee_percent = 0.001;			// 0.1 % Binance standard
	double sell_fee_percent = 0.001;		// 0.1 % Binance standard
	double leverage = 1.0;
	
};