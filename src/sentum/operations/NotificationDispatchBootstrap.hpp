#pragma once

#include <memory>
#include <string>

#include <sentum/operations/NotificationDispatchRuntime.hpp>

namespace sentum::operations {

std::unique_ptr<NotificationDispatchRuntime> start_notification_dispatch_runtime(
	const std::string& dispatch_configuration_path = "config/notification_dispatch.json");

} // namespace sentum::operations
