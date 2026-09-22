#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <sentum/operations/NotificationProviderBoundary.hpp>

namespace sentum::operations {

class INotificationProvider {
public:
	virtual ~INotificationProvider() = default;
	virtual NotificationDispatchResult dispatch(
		const NotificationDispatchRequest& request,
		const std::atomic<bool>& cancellation_requested) = 0;
};

class NotificationProviderRegistry {
public:
	void add(std::string channel, std::shared_ptr<INotificationProvider> provider) {
		if (channel.empty() || !provider) throw std::invalid_argument("notification provider registration is incomplete");
		if (!providers_.emplace(std::move(channel), std::move(provider)).second) {
			throw std::invalid_argument("duplicate notification provider channel");
		}
	}

	std::shared_ptr<INotificationProvider> find(const std::string& channel) const {
		const auto found = providers_.find(channel);
		return found == providers_.end() ? std::shared_ptr<INotificationProvider>{} : found->second;
	}

	std::size_t size() const noexcept { return providers_.size(); }

private:
	std::unordered_map<std::string, std::shared_ptr<INotificationProvider>> providers_;
};

} // namespace sentum::operations
