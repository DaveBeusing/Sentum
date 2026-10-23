#pragma once

#include <mutex>
#include <stdexcept>
#include <string>

#include <sqlite3.h>
#include <sentum/trader/types/TradePosition.hpp>

class TradeHistoryRepository {
public:
    class FinalizationError : public std::runtime_error {
    public:
        FinalizationError(const std::string& message, bool recovery_recorded, bool history_committed)
            : std::runtime_error(message), recovery_recorded_(recovery_recorded), history_committed_(history_committed) {}
        // False means durability was not confirmed, not proof that no bytes reached storage.
        bool recovery_recorded() const noexcept { return recovery_recorded_; }
        bool history_committed() const noexcept { return history_committed_; }
    private:
        bool recovery_recorded_;
        bool history_committed_;
    };

    class Finalization {
    public:
        ~Finalization();
        Finalization(const Finalization&) = delete;
        Finalization& operator=(const Finalization&) = delete;
        void complete(const TradePosition& position);
    private:
        friend class TradeHistoryRepository;
        explicit Finalization(TradeHistoryRepository& owner);
        TradeHistoryRepository& owner_;
        std::unique_lock<std::mutex> lock_;
        bool active_ = true;
    };

    explicit TradeHistoryRepository(const std::string& path, int busy_timeout_ms = 5000);
    ~TradeHistoryRepository();
    TradeHistoryRepository(const TradeHistoryRepository&) = delete;
    TradeHistoryRepository& operator=(const TradeHistoryRepository&) = delete;

    // Reserve the history writer before producing a simulated exit fill.
    Finalization begin_finalization() { return Finalization(*this); }
    void save(const TradePosition& position);
    static std::string new_finalization_id();
    const std::string& recovery_path() const noexcept { return recovery_path_; }

private:
    friend struct TradeFinalizationTestAccess;
    void recover_pending();
    void record_pending(const TradePosition& position, const std::string& payload);
    bool already_committed(const std::string& id, const std::string& payload);
    void insert_trade(const TradePosition& position, const std::string& payload);
    void remove_pending(const std::string& id);
    void rollback() noexcept;
    sqlite3* db_ = nullptr;
    sqlite3* journal_ = nullptr;
    std::mutex mutex_;
    std::string recovery_path_;
};
