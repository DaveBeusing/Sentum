#pragma once

#include <sentum/operations/NotificationDispatchConfiguration.hpp>
#include <sentum/operations/NotificationProvider.hpp>

namespace sentum::operations {

class HttpNotificationProvider final : public INotificationProvider {
public:
	explicit HttpNotificationProvider(NotificationProviderConfiguration configuration);

	NotificationDispatchResult dispatch(
		const NotificationDispatchRequest& request,
		const std::atomic<bool>& cancellation_requested) override;

private:
	NotificationProviderConfiguration configuration_;
};

} // namespace sentum::operations
