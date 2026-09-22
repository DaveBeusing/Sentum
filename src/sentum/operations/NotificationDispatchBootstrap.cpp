#include <sentum/operations/NotificationDispatchBootstrap.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <sentum/dashboard/DashboardState.hpp>
#include <sentum/operations/HttpNotificationProvider.hpp>

namespace sentum::operations {
namespace {

nlohmann::json load_json_file(const std::string& path) {
	std::ifstream file(path);
	if (!file) throw std::runtime_error("cannot open notification routing policy: " + path);
	nlohmann::json value;
	file >> value;
	return value;
}

std::string runtime_database_path() {
	std::ifstream file("config/config.json");
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

nlohmann::json current_operational_snapshot() {
	nlohmann::json state = nlohmann::json::object();
	std::ifstream file("log/status.json");
	if (file) {
		try {
			file >> state;
			if (!state.is_object()) state = nlohmann::json::object();
		} catch (...) {
			state = nlohmann::json::object();
		}
	}

	const auto live = sentum::dashboard::DashboardState::global().snapshot();
	for (auto it = live.begin(); it != live.end(); ++it) {
		if (!it.value().is_null() && !(it.key() == "mode" && it.value() == "idle")) state[it.key()] = it.value();
	}
	return state;
}

nlohmann::json disabled_metrics(const char* reason) {
	return {
		{"status", "DISABLED"},
		{"enabled", false},
		{"queued", 0},
		{"dispatching", 0},
		{"delivered", 0},
		{"retriable_failed", 0},
		{"terminal_failed", 0},
		{"provider_calls", 0},
		{"provider_latency_ms_last", 0.0},
		{"provider_latency_ms_average", 0.0},
		{"timeout_count", 0},
		{"queue_rejections", 0},
		{"snapshot_errors", 0},
		{"last_error", reason},
		{"execution_authorized", false}
	};
}

} // namespace

std::unique_ptr<NotificationDispatchRuntime> start_notification_dispatch_runtime(
	const std::string& dispatch_configuration_path) {
	auto& dashboard = sentum::dashboard::DashboardState::global();
	const auto database_path = runtime_database_path();
	dashboard.set("db_path", database_path);

	if (!std::filesystem::exists(dispatch_configuration_path)) {
		dashboard.set("notification_dispatch_runtime", disabled_metrics("notification dispatch configuration is absent"));
		return {};
	}

	const auto configuration = load_notification_dispatch_configuration(dispatch_configuration_path);
	if (!configuration.enabled) {
		dashboard.set("notification_dispatch_runtime", disabled_metrics("notification dispatch is disabled by configuration"));
		return {};
	}

	NotificationProviderRegistry providers;
	for (const auto& provider : configuration.providers) {
		switch (provider.type) {
			case NotificationProviderType::HttpJson:
				providers.add(provider.channel, std::make_shared<HttpNotificationProvider>(provider));
				break;
		}
	}

	const auto routing_policy = load_json_file(configuration.routing_policy_path);
	auto sequence = std::make_shared<std::atomic<std::uint64_t>>(0);
	auto runtime = std::make_unique<NotificationDispatchRuntime>(
		configuration,
		routing_policy,
		std::move(providers),
		database_path,
		[sequence] {
			NotificationRuntimeSnapshot snapshot;
			snapshot.generation = sequence->fetch_add(1, std::memory_order_relaxed) + 1;
			snapshot.state = current_operational_snapshot();
			return snapshot;
		},
		[](const nlohmann::json& metrics) {
			sentum::dashboard::DashboardState::global().set("notification_dispatch_runtime", metrics);
		});
	runtime->start();
	return runtime;
}

} // namespace sentum::operations
