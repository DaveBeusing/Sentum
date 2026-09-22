#include <sentum/operations/NotificationDispatchConfiguration.hpp>

#include <fstream>
#include <stdexcept>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace sentum::operations {
namespace {

std::size_t bounded_value(
	const nlohmann::json& json,
	const char* key,
	std::size_t fallback,
	std::size_t minimum,
	std::size_t maximum) {
	const auto value = json.value(key, fallback);
	if (value < minimum || value > maximum) {
		throw std::runtime_error(std::string("notification dispatch ") + key + " is outside the supported bound");
	}
	return value;
}

NotificationProviderType provider_type(const std::string& value) {
	if (value == "HTTP_JSON") return NotificationProviderType::HttpJson;
	throw std::runtime_error("unsupported notification provider type: " + value);
}

bool secure_endpoint(const std::string& endpoint) {
	return endpoint.rfind("https://", 0) == 0;
}

} // namespace

const char* notification_provider_type_name(NotificationProviderType type) noexcept {
	switch (type) {
		case NotificationProviderType::HttpJson: return "HTTP_JSON";
	}
	return "HTTP_JSON";
}

NotificationDispatchConfiguration load_notification_dispatch_configuration(const std::string& path) {
	std::ifstream file(path);
	if (!file) throw std::runtime_error("cannot open notification dispatch configuration: " + path);

	nlohmann::json json;
	file >> json;
	if (!json.is_object() || json.value("schema_version", 0) != 1) {
		throw std::runtime_error("notification dispatch configuration requires schema_version 1");
	}

	NotificationDispatchConfiguration config;
	config.enabled = json.value("enabled", false);
	config.routing_policy_path = json.value("routing_policy_path", config.routing_policy_path);
	if (config.routing_policy_path.empty()) throw std::runtime_error("notification routing policy path must not be empty");
	config.queue_capacity = bounded_value(json, "queue_capacity", config.queue_capacity, 1, 4096);
	config.worker_count = bounded_value(json, "worker_count", config.worker_count, 1, 8);
	config.max_attempts = bounded_value(json, "max_attempts", config.max_attempts, 1, 10);
	config.snapshot_poll_interval_ms = bounded_value(
		json, "snapshot_poll_interval_ms", config.snapshot_poll_interval_ms, 100, 60000);
	config.recovery_limit = bounded_value(
		json, "recovery_limit", config.recovery_limit, 1, 4096);

	const auto channels = json.value("enabled_channels", nlohmann::json::array());
	if (!channels.is_array()) throw std::runtime_error("notification enabled_channels must be an array");
	std::unordered_set<std::string> enabled;
	for (const auto& item : channels) {
		if (!item.is_string() || item.get<std::string>().empty()) {
			throw std::runtime_error("notification enabled channel must be a non-empty string");
		}
		const auto channel = item.get<std::string>();
		if (!enabled.insert(channel).second) throw std::runtime_error("duplicate notification enabled channel: " + channel);
		config.enabled_channels.push_back(channel);
	}

	const auto providers = json.value("providers", nlohmann::json::array());
	if (!providers.is_array()) throw std::runtime_error("notification providers must be an array");
	std::unordered_set<std::string> provider_channels;
	for (const auto& item : providers) {
		if (!item.is_object()) throw std::runtime_error("notification provider entry must be an object");
		NotificationProviderConfiguration provider;
		provider.channel = item.value("channel", std::string{});
		provider.type = provider_type(item.value("type", std::string{}));
		provider.endpoint = item.value("endpoint", std::string{});
		provider.credential_environment = item.value("credential_environment", std::string{});
		provider.connect_timeout_ms = bounded_value(item, "connect_timeout_ms", provider.connect_timeout_ms, 100, 30000);
		provider.request_timeout_ms = bounded_value(item, "request_timeout_ms", provider.request_timeout_ms, 100, 30000);
		if (provider.channel.empty()) throw std::runtime_error("notification provider channel must not be empty");
		if (!provider_channels.insert(provider.channel).second) throw std::runtime_error("duplicate notification provider: " + provider.channel);
		if (enabled.find(provider.channel) == enabled.end()) {
			throw std::runtime_error("notification provider channel is not enabled: " + provider.channel);
		}
		if (!secure_endpoint(provider.endpoint)) {
			throw std::runtime_error("notification HTTP provider endpoints must use HTTPS");
		}
		if (provider.connect_timeout_ms > provider.request_timeout_ms) {
			throw std::runtime_error("notification connect timeout must not exceed request timeout");
		}
		config.providers.push_back(std::move(provider));
	}

	if (config.enabled) {
		if (config.enabled_channels.empty()) throw std::runtime_error("enabled notification dispatch requires at least one channel");
		for (const auto& channel : config.enabled_channels) {
			if (provider_channels.find(channel) == provider_channels.end()) {
				throw std::runtime_error("enabled notification channel has no configured provider: " + channel);
			}
		}
	}

	config.execution_authorized = false;
	return config;
}

} // namespace sentum::operations
