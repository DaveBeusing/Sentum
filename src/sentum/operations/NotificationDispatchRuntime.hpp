#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/operations/NotificationDeliveryEvidenceRepository.hpp>
#include <sentum/operations/NotificationDispatchConfiguration.hpp>
#include <sentum/operations/NotificationProvider.hpp>
#include <sentum/operations/NotificationRoutingPolicy.hpp>

namespace sentum::operations {

struct NotificationRuntimeSnapshot {
	std::uint64_t generation = 0;
	nlohmann::json state = nlohmann::json::object();
};

class NotificationDispatchRuntime {
public:
	using SnapshotSource = std::function<NotificationRuntimeSnapshot()>;
	using MetricsSink = std::function<void(const nlohmann::json&)>;

	NotificationDispatchRuntime(
		NotificationDispatchConfiguration configuration,
		nlohmann::json routing_policy,
		NotificationProviderRegistry providers,
		std::string evidence_database_path,
		SnapshotSource snapshot_source = {},
		MetricsSink metrics_sink = {});
	~NotificationDispatchRuntime();

	NotificationDispatchRuntime(const NotificationDispatchRuntime&) = delete;
	NotificationDispatchRuntime& operator=(const NotificationDispatchRuntime&) = delete;

	void start();
	void stop() noexcept;
	bool running() const noexcept { return started_.load(std::memory_order_acquire); }

	std::size_t submit(const NotificationRoutingPlan& plan);
	nlohmann::json metrics_json() const;

private:
	struct WorkItem {
		std::chrono::steady_clock::time_point due;
		std::uint64_t sequence = 0;
		NotificationDeliveryAttempt attempt;
	};

	NotificationDispatchConfiguration configuration_;
	nlohmann::json routing_policy_;
	NotificationProviderRegistry providers_;
	NotificationDeliveryEvidenceRepository evidence_;
	SnapshotSource snapshot_source_;
	MetricsSink metrics_sink_;

	std::atomic<bool> started_{false};
	std::atomic<bool> accepting_{false};
	std::atomic<bool> stop_requested_{false};
	std::atomic<bool> provider_cancellation_{false};

	mutable std::mutex queue_mutex_;
	std::condition_variable queue_cv_;
	std::vector<WorkItem> queue_;
	std::uint64_t queue_sequence_ = 0;

	mutable std::mutex state_mutex_;
	std::unordered_set<std::string> known_keys_;
	std::string status_ = "STOPPED";
	std::string last_error_;

	std::mutex control_mutex_;
	std::condition_variable control_cv_;
	std::thread scheduler_thread_;
	std::vector<std::thread> workers_;

	std::atomic<std::size_t> queued_{0};
	std::atomic<std::size_t> dispatching_{0};
	std::atomic<std::uint64_t> delivered_total_{0};
	std::atomic<std::uint64_t> retriable_failed_total_{0};
	std::atomic<std::uint64_t> terminal_failed_total_{0};
	std::atomic<std::uint64_t> provider_calls_{0};
	std::atomic<std::uint64_t> provider_latency_us_total_{0};
	std::atomic<std::uint64_t> provider_latency_us_last_{0};
	std::atomic<std::uint64_t> timeout_count_{0};
	std::atomic<std::uint64_t> queue_rejections_{0};
	std::atomic<std::uint64_t> snapshot_errors_{0};

	mutable std::mutex publish_mutex_;
	std::string last_published_metrics_;

	void recover();
	void scheduler_loop();
	void worker_loop();
	bool take_work(WorkItem& item);
	bool enqueue(NotificationDeliveryAttempt attempt, std::chrono::steady_clock::time_point due, bool require_accepting);
	void persist(const NotificationDeliveryAttempt& attempt);
	void record_terminal_queue_rejection(NotificationDeliveryAttempt attempt, const char* code, const char* reason);
	void set_status(std::string status, std::string error = {});
	void publish_metrics();
};

} // namespace sentum::operations
