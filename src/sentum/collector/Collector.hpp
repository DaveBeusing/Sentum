/****
 * Copyright (C) 2025 Dave Beusing <david.beusing@gmail.com>
 * MIT License - https://opensource.org/license/mit/
 */
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sentum/utils/Database.hpp>
#include <sentum/utils/AsyncLogger.hpp>
#include <sentum/api/model/MarketInfo.hpp>

class Collector {
public:
	Collector(Database& db, const std::vector<MarketInfo>& markets);
	~Collector();
	void start();
	void stop();

private:
	struct Impl;
	void run();

	Database& db_ref;
	std::vector<MarketInfo> markets;
	std::thread ws_thread;
	std::atomic<bool> running;
	AsyncLogger logger;
	std::unique_ptr<Impl> impl;
};
