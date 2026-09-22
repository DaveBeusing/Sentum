#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <sentum/operations/NotificationDeliveryEvidence.hpp>

struct sqlite3;

namespace sentum::operations {

struct PersistedNotificationDeliveryEvidence {
	std::int64_t sequence = 0;
	NotificationDeliveryEvidenceRecord record;
};

struct NotificationDeliveryEvidenceBatch {
	std::vector<PersistedNotificationDeliveryEvidence> records;
	std::size_t total = 0;
	bool truncated = false;
};

enum class NotificationDeliveryEvidenceOpenMode {
	ReadWriteCreate,
	ReadOnly
};

class NotificationDeliveryEvidenceRepository {
public:
	static constexpr std::size_t kMaximumQueryLimit = 4096;

	explicit NotificationDeliveryEvidenceRepository(
		std::string path,
		NotificationDeliveryEvidenceOpenMode mode = NotificationDeliveryEvidenceOpenMode::ReadWriteCreate);
	~NotificationDeliveryEvidenceRepository();

	NotificationDeliveryEvidenceRepository(const NotificationDeliveryEvidenceRepository&) = delete;
	NotificationDeliveryEvidenceRepository& operator=(const NotificationDeliveryEvidenceRepository&) = delete;

	bool append(const NotificationDeliveryEvidenceRecord& record);
	NotificationDeliveryEvidenceBatch load_recent(std::size_t limit = 128) const;
	NotificationDeliveryEvidenceBatch load_latest_per_dedup_key(std::size_t limit = 1024) const;
	std::vector<NotificationDeliveryAttempt> restore_active_delivery_state(std::size_t limit = 1024) const;

	const std::string& path() const noexcept { return path_; }
	bool read_only() const noexcept { return mode_ == NotificationDeliveryEvidenceOpenMode::ReadOnly; }

private:
	static std::size_t bounded_limit(std::size_t requested) noexcept;
	void exec_or_throw(const char* sql);
	std::size_t scalar_count(const char* sql) const;
	NotificationDeliveryEvidenceBatch load_batch(const char* sql, std::size_t limit, std::size_t total) const;

	std::string path_;
	NotificationDeliveryEvidenceOpenMode mode_ = NotificationDeliveryEvidenceOpenMode::ReadWriteCreate;
	sqlite3* db_ = nullptr;
	mutable std::mutex mutex_;
};

} // namespace sentum::operations
