#include <sentum/operations/GovernedIncidentLifecycleRuntime.hpp>

#include <fstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>
#include <sentum/dashboard/DashboardState.hpp>
#include <sentum/operations/NotificationIncidentWorkflowBridge.hpp>

namespace sentum::operations {

GovernedIncidentLifecycleRuntime::GovernedIncidentLifecycleRuntime(
	std::string database_path,
	std::string git_sha,
	std::chrono::milliseconds poll_interval)
	: database_path_(std::move(database_path)),
	  git_sha_(std::move(git_sha)),
	  poll_interval_(poll_interval),
	  lifecycle_(database_path_),
	  notification_evidence_(database_path_, NotificationDeliveryEvidenceOpenMode::ReadOnly) {
	if (poll_interval_.count() <= 0) {
		throw std::invalid_argument("governed incident lifecycle poll interval must be positive");
	}
}

GovernedIncidentLifecycleRuntime::~GovernedIncidentLifecycleRuntime() {
	stop();
}

void GovernedIncidentLifecycleRuntime::start() {
	sentum::dashboard::DashboardState::global().set("db_path", database_path_);
	bool expected = false;
	if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
	try {
		tick_once();
		worker_ = std::thread(&GovernedIncidentLifecycleRuntime::run, this);
	} catch (...) {
		running_.store(false, std::memory_order_release);
		throw;
	}
}

void GovernedIncidentLifecycleRuntime::stop() noexcept {
	running_.store(false, std::memory_order_release);
	if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
	try {
		publish_state();
	} catch (...) {
	}
}

void GovernedIncidentLifecycleRuntime::tick_once() {
	try {
		const auto candidate = derive_notification_incident_candidate(notification_evidence_);
		if (candidate.state == NotificationIncidentCandidateState::ProposalReady &&
			!candidate.source_correlation_id.empty() &&
			!lifecycle_.request_id_for_correlation(candidate.source_correlation_id).has_value()) {
			lifecycle_.submit_open_incident_request(
				candidate.source_correlation_id,
				"notification-runtime",
				candidate.reason,
				git_sha_,
				candidate.source_evidence_id);
		}
	} catch (const std::exception&) {
		// Missing, incomplete or unavailable notification evidence must not create
		// incident state. Existing durable lifecycle state is still projected.
	}
	publish_state();
}

void GovernedIncidentLifecycleRuntime::publish_state() {
	const auto snapshot = sentum::dashboard::DashboardState::global().snapshot();
	const auto merged = merge_governed_incident_lifecycle_snapshot(snapshot, lifecycle_);
	const auto control_plane = merged.value("operations_control_plane", nlohmann::json::object());
	sentum::dashboard::DashboardState::global().set("operations_control_plane", control_plane);
}

void GovernedIncidentLifecycleRuntime::run() noexcept {
	while (running_.load(std::memory_order_acquire)) {
		try {
			tick_once();
		} catch (...) {
			try {
				publish_state();
			} catch (...) {
			}
		}
		std::this_thread::sleep_for(poll_interval_);
	}
}

std::string operations_runtime_database_path(const std::string& configuration_path) {
	std::ifstream file(configuration_path);
	if (!file) return "log/sentum.sqlite3";
	try {
		nlohmann::json value;
		file >> value;
		const auto path = value.value("databasePath", std::string("log/sentum.sqlite3"));
		return path.empty() ? std::string("log/sentum.sqlite3") : path;
	} catch (...) {
		return "log/sentum.sqlite3";
	}
}

std::unique_ptr<GovernedIncidentLifecycleRuntime> start_governed_incident_lifecycle_runtime(
	const std::string& git_sha,
	const std::string& configuration_path) {
	auto runtime = std::make_unique<GovernedIncidentLifecycleRuntime>(
		operations_runtime_database_path(configuration_path),
		git_sha);
	runtime->start();
	return runtime;
}

} // namespace sentum::operations
