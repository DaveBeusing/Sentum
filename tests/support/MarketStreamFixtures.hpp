#pragma once

#include <sentum/api/BinanceWebsocketClient.hpp>
#include <sentum/collector/Collector.hpp>

struct MarketStreamTestAccess {
    static bool joining(Collector& collector) {
        std::lock_guard<std::mutex> lock(collector.lifecycle_mutex);
        return collector.joining;
    }

    static bool joined(Collector& collector) {
        std::lock_guard<std::mutex> lock(collector.lifecycle_mutex);
        return !collector.ws_thread.joinable() && !collector.writer_thread.joinable();
    }

    static bool retained_canonical_batch(const Collector& collector, std::size_t expected) {
        if (collector.pending_batch.size() != expected) return false;
        for (const auto& item : collector.pending_batch) {
            if (item.symbol != &collector.canonical_symbols.front()) return false;
        }
        return true;
    }

    static void before_connect(Collector& collector, std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(collector.lifecycle_mutex);
        if (collector.ws_thread.joinable()) throw std::logic_error("Configure fixture before starting worker");
        collector.before_connect = std::move(callback);
    }

    static sentum::api::MarketStreamSettings settings() {
        sentum::api::MarketStreamSettings result;
        result.initial_backoff = std::chrono::milliseconds(75);
        result.maximum_backoff = std::chrono::milliseconds(300);
        result.connect_timeout = std::chrono::milliseconds(750);
        result.maximum_event_age = std::chrono::milliseconds(500);
        result.idle_reconnect_timeout = std::chrono::milliseconds(1500);
        return result;
    }

    static void configure(BinanceWebsocketClient& client, const std::string& url,
                          const std::string& hostname = "localhost",
                          sentum::api::MarketStreamSettings options = settings()) {
        std::lock_guard<std::mutex> lock(client.lifecycle_mutex);
        if (client.ws_thread.joinable()) throw std::logic_error("Configure fixture before starting worker");
        client.test_url = url;
        client.test_hostname = hostname;
        client.stream_settings = options;
    }

    static void configure(Collector& collector, const std::string& url,
                          const std::string& hostname = "localhost",
                          sentum::api::MarketStreamSettings options = settings()) {
        std::lock_guard<std::mutex> lock(collector.lifecycle_mutex);
        if (collector.ws_thread.joinable()) throw std::logic_error("Configure fixture before starting worker");
        collector.test_url = url;
        collector.test_hostname = hostname;
        collector.stream_settings = options;
    }
};
