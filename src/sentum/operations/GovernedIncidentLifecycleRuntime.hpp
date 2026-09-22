#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <sentum/operations/GovernedIncidentLifecycleRepository.hpp>
#include <sentum/operations/NotificationDeliveryEvidenceRepository.hpp>

namespace sentum::operations {

class GovernedIncidentLifecycleRuntime {
public:
	GovernedIncidentLifecycleRuntime(
		std::string database_path,
		std::string git_sha,
		std::chrono::milliseconds poll_interval = std::chrono::milliseconds(500));
	~GovernedIncidentLifecycleRuntime();

	GovernedIncidentLifecycleRuntime(const GovernedIncidentLifecycleRuntime&) = delete;
	GovernedIncidentLifecycleRuntime& operator=(const GovernedIncidentLifecycleRuntime&) = delete;

	void start();
	void stop() noexcept;
	void tick_once();

	bool running() const noexcept { return running_.load(std::memory_order_acquire); }
	const std::string& database_path() const noexcept { return database_path_; }

private:
	void run() noexcept;
	void publish_state();

	std::string database_path_;
	std::string git_sha_;
	std::chrono::milliseconds poll_interval_;
	GovernedIncidentLifecycleRepository lifecycle_;
	NotificationDeliveryEvidenceRepository notification_evidence_;
	std::atomic<bool> running_{false};
	std::thread worker_;
};

std::string operations_runtime_database_path(
	const std::string& configuration_path = "config/config.json");

std::unique_ptr<GovernedIncidentLifecycleRuntime> start_governed_incident_lifecycle_runtime(
	const std::string& git_sha,
	const std::string& configuration_path = "config/config.json");

} // namespace sentum::operations
