#include <sentum/trader/paper/PaperAccount.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include <linux/capability.h>
#include <sys/syscall.h>
#include <sys/wait.h>

namespace sentum::paper {

struct PaperAccountTestAccess {
    static inline int write_calls = 0;
    static inline int sync_calls = 0;
    static inline int replace_calls = 0;
    static inline int fail_sync_call = 0;
    static inline int fail_replace_call = 0;
    static inline bool persistent_sync_failure = false;

    static ssize_t partial_write_then_fail(int fd, const void* data, std::size_t size) {
        if (++write_calls == 1) return ::write(fd, data, std::min<std::size_t>(size, 11));
        errno = ENOSPC;
        return -1;
    }

    static ssize_t interrupted_short_write(int fd, const void* data, std::size_t size) {
        if (++write_calls == 1) { errno = EINTR; return -1; }
        return ::write(fd, data, std::min<std::size_t>(size, 7));
    }

    static int failing_sync(int fd) {
        ++sync_calls;
        if (sync_calls == fail_sync_call || (persistent_sync_failure && sync_calls >= fail_sync_call)) {
            errno = EIO;
            return -1;
        }
        return ::fsync(fd);
    }

    static int failing_replace(int from_fd, const char* from, int to_fd, const char* to) {
        if (++replace_calls == fail_replace_call) { errno = EACCES; return -1; }
        return ::renameat(from_fd, from, to_fd, to);
    }

    static void clear(PaperAccount& account) {
        account.file_operations_ = {};
        write_calls = sync_calls = replace_calls = fail_sync_call = fail_replace_call = 0;
        persistent_sync_failure = false;
    }

    static void fail_write(PaperAccount& account) { account.file_operations_.write = partial_write_then_fail; }
    static void short_writes(PaperAccount& account) { account.file_operations_.write = interrupted_short_write; }
    static void fail_sync(PaperAccount& account, int call, bool persistent = false) {
        fail_sync_call = call;
        persistent_sync_failure = persistent;
        account.file_operations_.sync = failing_sync;
    }
    static void fail_replace(PaperAccount& account, int call) {
        fail_replace_call = call;
        account.file_operations_.replace = failing_replace;
    }
    static nlohmann::json raw_snapshot(const PaperAccount& account) {
        return account.make_snapshot(account.equity_, account.realized_profit_, account.closed_trades_);
    }
};

} // namespace sentum::paper

namespace {

using sentum::paper::PaperAccount;
using sentum::paper::PaperAccountTestAccess;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TemporaryDirectory {
    TemporaryDirectory() {
        std::string pattern = (fs::temp_directory_path() / "sentum-paper-account-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const auto* directory = ::mkdtemp(buffer.data());
        if (!directory) throw std::runtime_error("Cannot create test directory");
        path = directory;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    fs::path path;
};

std::string read_bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good(), "Cannot read test state");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write_bytes(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
    require(file.good(), "Cannot write test fixture");
}

nlohmann::json valid_state() {
    return {{"currency", "USDT"}, {"initial_balance", 1000.0}, {"equity", 1050.0},
            {"realized_profit", 50.0}, {"closed_trades", 2}};
}

template <typename Operation>
void persistence_failure(Operation operation, bool uncertain = false) {
    bool failed = false;
    try { operation(); }
    catch (const PaperAccount::PersistenceError& error) {
        failed = true;
        require(error.state_uncertain() == uncertain, "Wrong persistence uncertainty classification");
    }
    require(failed, "Persistence operation silently succeeded");
}

template <typename Operation>
void invalid_argument(Operation operation) {
    bool failed = false;
    try { operation(); }
    catch (const std::invalid_argument&) { failed = true; }
    require(failed, "Invalid numeric input was accepted");
}

std::vector<fs::path> transaction_files(const fs::path& directory) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().filename().string().find(".sentum-paper-") == 0) files.push_back(entry.path());
    return files;
}

void test_first_start_reopen_and_reset() {
    TemporaryDirectory directory;
    const auto state = directory.path / "new" / "nested" / "account.json";
    PaperAccount account(state.string(), "USDT", 1000.0);
    require(account.equity() == 1000.0 && fs::is_regular_file(state), "First start did not persist initial state");
    account.apply_realized_profit(25.0);
    account.apply_realized_profit(-10.0);
    PaperAccount reopened(state.string(), "USDT", 1000.0);
    require(reopened.snapshot() == account.snapshot(), "Successful update did not survive reopen");
    require(reopened.snapshot()["closed_trades"] == 2 && reopened.equity() == 1015.0, "Reopen changed account totals");
    struct stat status {};
    require(::stat(state.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600, "State file is not private to its owner");
    reopened.reset();
    PaperAccount reset(state.string(), "USDT", 1000.0);
    require(reset.equity() == 1000.0 && reset.snapshot()["closed_trades"] == 0 && reset.snapshot()["realized_profit"] == 0.0,
            "Reset was not durable");
    require(transaction_files(state.parent_path()).empty(), "Successful save leaked transaction files");
}

void test_existing_schema_loads_without_version_migration() {
    TemporaryDirectory directory;
    const auto state = directory.path / "account.json";
    const auto bytes = valid_state().dump();
    write_bytes(state, bytes);
    PaperAccount account(state.string(), "USDT", 1000.0);
    require(account.snapshot() == valid_state(), "Existing unversioned schema did not load");
    require(read_bytes(state) == bytes, "Loading existing state rewrote its bytes");
}

void test_corrupt_and_invalid_existing_state_never_resets() {
    TemporaryDirectory directory;
    const auto state = directory.path / "account.json";
    std::vector<std::string> invalid = {"", "{", "[]", "null", valid_state().dump() + " trailing", valid_state().dump() + "{}"};
    for (const auto* field : {"currency", "initial_balance", "equity", "realized_profit", "closed_trades"}) {
        auto value = valid_state();
        value.erase(field);
        invalid.push_back(value.dump());
    }
    for (const auto& currency : {nlohmann::json("EUR"), nlohmann::json(123)}) {
        auto value = valid_state(); value["currency"] = currency; invalid.push_back(value.dump());
    }
    for (const auto* field : {"initial_balance", "equity", "realized_profit"}) {
        for (const auto& number : {nlohmann::json("1000"), nlohmann::json(nullptr), nlohmann::json(true)}) {
            auto value = valid_state(); value[field] = number; invalid.push_back(value.dump());
        }
    }
    for (const auto& count : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json(2147483648ULL), nlohmann::json("2")}) {
        auto value = valid_state(); value["closed_trades"] = count; invalid.push_back(value.dump());
    }
    auto different_initial = valid_state(); different_initial["initial_balance"] = 2000.0; invalid.push_back(different_initial.dump());
    invalid.push_back("{\"currency\":\"USDT\",\"initial_balance\":1000,\"equity\":1e999,\"realized_profit\":0,\"closed_trades\":0}");
    for (const auto& bytes : invalid) {
        write_bytes(state, bytes);
        persistence_failure([&] { PaperAccount account(state.string(), "USDT", 1000.0); });
        require(read_bytes(state) == bytes, "Invalid existing state was reset or rewritten");
        require(transaction_files(directory.path).empty(), "Rejected state created transaction files");
    }
}

void test_unreadable_existing_state_is_not_first_start() {
    TemporaryDirectory directory;
    const auto state = directory.path / "account.json";
    const auto bytes = valid_state().dump();
    write_bytes(state, bytes);
    require(::chmod(directory.path.c_str(), 0755) == 0 && ::chmod(state.c_str(), 0000) == 0, "Cannot set unreadable fixture permissions");
    if (::geteuid() == 0) {
        const auto child = ::fork();
        require(child >= 0, "Cannot fork unreadable-state probe");
        if (child == 0) {
            // Dropping only the child's DAC bypass also works in user
            // namespaces that map root but do not map an unprivileged UID.
            struct __user_cap_header_struct header {};
            header.version = _LINUX_CAPABILITY_VERSION_3;
            struct __user_cap_data_struct capabilities[2] {};
            if (::syscall(SYS_capget, &header, capabilities) != 0) ::_exit(2);
            capabilities[0].effective &= ~((1U << CAP_DAC_OVERRIDE) | (1U << CAP_DAC_READ_SEARCH));
            if (::syscall(SYS_capset, &header, capabilities) != 0) ::_exit(2);
            try { PaperAccount account(state.string(), "USDT", 1000.0); }
            catch (const PaperAccount::PersistenceError&) { ::_exit(0); }
            catch (...) { ::_exit(3); }
            ::_exit(1);
        }
        int status = 0;
        require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "Unreadable state was not rejected by an unprivileged reader");
    } else {
        persistence_failure([&] { PaperAccount account(state.string(), "USDT", 1000.0); });
    }
    require(::chmod(state.c_str(), 0600) == 0, "Cannot restore fixture permissions");
    require(read_bytes(state) == bytes, "Unreadable state was replaced");
}

void test_non_regular_existing_state_is_rejected() {
    TemporaryDirectory directory;
    const auto state = directory.path / "account.json";
    fs::create_directory(state);
    persistence_failure([&] { PaperAccount account(state.string(), "USDT", 1000.0); });
    require(fs::is_directory(state), "Existing directory was removed");
    fs::remove(state);
    const auto target = directory.path / "target.json";
    write_bytes(target, valid_state().dump());
    fs::create_symlink(target, state);
    persistence_failure([&] { PaperAccount account(state.string(), "USDT", 1000.0); });
    require(read_bytes(target) == valid_state().dump(), "Symlink target was overwritten");
    fs::remove(state);
    fs::create_symlink(directory.path / "missing.json", state);
    persistence_failure([&] { PaperAccount account(state.string(), "USDT", 1000.0); });
    require(fs::is_symlink(state), "Dangling symlink was treated as first start");
    fs::remove(state);
    require(::mkfifo(state.c_str(), 0600) == 0, "Cannot create FIFO fixture");
    persistence_failure([&] { PaperAccount account(state.string(), "USDT", 1000.0); });
}

void test_numeric_input_failures_do_not_change_state() {
    TemporaryDirectory directory;
    const auto state = directory.path / "account.json";
    for (double balance : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        invalid_argument([&] { PaperAccount account(state.string(), "USDT", balance); });
    require(!fs::exists(state), "Invalid initial balance created state");
    PaperAccount account(state.string(), "USDT", 1000.0);
    const auto before = account.snapshot();
    const auto bytes = read_bytes(state);
    for (double delta : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        invalid_argument([&] { account.apply_realized_profit(delta); });
    require(account.snapshot() == before && read_bytes(state) == bytes, "Invalid delta changed state");
    const auto large = directory.path / "large.json";
    PaperAccount maximum(large.string(), "USDT", std::numeric_limits<double>::max());
    const auto maximum_bytes = read_bytes(large);
    invalid_argument([&] { maximum.apply_realized_profit(std::numeric_limits<double>::max()); });
    require(read_bytes(large) == maximum_bytes, "Overflow changed durable state");
    auto capped = valid_state(); capped["closed_trades"] = std::numeric_limits<int>::max();
    const auto capped_path = directory.path / "capped.json";
    write_bytes(capped_path, capped.dump());
    PaperAccount count_limit(capped_path.string(), "USDT", 1000.0);
    invalid_argument([&] { count_limit.apply_realized_profit(1.0); });
    require(read_bytes(capped_path) == capped.dump(), "Trade count overflow changed durable state");
}

void test_failed_transactions_preserve_exact_bytes_and_memory() {
    for (int failure = 0; failure < 5; ++failure) {
        TemporaryDirectory directory;
        const auto state = directory.path / "account.json";
        PaperAccount account(state.string(), "USDT", 1000.0);
        account.apply_realized_profit(50.0);
        const auto before = account.snapshot();
        const auto bytes = read_bytes(state);
        PaperAccountTestAccess::clear(account);
        if (failure == 0) PaperAccountTestAccess::fail_write(account);
        if (failure == 1) PaperAccountTestAccess::fail_sync(account, 1); // Temporary file.
        if (failure == 2) PaperAccountTestAccess::fail_sync(account, 2); // Retained old inode.
        if (failure == 3) PaperAccountTestAccess::fail_replace(account, 1);
        if (failure == 4) PaperAccountTestAccess::fail_sync(account, 3); // Replacement directory.
        persistence_failure([&] { account.apply_realized_profit(25.0); });
        require(account.snapshot() == before, "Failed transaction changed in-memory state");
        require(read_bytes(state) == bytes, "Failed transaction changed original file bytes");
        require(transaction_files(directory.path).empty(), "Recoverable failure leaked transaction files");
        PaperAccount reopened(state.string(), "USDT", 1000.0);
        require(reopened.snapshot() == before, "Failed transaction changed reopened state");
        PaperAccountTestAccess::clear(account);
        PaperAccountTestAccess::fail_write(account);
        persistence_failure([&] { account.reset(); });
        require(account.snapshot() == before && read_bytes(state) == bytes, "Failed reset changed memory or disk");
        PaperAccountTestAccess::clear(account);
        account.apply_realized_profit(25.0);
        require(account.equity() == 1075.0, "Recoverable I/O failure blocked a subsequent durable save");
    }
}

void test_legacy_temporary_directory_is_never_removed() {
    TemporaryDirectory directory;
    const auto state = directory.path / "account.json";
    const auto collision = fs::path(state.string() + ".tmp");
    fs::create_directory(collision);
    write_bytes(collision / "owner.txt", "untouched");
    PaperAccount account(state.string(), "USDT", 1000.0);
    account.apply_realized_profit(50.0);
    require(read_bytes(collision / "owner.txt") == "untouched", "Temporary-directory collision was removed or modified");
    require(account.equity() == 1050.0 && transaction_files(directory.path).empty(), "Exclusive temporary save failed");
}

void test_partial_writes_and_interruptions_complete_before_commit() {
    TemporaryDirectory directory;
    const auto state = directory.path / "account.json";
    PaperAccount account(state.string(), "USDT", 1000.0);
    PaperAccountTestAccess::clear(account);
    PaperAccountTestAccess::short_writes(account);
    account.apply_realized_profit(50.0);
    require(PaperAccountTestAccess::write_calls > 2, "Short-write fixture did not exercise retry loop");
    PaperAccount reopened(state.string(), "USDT", 1000.0);
    require(account.snapshot() == reopened.snapshot() && reopened.equity() == 1050.0, "Short writes lost durable data");
}

void test_failed_rollback_sync_latches_uncertain_state() {
    TemporaryDirectory directory;
    const auto state = directory.path / "account.json";
    PaperAccount account(state.string(), "USDT", 1000.0);
    const auto before = account.snapshot();
    const auto bytes = read_bytes(state);
    PaperAccountTestAccess::clear(account);
    PaperAccountTestAccess::fail_sync(account, 3, true);
    persistence_failure([&] { account.apply_realized_profit(50.0); }, true);
    require(PaperAccountTestAccess::raw_snapshot(account) == before, "Uncertain save committed in-memory state");
    require(read_bytes(state) == bytes, "Rollback did not restore original bytes");
    PaperAccountTestAccess::clear(account);
    persistence_failure([&] { account.equity(); }, true);
    persistence_failure([&] { account.snapshot(); }, true);
    persistence_failure([&] { account.apply_realized_profit(1.0); }, true);
    persistence_failure([&] { account.reset(); }, true);
    require(read_bytes(state) == bytes, "Latched account accepted another write");
}

void test_failed_rollback_retains_previous_file_for_reconciliation() {
    TemporaryDirectory directory;
    const auto state = directory.path / "account.json";
    PaperAccount account(state.string(), "USDT", 1000.0);
    const auto before = account.snapshot();
    const auto bytes = read_bytes(state);
    PaperAccountTestAccess::clear(account);
    PaperAccountTestAccess::fail_sync(account, 3);
    PaperAccountTestAccess::fail_replace(account, 2); // Initial replace works, restoration fails.
    persistence_failure([&] { account.apply_realized_profit(50.0); }, true);
    require(PaperAccountTestAccess::raw_snapshot(account) == before, "Failed rollback committed in-memory state");
    const auto retained = transaction_files(directory.path);
    require(retained.size() == 1 && read_bytes(retained.front()) == bytes, "Failed rollback destroyed last good file");
    require(nlohmann::json::parse(read_bytes(state))["equity"] == 1050.0, "Fixture did not reach visible uncommitted replacement");
    persistence_failure([&] { account.apply_realized_profit(1.0); }, true);
    require(read_bytes(retained.front()) == bytes, "Latched account modified retained recovery state");
}

} // namespace

int main() {
    try {
        test_first_start_reopen_and_reset();
        test_existing_schema_loads_without_version_migration();
        test_corrupt_and_invalid_existing_state_never_resets();
        test_unreadable_existing_state_is_not_first_start();
        test_non_regular_existing_state_is_rejected();
        test_numeric_input_failures_do_not_change_state();
        test_failed_transactions_preserve_exact_bytes_and_memory();
        test_legacy_temporary_directory_is_never_removed();
        test_partial_writes_and_interruptions_complete_before_commit();
        test_failed_rollback_sync_latches_uncertain_state();
        test_failed_rollback_retains_previous_file_for_reconciliation();
        std::cout << "paper_account_durability_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "paper_account_durability_tests failed: " << error.what() << '\n';
        return 1;
    }
}
