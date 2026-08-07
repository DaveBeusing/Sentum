#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/backtest/Backtest.hpp>

namespace sentum::research {

struct DatasetEntry {
    std::string id;
    std::string symbol;
    std::string path;
    std::string interval = "1s";
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::vector<std::string> tags;
};

struct DatasetSelection {
    DatasetEntry dataset;
    std::int64_t from_ms = 0;
    std::int64_t to_ms = 0;
};

class DatasetCatalog {
public:
    static DatasetCatalog load(const std::string& path) {
        std::ifstream file(path);
        if (!file) throw std::runtime_error("Cannot open dataset catalog: " + path);
        nlohmann::json json;
        file >> json;
        if (!json.contains("datasets") || !json.at("datasets").is_array())
            throw std::runtime_error("Dataset catalog requires a datasets array");

        DatasetCatalog catalog;
        for (const auto& item : json.at("datasets")) {
            DatasetEntry entry;
            entry.id = item.value("id", std::string{});
            entry.symbol = item.value("symbol", std::string{});
            entry.path = item.value("path", std::string{});
            entry.interval = item.value("interval", std::string("1s"));
            entry.start_ms = item.value("start_ms", std::int64_t{0});
            entry.end_ms = item.value("end_ms", std::int64_t{0});
            if (item.contains("tags")) entry.tags = item.at("tags").get<std::vector<std::string>>();
            if (entry.id.empty() || entry.symbol.empty() || entry.path.empty())
                throw std::runtime_error("Every dataset requires id, symbol and path");
            if (std::any_of(catalog.entries_.begin(), catalog.entries_.end(), [&](const auto& e) { return e.id == entry.id; }))
                throw std::runtime_error("Duplicate dataset id: " + entry.id);
            catalog.entries_.push_back(std::move(entry));
        }
        return catalog;
    }

    const DatasetEntry& by_id(const std::string& id) const {
        const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& e) { return e.id == id; });
        if (it == entries_.end()) throw std::runtime_error("Unknown dataset id: " + id);
        return *it;
    }

    std::vector<DatasetEntry> by_symbol(const std::string& symbol) const {
        std::vector<DatasetEntry> out;
        for (const auto& entry : entries_) if (entry.symbol == symbol) out.push_back(entry);
        return out;
    }

    const std::vector<DatasetEntry>& entries() const noexcept { return entries_; }

    static std::vector<MarketEvent> load_selection(const DatasetSelection& selection) {
        auto events = HistoricalEventReader::read_csv(selection.dataset.path, selection.dataset.symbol);
        const auto from = selection.from_ms > 0 ? selection.from_ms : selection.dataset.start_ms;
        const auto to = selection.to_ms > 0 ? selection.to_ms : selection.dataset.end_ms;
        if (from <= 0 && to <= 0) return events;
        events.erase(std::remove_if(events.begin(), events.end(), [&](const auto& event) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(event.timestamp.time_since_epoch()).count();
            return (from > 0 && ms < from) || (to > 0 && ms > to);
        }), events.end());
        if (events.empty()) throw std::runtime_error("Dataset selection contains no events: " + selection.dataset.id);
        return events;
    }

    static void write_selection_csv(const DatasetSelection& selection, const std::string& output_path) {
        const auto events = load_selection(selection);
        const auto parent = std::filesystem::path(output_path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        std::ofstream out(output_path, std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot write dataset slice: " + output_path);
        out << "timestamp_ms,price,volume\n";
        out.precision(17);
        for (const auto& event : events) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(event.timestamp.time_since_epoch()).count();
            out << ms << ',' << event.price << ',' << event.volume << '\n';
        }
    }

private:
    std::vector<DatasetEntry> entries_;
};

} // namespace sentum::research
