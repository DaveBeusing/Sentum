#include <sentum/operations/NotificationDispatchRuntime.hpp>
#include <sentum/operations/NotificationOperationsObservability.hpp>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace sentum::operations;
using namespace sentum::ui;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate>
void eventually(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
        std::this_thread::sleep_for(2ms);
    }
}

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        std::string pattern = (std::filesystem::temp_directory_path() / "sentum-alert-episodes-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        require(created != nullptr, "cannot create alert fixture directory");
        directory_ = created;
        path = (directory_ / "evidence.sqlite3").string();
    }
    ~TemporaryDatabase() { std::error_code ignored; std::filesystem::remove_all(directory_, ignored); }
    std::string path;
private:
    std::filesystem::path directory_;
};

class SqliteControl {
public:
    explicit SqliteControl(const std::string& path) {
        require(sqlite3_open(path.c_str(), &db_) == SQLITE_OK, "cannot open SQLite control connection");
        sqlite3_busy_timeout(db_, 1000);
    }
    ~SqliteControl() { sqlite3_close(db_); }
    void execute(const char* sql) { require(sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "fixture SQL failed"); }
    void replace_identity(std::uint64_t generation, const nlohmann::json& item) {
        sqlite3_stmt* raw = nullptr;
        require(sqlite3_prepare_v2(db_, "UPDATE notification_alert_lifecycle SET generation=?,item_json=?;", -1,
                                  &raw, nullptr) == SQLITE_OK, "cannot prepare identity fixture");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
        const auto json = item.dump();
        sqlite3_bind_int64(raw, 1, static_cast<sqlite3_int64>(generation));
        sqlite3_bind_text(raw, 2, json.c_str(), -1, SQLITE_TRANSIENT);
        require(sqlite3_step(raw) == SQLITE_DONE, "cannot replace identity fixture");
    }
private:
    sqlite3* db_ = nullptr;
};

nlohmann::json healthy() {
    return {{"health", "healthy"}, {"market_data_connected", true}, {"market_data_fresh", true},
            {"kill_switch_active", false}, {"entries_paused", false}, {"performance", {{"queue_pressure", "normal"}}},
            {"operations_control_plane", {{"governance_state", "CONTROLLED"}, {"evidence_status", "AVAILABLE"},
                                          {"incident_state", "NONE"}, {"pending_approvals", 0}}}};
}

nlohmann::json outage(bool kill_switch = false) {
    auto state = healthy();
    if (kill_switch) state["kill_switch_active"] = true;
    else state["market_data_connected"] = false;
    return state;
}

nlohmann::json routing_policy() {
    auto routes = nlohmann::json::array();
    for (const auto* severity : {"WARNING", "CRITICAL"})
        routes.push_back({{"severity", severity}, {"minimum_escalation_level", 0},
                          {"channels", {"PAGER"}}, {"audiences", {"OPERATIONS_ON_CALL"}}});
    return {{"schema_version", 1}, {"authority", "NOTIFICATION_ROUTING_ONLY"}, {"default_action", "BLOCK"},
            {"allowed_channels", {"PAGER"}}, {"routes", std::move(routes)}};
}

class SnapshotSource {
public:
    explicit SnapshotSource(nlohmann::json state) : state_(std::move(state)) {}
    NotificationRuntimeSnapshot read() {
        std::lock_guard<std::mutex> lock(mutex_);
        return {++polls_, state_};
    }
    void set(nlohmann::json state) { std::lock_guard<std::mutex> lock(mutex_); state_ = std::move(state); }
    void wait_polls(std::uint64_t additional = 3) {
        const auto target = polls_.load() + additional;
        eventually([&] { return polls_.load() >= target; }, "scheduler did not observe repeated snapshots");
    }
private:
    std::mutex mutex_;
    std::atomic<std::uint64_t> polls_{0};
    nlohmann::json state_;
};

class RecordingProvider final : public INotificationProvider {
public:
    explicit RecordingProvider(bool terminal_failure = false) : terminal_failure_(terminal_failure) {}
    NotificationDispatchResult dispatch(const NotificationDispatchRequest& request, const std::atomic<bool>&) override {
        require(request.delivery_authorized && !request.execution_authorized, "provider request crossed authority boundary");
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
        NotificationDispatchResult result;
        result.accepted = result.delivered = !terminal_failure_;
        result.provider_reference = "fixture-" + std::to_string(requests_.size());
        if (terminal_failure_) { result.failure_code = "REJECTED"; result.failure_reason = "fixture terminal rejection"; }
        return result;
    }
    std::vector<NotificationDispatchRequest> requests() const { std::lock_guard<std::mutex> lock(mutex_); return requests_; }
private:
    const bool terminal_failure_;
    mutable std::mutex mutex_;
    std::vector<NotificationDispatchRequest> requests_;
};

std::unique_ptr<NotificationDispatchRuntime> runtime_for(const std::string& path, SnapshotSource& source,
                                                       const std::shared_ptr<RecordingProvider>& provider) {
    NotificationDispatchConfiguration configuration;
    configuration.enabled = true;
    configuration.worker_count = 1;
    configuration.queue_capacity = 8;
    configuration.recovery_limit = 64;
    configuration.snapshot_poll_interval_ms = 100;
    configuration.max_attempts = 1;
    configuration.enabled_channels = {"PAGER"};
    NotificationProviderRegistry registry;
    registry.add("PAGER", provider);
    return std::make_unique<NotificationDispatchRuntime>(configuration, routing_policy(), std::move(registry), path,
                                                        [&] { return source.read(); });
}

OperatorAlertLifecycleItem episode(const std::string& path, const std::string& id) {
    NotificationDeliveryEvidenceRepository reader(path, NotificationDeliveryEvidenceOpenMode::ReadOnly);
    const auto lifecycle = reader.load_alert_lifecycle();
    for (const auto& item : lifecycle.items) if (item.alert.id == id) return item;
    throw std::runtime_error("missing durable alert episode: " + id);
}

void wait_terminal(const std::string& path, const std::string& id, std::size_t generation) {
    NotificationDeliveryEvidenceRepository reader(path, NotificationDeliveryEvidenceOpenMode::ReadOnly);
    eventually([&] {
        for (const auto& row : reader.load_latest_per_dedup_key().records)
            if (row.record.alert_id == id && row.record.generation == generation && row.record.terminal) return true;
        return false;
    }, "scheduler did not persist terminal delivery for this episode");
}

void repeated_clears_keep_generation_and_ack_scope() {
    auto state = derive_operator_alert_lifecycle(outage());
    require(state.size() == 1 && state.front().generation == 1, "initial alert generation missing");
    for (int poll = 0; poll < 6; ++poll) state = reconcile_operator_alert_lifecycle(state, {});
    require(state.size() == 1 && state.front().state == OperatorAlertLifecycleState::Cleared,
            "healthy polls discarded cleared episode identity");
    state = reconcile_operator_alert_lifecycle(state, derive_operator_alerts(outage()),
                                               {{"market.disconnected", 1, "operator", "old incident"}});
    require(state.front().generation == 2 && state.front().state == OperatorAlertLifecycleState::New &&
            state.front().acknowledged_by.empty(), "old acknowledgement crossed recurrence boundary");
}

void actual_scheduler_recurs(bool kill_switch, bool terminal_failure) {
    TemporaryDatabase temporary;
    const std::string id = kill_switch ? "runtime.kill_switch" : "market.disconnected";
    SnapshotSource source(outage(kill_switch));
    auto provider = std::make_shared<RecordingProvider>(terminal_failure);
    auto runtime = runtime_for(temporary.path, source, provider);
    runtime->start();
    wait_terminal(temporary.path, id, 1);
    source.wait_polls();
    require(provider->requests().size() == 1, "same episode was dispatched repeatedly");
    source.set(healthy());
    source.wait_polls(5);
    require(episode(temporary.path, id).state == OperatorAlertLifecycleState::Cleared,
            "scheduler did not durably clear alert");
    source.set(outage(kill_switch));
    wait_terminal(temporary.path, id, 2);
    runtime->stop();
    const auto requests = provider->requests();
    require(requests.size() == 2 && requests[0].generation == 1 && requests[1].generation == 2 &&
            requests[0].dedup_key != requests[1].dedup_key, "recurring outage reused the first delivery key");
}

void restart_keeps_active_episode_and_cleared_tombstone() {
    TemporaryDatabase temporary;
    SnapshotSource source(outage());
    auto provider = std::make_shared<RecordingProvider>();
    {
        auto runtime = runtime_for(temporary.path, source, provider);
        runtime->start();
        wait_terminal(temporary.path, "market.disconnected", 1);
    }
    {
        auto runtime = runtime_for(temporary.path, source, provider);
        runtime->start();
        source.wait_polls();
        require(provider->requests().size() == 1, "restart redelivered an unchanged active episode");
        source.set(healthy());
        source.wait_polls();
    }
    {
        auto runtime = runtime_for(temporary.path, source, provider);
        runtime->start();
        source.wait_polls();
        require(episode(temporary.path, "market.disconnected").state == OperatorAlertLifecycleState::Cleared,
                "restart forgot the cleared tombstone");
        source.set(outage());
        wait_terminal(temporary.path, "market.disconnected", 2);
    }
    // A crash can occur after a recurrence is committed but before a delivery
    // intent is appended. The next process must deliver that same generation.
    {
        NotificationDeliveryEvidenceRepository owner(temporary.path);
        owner.observe_alert_lifecycle(healthy());
        owner.observe_alert_lifecycle(outage());
    }
    {
        auto runtime = runtime_for(temporary.path, source, provider);
        runtime->start();
        wait_terminal(temporary.path, "market.disconnected", 3);
        source.wait_polls();
    }
    require(provider->requests().size() == 3, "restart lost or duplicated a committed recurrence");
}

NotificationDeliveryEvidenceRecord legacy_record(std::size_t generation, bool failed = false) {
    NotificationDeliveryEvidenceRecord record;
    record.alert_id = "market.disconnected";
    record.generation = generation;
    record.channel = "PAGER";
    record.audience = "OPERATIONS_ON_CALL";
    record.dedup_key = notification_dedup_key(record.alert_id, generation, 2, record.channel, record.audience);
    record.state = failed ? "FAILED" : "DELIVERED";
    record.attempt = record.max_attempts = 1;
    record.terminal = true;
    record.delivery_authorized = true;
    return record;
}

void legacy_delivery_history_sets_generation_floor() {
    TemporaryDatabase temporary;
    {
        NotificationDeliveryEvidenceRepository owner(temporary.path);
        owner.append(legacy_record(2));
        owner.append(legacy_record(7, true));
    }
    SqliteControl control(temporary.path);
    control.execute("DROP TABLE notification_alert_lifecycle; DROP TABLE notification_alert_lifecycle_checkpoint;");
    SnapshotSource source(outage());
    auto provider = std::make_shared<RecordingProvider>();
    auto runtime = runtime_for(temporary.path, source, provider);
    runtime->start();
    source.wait_polls();
    require(provider->requests().empty() && episode(temporary.path, "market.disconnected").generation == 7,
            "migration reset or redispatched the legacy active generation");
    source.set(healthy());
    source.wait_polls();
    source.set(outage());
    wait_terminal(temporary.path, "market.disconnected", 8);
    runtime->stop();
    require(provider->requests().size() == 1 && provider->requests().front().generation == 8,
            "legacy terminal failure suppressed a distinct later episode");
}

void acknowledgements_survive_restart_without_covering_recurrence() {
    TemporaryDatabase temporary;
    auto acknowledged = outage(true);
    acknowledged["operations_control_plane"]["alert_acknowledgements"] = {{{"alert_id", "runtime.kill_switch"},
        {"generation", 1}, {"actor", "operator"}, {"reason", "reviewed first episode"}}};
    SnapshotSource source(acknowledged);
    auto provider = std::make_shared<RecordingProvider>();
    {
        auto runtime = runtime_for(temporary.path, source, provider);
        runtime->start();
        source.wait_polls();
        require(provider->requests().empty(), "matching acknowledgement did not suppress initial routing");
    }
    {
        source.set(outage(true));
        auto runtime = runtime_for(temporary.path, source, provider);
        runtime->start();
        source.wait_polls();
        require(episode(temporary.path, "runtime.kill_switch").state == OperatorAlertLifecycleState::Acknowledged &&
                provider->requests().empty(), "restart lost the acknowledgement of the active episode");
        source.set(healthy());
        source.wait_polls();
        source.set(acknowledged);
        wait_terminal(temporary.path, "runtime.kill_switch", 2);
    }
    require(provider->requests().size() == 1 && provider->requests().front().generation == 2,
            "acknowledgement suppressed a different episode generation");
}

void failed_lifecycle_write_never_authorizes_an_episode() {
    TemporaryDatabase temporary;
    SnapshotSource source(outage());
    auto provider = std::make_shared<RecordingProvider>();
    auto runtime = runtime_for(temporary.path, source, provider);
    SqliteControl control(temporary.path);
    control.execute("CREATE TRIGGER reject_episode BEFORE INSERT ON notification_alert_lifecycle "
                    "BEGIN SELECT RAISE(ABORT,'fixture lifecycle write failure'); END;");
    runtime->start();
    eventually([&] { return runtime->metrics_json().at("snapshot_errors").get<std::uint64_t>() > 0; },
               "lifecycle write failure was not observable");
    require(provider->requests().empty() && runtime->metrics_json().at("alert_lifecycle_status") == "UNAVAILABLE",
            "uncommitted lifecycle authorized delivery");
    {
        NotificationDeliveryEvidenceRepository reader(temporary.path, NotificationDeliveryEvidenceOpenMode::ReadOnly);
        require(!reader.load_alert_lifecycle().initialized && reader.load_latest_per_dedup_key().records.empty(),
                "failed transaction left partial episode or delivery evidence");
    }
    control.execute("DROP TRIGGER reject_episode;");
    wait_terminal(temporary.path, "market.disconnected", 1);
    control.execute("CREATE TRIGGER reject_clear BEFORE UPDATE ON notification_alert_lifecycle WHEN NEW.state='CLEARED' "
                    "BEGIN SELECT RAISE(ABORT,'fixture clear write failure'); END;");
    const auto failures = runtime->metrics_json().at("snapshot_errors").get<std::uint64_t>();
    source.set(healthy());
    eventually([&] { return runtime->metrics_json().at("snapshot_errors").get<std::uint64_t>() > failures; },
               "failed clear was not observed");
    require(episode(temporary.path, "market.disconnected").active, "failed clear changed durable episode identity");
    runtime->stop();
    source.set(outage());
    control.execute("DROP TRIGGER reject_clear;");
    runtime = runtime_for(temporary.path, source, provider);
    runtime->start();
    source.wait_polls();
    require(provider->requests().size() == 1 && episode(temporary.path, "market.disconnected").generation == 1,
            "failed clear was treated as a committed recurrence boundary");
    source.set(healthy());
    source.wait_polls();
    source.set(outage());
    wait_terminal(temporary.path, "market.disconnected", 2);
    runtime->stop();
}

void unavailable_or_stale_episode_storage_keeps_conditions_visible() {
    TemporaryDatabase temporary;
    NotificationDeliveryEvidenceRepository owner(temporary.path);
    auto state = outage(true);
    state["operations_control_plane"]["alert_lifecycle_status"] = "AVAILABLE";
    state["operations_control_plane"]["alert_lifecycle"] = nlohmann::json::array();
    const auto uninitialized = notification_delivery_snapshot_from_repository(state, owner);
    auto visible = derive_operator_alert_center_view(uninitialized);
    require(visible.active == 1 && visible.items.front().generation == 0 && visible.items.front().state == "UNTRACKED" &&
            !visible.items.front().notification_candidate, "uninitialized storage hid a kill switch or invented its episode");
    require(derive_notification_routing_plan(uninitialized, routing_policy()).eligible == 0,
            "untracked condition produced an eligible route");
    owner.observe_alert_lifecycle(healthy());
    const auto stale = notification_delivery_snapshot_from_repository(state, owner);
    visible = derive_operator_alert_center_view(stale);
    require(visible.active == 1 && visible.items.front().generation == 0, "stale empty lifecycle hid a new condition");
    owner.observe_alert_lifecycle(outage(true));
    const auto coherent = notification_delivery_snapshot_from_repository(outage(true), owner);
    visible = derive_operator_alert_center_view(coherent);
    require(visible.active == 1 && visible.items.front().generation == 1 && !visible.execution_authorized,
            "read-only projection did not use committed generation");
    const auto cleared = notification_delivery_snapshot_from_repository(healthy(), owner);
    require(derive_operator_alert_center_view(cleared).active == 0, "stale stored active row overrode a cleared condition");
    SqliteControl control(temporary.path);
    control.execute("UPDATE notification_alert_lifecycle SET item_json='{}';");
    const auto corrupt = notification_delivery_snapshot_from_repository(outage(true), owner);
    require(derive_operator_alert_center_view(corrupt).active == 1 &&
            derive_notification_routing_plan(corrupt, routing_policy()).eligible == 0,
            "unreadable episode state hid current condition or authorized dispatch");
    bool rejected = false;
    try { owner.observe_alert_lifecycle(outage(true)); } catch (const std::exception&) { rejected = true; }
    require(rejected, "corrupt lifecycle identity was silently reset");
}

void generation_exhaustion_is_fail_closed() {
    TemporaryDatabase temporary;
    NotificationDeliveryEvidenceRepository owner(temporary.path);
    owner.append(legacy_record(kMaximumOperatorAlertGeneration));
    owner.observe_alert_lifecycle(outage());
    owner.observe_alert_lifecycle(healthy());
    bool rejected = false;
    try { owner.observe_alert_lifecycle(outage()); } catch (const std::overflow_error&) { rejected = true; }
    require(rejected, "exhausted generation wrapped into a reused delivery identity");
    const auto item = episode(temporary.path, "market.disconnected");
    require(item.generation == kMaximumOperatorAlertGeneration && !item.active,
            "failed generation advance changed durable episode state");
}

void rounded_metadata_cannot_reduce_durable_generation() {
    TemporaryDatabase temporary;
    NotificationDeliveryEvidenceRepository owner(temporary.path);
    const auto initial = owner.observe_alert_lifecycle(outage());
    auto metadata = operator_alert_lifecycle_json(initial.front());
    metadata["generation"] = 9007199254740992.0;
    SqliteControl control(temporary.path);
    control.replace_identity(9007199254740993ULL, metadata);
    bool rejected = false;
    try { owner.observe_alert_lifecycle(outage()); } catch (const std::exception&) { rejected = true; }
    require(rejected, "rounded JSON metadata reduced the durable generation identity");
}
} // namespace

int main() {
    try {
        repeated_clears_keep_generation_and_ack_scope();
        actual_scheduler_recurs(false, false);
        actual_scheduler_recurs(true, false);
        actual_scheduler_recurs(false, true);
        restart_keeps_active_episode_and_cleared_tombstone();
        legacy_delivery_history_sets_generation_floor();
        acknowledgements_survive_restart_without_covering_recurrence();
        failed_lifecycle_write_never_authorizes_an_episode();
        unavailable_or_stale_episode_storage_keeps_conditions_visible();
        generation_exhaustion_is_fail_closed();
        rounded_metadata_cannot_reduce_durable_generation();
        std::cout << "Notification alert generation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Notification alert generation failure: " << error.what() << '\n';
        return 1;
    }
}
