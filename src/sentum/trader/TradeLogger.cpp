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

#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>

#include <sentum/trader/TradeLogger.hpp>

using json = nlohmann::json;

TradeLogger::TradeLogger(const std::string& file_path) : log_file(file_path) {}

void TradeLogger::log(const TradePosition& position, TradeAction action) {
	auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::stringstream timestamp;
	timestamp << std::put_time(std::localtime(&now), "%Y-%m-%d %H:%M:%S");
	std::string action_str = (action == TradeAction::BUY ? "BUY" : "SELL");
	json j{
		{ "timestamp", timestamp.str() },
		{ "action", action_str },
		{ "symbol", position.symbol },
		{ "entry_price", position.entry_price },
		{ "executed_price", position.executed_price },
		{ "exit_price", position.exit_price },
		{ "quantity", position.quantity },
		{ "gross_profit", position.gross_profit },
		{ "net_profit", position.net_profit },
		{ "fee_entry", position.fee_entry },
		{ "fee_exit", position.fee_exit },
		{ "buy_fee_percent", position.buy_fee_percent },
		{ "sell_fee_percent", position.sell_fee_percent },
		{ "stop_loss_price", position.stop_loss_price },
		{ "take_profit_price", position.take_profit_price },
		{ "stop_loss_triggered", position.stop_loss_triggered },
		{ "take_profit_triggered", position.take_profit_triggered },
		{ "highest_price", position.highest_price },
		{ "lowest_price", position.lowest_price },
		{ "risk_per_trade", position.risk_per_trade },
		{ "stop_loss_percent", position.stop_loss_percent },
		{ "take_profit_percent", position.take_profit_percent },
		{ "trailing_sl_enabled", position.trailing_sl_enabled },
		{ "trailing_sl_percent", position.trailing_sl_percent },
		{ "trailing_tp_enabled", position.trailing_tp_enabled },
		{ "trailing_tp_percent", position.trailing_tp_percent },
		{ "open", position.open },
		{ "simulated", position.simulated },
		{ "notes", position.notes },
		{ "holding_seconds", position.holding_seconds() }
	};
	std::ofstream out(log_file, std::ios::app);
	out << j.dump() << "\n";
	out.close();
}