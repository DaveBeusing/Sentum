#pragma once

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace sentum::paper {

struct PaperAccountTestAccess;

class PaperAccount {
public:
    class PersistenceError : public std::runtime_error {
    public:
        PersistenceError(std::string message, bool state_uncertain = false)
            : std::runtime_error(std::move(message)), state_uncertain_(state_uncertain) {}
        bool state_uncertain() const noexcept { return state_uncertain_; }
    private:
        bool state_uncertain_;
    };

    PaperAccount(std::string state_path, std::string currency, double initial_balance)
        : currency_(std::move(currency)), initial_balance_(initial_balance), equity_(initial_balance) {
        if (state_path.empty() || state_path.find('\0') != std::string::npos ||
            currency_.empty() || !std::isfinite(initial_balance_) || initial_balance_ <= 0.0)
            throw std::invalid_argument("Paper account requires a path, currency and finite positive initial balance");
        path_ = std::filesystem::absolute(state_path).lexically_normal();
        if (path_.filename().empty() || path_.filename() == "." || path_.filename() == "..")
            throw std::invalid_argument("Paper account state path must name a file");
        load();
    }

    double equity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        require_certain_state();
        return equity_;
    }
    double initial_balance() const noexcept { return initial_balance_; }
    const std::string& currency() const noexcept { return currency_; }

    void apply_realized_profit(double delta) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_certain_state();
        if (!std::isfinite(delta) || !std::isfinite(equity_ + delta) ||
            !std::isfinite(realized_profit_ + delta) || closed_trades_ == std::numeric_limits<int>::max())
            throw std::invalid_argument("Paper account profit or trade count is outside its finite range");
        const double next_equity = equity_ + delta;
        const double next_profit = realized_profit_ + delta;
        const int next_trades = closed_trades_ + 1;
        save_locked(make_snapshot(next_equity, next_profit, next_trades));
        equity_ = next_equity;
        realized_profit_ = next_profit;
        closed_trades_ = next_trades;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        require_certain_state();
        save_locked(make_snapshot(initial_balance_, 0.0, 0));
        equity_ = initial_balance_;
        realized_profit_ = 0.0;
        closed_trades_ = 0;
    }

    nlohmann::json snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        require_certain_state();
        return make_snapshot(equity_, realized_profit_, closed_trades_);
    }

private:
    friend struct PaperAccountTestAccess;

    // Per-instance system-call seams support I/O fault tests without a runtime
    // setting that can bypass persistence.
    struct FileOperations {
        ssize_t (*write)(int, const void*, std::size_t) = ::write;
        int (*sync)(int) = ::fsync;
        int (*replace)(int, const char*, int, const char*) = ::renameat;
    } file_operations_;

    struct FileDescriptor {
        explicit FileDescriptor(int value) : value(value) {}
        ~FileDescriptor() { if (value >= 0) ::close(value); }
        FileDescriptor(const FileDescriptor&) = delete;
        FileDescriptor& operator=(const FileDescriptor&) = delete;
        int release() noexcept { return std::exchange(value, -1); }
        int value;
    };

    struct TemporaryPath {
        explicit TemporaryPath(int directory) : directory(directory) {}
        ~TemporaryPath() { if (remove_on_exit && !name.empty()) ::unlinkat(directory, name.c_str(), 0); }
        TemporaryPath(const TemporaryPath&) = delete;
        TemporaryPath& operator=(const TemporaryPath&) = delete;
        int directory;
        std::string name;
        bool remove_on_exit = true;
    };

    [[noreturn]] void io_error(const std::string& operation, int error) const {
        throw PersistenceError("Paper account " + operation + " failed for " + path_.string() +
                               ": " + std::error_code(error, std::generic_category()).message());
    }

    void require_certain_state() const {
        if (state_uncertain_)
            throw PersistenceError("Paper account durable state is uncertain; stop and reconcile " + path_.string(), true);
    }

    int open_directory() const {
        const int directory = ::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory < 0) io_error("opening state directory", errno);
        return directory;
    }

    void sync(int descriptor, const std::string& operation) const {
        int result;
        do { result = file_operations_.sync(descriptor); } while (result < 0 && errno == EINTR);
        if (result < 0) io_error(operation, errno);
    }

    void prepare_directory() const {
        if (!std::filesystem::create_directories(path_.parent_path())) return;
        // A newly created state directory is itself a directory entry. Sync its
        // ancestors too, so a successful first save survives loss of power.
        for (auto parent = path_.parent_path(); !parent.empty();) {
            FileDescriptor directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (directory.value < 0) io_error("opening newly created directory", errno);
            sync(directory.value, "syncing newly created directory");
            const auto next = parent.parent_path();
            if (next == parent) break;
            parent = next;
        }
    }

    static bool finite_number(const nlohmann::json& value) {
        return value.is_number() && std::isfinite(value.get<double>());
    }

    void load() {
        prepare_directory();
        FileDescriptor directory(open_directory());
        const auto filename = path_.filename().string();
        FileDescriptor file(::openat(directory.value, filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (file.value < 0) {
            if (errno != ENOENT) io_error("opening existing state", errno);
            save_locked(make_snapshot(initial_balance_, 0.0, 0), true);
            return;
        }
        struct stat status {};
        if (::fstat(file.value, &status) < 0) io_error("inspecting existing state", errno);
        if (!S_ISREG(status.st_mode)) throw PersistenceError("Paper account state is not a regular file: " + path_.string());
        std::string contents;
        char buffer[4096];
        for (;;) {
            const auto count = ::read(file.value, buffer, sizeof(buffer));
            if (count < 0) {
                if (errno == EINTR) continue;
                io_error("reading existing state", errno);
            }
            if (count == 0) break;
            contents.append(buffer, static_cast<std::size_t>(count));
            if (contents.size() > 65536) throw PersistenceError("Paper account state exceeds 64 KiB: " + path_.string());
        }
        try {
            const auto json = nlohmann::json::parse(contents);
            if (!json.is_object() || !json.contains("currency") || !json["currency"].is_string() ||
                json["currency"].get<std::string>() != currency_ ||
                !json.contains("initial_balance") || !finite_number(json["initial_balance"]) ||
                json["initial_balance"].get<double>() != initial_balance_ ||
                !json.contains("equity") || !finite_number(json["equity"]) ||
                !json.contains("realized_profit") || !finite_number(json["realized_profit"]) ||
                !json.contains("closed_trades") || !json["closed_trades"].is_number_integer() ||
                json["closed_trades"].get<double>() < 0.0 ||
                json["closed_trades"].get<double>() > std::numeric_limits<int>::max())
                throw std::runtime_error("required fields, currency, initial balance or numeric ranges are invalid");
            equity_ = json["equity"].get<double>();
            realized_profit_ = json["realized_profit"].get<double>();
            closed_trades_ = json["closed_trades"].get<int>();
        } catch (const std::exception& error) {
            throw PersistenceError("Invalid existing paper account state at " + path_.string() + ": " + error.what());
        }
    }

    std::string temporary_name() const {
        std::uint64_t value = 0;
        std::size_t received = 0;
        while (received < sizeof(value)) {
            const auto count = ::getrandom(reinterpret_cast<char*>(&value) + received, sizeof(value) - received, 0);
            if (count < 0) {
                if (errno == EINTR) continue;
                io_error("generating temporary name", errno);
            }
            if (count == 0) io_error("generating temporary name", EIO);
            received += static_cast<std::size_t>(count);
        }
        return ".sentum-paper-" + std::to_string(value) + ".tmp";
    }

    int create_temporary(TemporaryPath& temporary) const {
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto name = temporary_name();
            const int file = ::openat(temporary.directory, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (file >= 0) { temporary.name = name; return file; }
            if (errno != EEXIST) io_error("creating temporary state", errno);
        }
        io_error("creating unique temporary state", EEXIST);
    }

    void retain_previous(TemporaryPath& previous, const std::string& filename) const {
        struct stat status {};
        if (::fstatat(previous.directory, filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) < 0)
            io_error("inspecting previous state", errno);
        if (!S_ISREG(status.st_mode)) throw PersistenceError("Paper account state is no longer a regular file: " + path_.string());
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto name = temporary_name();
            if (::linkat(previous.directory, filename.c_str(), previous.directory, name.c_str(), 0) == 0) {
                previous.name = name;
                sync(previous.directory, "syncing previous state link");
                return;
            }
            if (errno != EEXIST) io_error("retaining previous state", errno);
        }
        io_error("retaining previous state under unique name", EEXIST);
    }

    void save_locked(const nlohmann::json& next, bool first_start = false) {
        const auto contents = next.dump(2) + '\n';
        const auto filename = path_.filename().string();
        FileDescriptor directory(open_directory());
        TemporaryPath temporary(directory.value);
        TemporaryPath previous(directory.value);
        FileDescriptor file(create_temporary(temporary));
        for (std::size_t written = 0; written < contents.size();) {
            const auto count = file_operations_.write(file.value, contents.data() + written, contents.size() - written);
            if (count < 0) {
                if (errno == EINTR) continue;
                io_error("writing temporary state", errno);
            }
            if (count == 0) io_error("writing temporary state", EIO);
            written += static_cast<std::size_t>(count);
        }
        sync(file.value, "syncing temporary state");
        if (::close(file.release()) < 0) io_error("closing temporary state", errno);

        if (first_start) {
            // Never overwrite a state created after load observed ENOENT.
            if (::linkat(directory.value, temporary.name.c_str(), directory.value, filename.c_str(), 0) < 0)
                io_error("publishing initial state", errno);
        } else {
            retain_previous(previous, filename);
            if (file_operations_.replace(directory.value, temporary.name.c_str(), directory.value, filename.c_str()) < 0)
                io_error("replacing state", errno);
            temporary.remove_on_exit = false;
        }

        try {
            sync(directory.value, "syncing state replacement");
        } catch (const PersistenceError& error) {
            // A visible replacement is not committed until its directory sync.
            // Retain the old inode until restoration and another sync succeed.
            const bool restored = first_start
                ? ::unlinkat(directory.value, filename.c_str(), 0) == 0
                : file_operations_.replace(directory.value, previous.name.c_str(), directory.value, filename.c_str()) == 0;
            if (restored) {
                previous.remove_on_exit = false;
                try { sync(directory.value, "syncing restored state"); }
                catch (const PersistenceError&) { state_uncertain_ = true; }
            } else {
                state_uncertain_ = true;
                previous.remove_on_exit = false;
            }
            if (state_uncertain_) {
                const std::string retained = !restored && !previous.name.empty()
                    ? "; previous state retained at " + (path_.parent_path() / previous.name).string() : "";
                throw PersistenceError(std::string(error.what()) + "; recovery durability is uncertain" + retained, true);
            }
            throw;
        }
        // The account transaction is committed. Cleanup failure may leave an
        // owned temporary file, but must not report a failed account update.
        if (temporary.remove_on_exit && !temporary.name.empty() && ::unlinkat(directory.value, temporary.name.c_str(), 0) == 0)
            temporary.remove_on_exit = false;
        if (previous.remove_on_exit && !previous.name.empty() && ::unlinkat(directory.value, previous.name.c_str(), 0) == 0)
            previous.remove_on_exit = false;
        ::fsync(directory.value);
    }

    nlohmann::json make_snapshot(double equity, double profit, int trades) const {
        return {{"currency", currency_}, {"initial_balance", initial_balance_}, {"equity", equity},
                {"realized_profit", profit}, {"closed_trades", trades}};
    }

    std::filesystem::path path_;
    std::string currency_;
    double initial_balance_ = 0.0;
    mutable std::mutex mutex_;
    double equity_ = 0.0;
    double realized_profit_ = 0.0;
    int closed_trades_ = 0;
    bool state_uncertain_ = false;
};

} // namespace sentum::paper
