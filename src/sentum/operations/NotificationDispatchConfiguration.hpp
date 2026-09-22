#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sentum::operations {

enum class NotificationProviderType {
	HttpJson
};

struct NotificationProviderConfiguration {
	std::string channel;
	NotificationProviderType type = NotificationProviderType::HttpJson;
	std::string endpoint;
	std::string credential_environment;
	std::size_t connect_timeout_ms = 1000;
	std::size_t request_timeout_ms = 5000;
};

struct NotificationDispatchConfiguration {
	bool enabled = false;
	std::string routing_policy_path = "config/notification_routing_policy.json";
	std::size_t queue_capacity = 128;
	std::size_t worker_count = 2;
	std::size_t max_attempts = 3;
	std::size_t snapshot_poll_interval_ms = 500;
	std::size_t recovery_limit = 1024;
	std::vector<std::string> enabled_channels;
	std::vector<NotificationProviderConfiguration> providers;
	bool execution_authorized = false;
};

const char* notification_provider_type_name(NotificationProviderType type) noexcept;
NotificationDispatchConfiguration load_notification_dispatch_configuration(const std::string& path);

} // namespace sentum::operations
