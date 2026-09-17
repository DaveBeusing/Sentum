#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sentum/operations/NotificationProviderBoundary.hpp>

namespace sentum::operations {

struct NotificationDeliveryEvidenceRecord {
	std::string dedup_key;
	std::string alert_id;
	std::size_t generation = 1;
	std::string channel;
	std::string audience;
	std::string state;
	std::size_t attempt = 0;
	bool terminal = false;
	std::string provider_reference;
	std::string failure_code;
	std::string failure_reason;
	std::string observed_at_utc;
	bool delivery_authorized = false;
	bool execution_authorized = false;
};

inline NotificationDeliveryEvidenceRecord notification_delivery_evidence_record(
	const NotificationDeliveryAttempt& attempt,
	std::string observed_at_utc = {}) {
	return {
		attempt.dedup_key,
		attempt.alert_id,
		attempt.generation,
		attempt.channel,
		attempt.audience,
		notification_delivery_state_name(attempt.state),
		attempt.attempt,
		attempt.terminal,
		attempt.provider_reference,
		attempt.failure_code,
		attempt.failure_reason,
		std::move(observed_at_utc),
		attempt.delivery_authorized,
		false
	};
}

inline nlohmann::json notification_delivery_evidence_json(
	const NotificationDeliveryEvidenceRecord& record) {
	return {
		{"dedup_key", record.dedup_key},
		{"alert_id", record.alert_id},
		{"generation", record.generation},
		{"channel", record.channel},
		{"audience", record.audience},
		{"state", record.state},
		{"attempt", record.attempt},
		{"terminal", record.terminal},
		{"provider_reference", record.provider_reference},
		{"failure_code", record.failure_code},
		{"failure_reason", record.failure_reason},
		{"observed_at_utc", record.observed_at_utc},
		{"delivery_authorized", record.delivery_authorized},
		{"execution_authorized", false}
	};
}

inline bool append_notification_delivery_evidence(
	std::vector<NotificationDeliveryEvidenceRecord>& log,
	const NotificationDeliveryEvidenceRecord& record) {
	if (record.dedup_key.empty() || record.state.empty()) return false;
	if (!log.empty()) {
		const auto& last = log.back();
		if (last.dedup_key == record.dedup_key &&
			last.state == record.state &&
			last.attempt == record.attempt &&
			last.provider_reference == record.provider_reference &&
			last.failure_code == record.failure_code) {
			return false;
		}
	}
	log.push_back(record);
	return true;
}

} // namespace sentum::operations
