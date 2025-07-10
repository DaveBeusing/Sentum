#include "../include/logger.hpp"
#include <fstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <iostream>

std::string current_time_string() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void log_trade(const std::string& action, const std::string& symbol, double price, double quantity) {
    std::ofstream file("trades.csv", std::ios::app);
    file << current_time_string() << "," << action << "," << symbol << "," << price << "," << quantity << "\n";
    file.close();
    std::cout << "[LOG] " << action << " " << quantity << " " << symbol << " @ $" << price << "\n";
}
