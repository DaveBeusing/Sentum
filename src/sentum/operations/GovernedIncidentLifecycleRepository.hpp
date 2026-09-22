#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace sentum::operations {

enum class GovernedIncidentLifecycleOpenMode {
	ReadWriteCreate,
	ReadOnly
};

struct IncidentRequestSubmission {
	std::string request_id;
	bool created = false;
};

struct IncidentApprovalResult {
	std::string incident_id;
	bool opened = false;
};

class GovernedIncidentLifecycleRepository {
public:
	static constexpr std::size_t kMaximumAuditLimit = 256;

	explicit GovernedIncidentLifecycleRepository(
		std::string path,
		GovernedIncidentLifecycleOpenMode mode = GovernedIncidentLifecycleOpenMode::ReadWriteCreate);
	~GovernedIncidentLifecycleRepository();

	GovernedIncidentLifecycleRepository(const GovernedIncidentLifecycleRepository&) = delete;
	GovernedIncidentLifecycleRepository& operator=(const GovernedIncidentLifecycleRepository&) = delete;

	IncidentRequestSubmission submit_open_incident_request(
		const std::string& source_correlation_id,
		const std::string& actor,
		const std::string& reason,
		const std::string& git_sha = {},
		const std::string& runtime_evidence_id = {});

	IncidentApprovalResult approve_open_incident_request(
		const std::string& request_id,
		const std::string& approver,
		const std::string& reason,
		const std::string& expected_source_correlation_id = {});

	bool deny_open_incident_request(
		const std::string& request_id,
		const std::string& approver,
		const std::string& reason,
		const std::string& expected_source_correlation_id = {});

	bool acknowledge_incident(
		const std::string& incident_id,
		const std::string& actor,
		const std::string& reason);

	bool begin_recovery(
		const std::string& incident_id,
		const std::string& actor,
		const std::string& reason,
		const std::string& reconciliation_evidence_id);

	bool resolve_incident(
		const std::string& incident_id,
		const std::string& actor,
		const std::string& reason);

	bool close_incident(
		const std::string& incident_id,
		const std::string& actor,
		const std::string& reason);

	std::optional<std::string> request_id_for_correlation(const std::string& source_correlation_id) const;
	std::optional<std::string> incident_id_for_request(const std::string& request_id) const;

	nlohmann::json control_plane_snapshot(
		std::size_t approval_limit = 8,
		std::size_t audit_limit = 64) const;

	const std::string& path() const noexcept { return path_; }
	bool read_only() const noexcept { return mode_ == GovernedIncidentLifecycleOpenMode::ReadOnly; }

private:
	void exec_or_throw(const char* sql);
	void require_writable() const;
	void begin_transaction();
	void commit_transaction();
	void rollback_transaction() noexcept;

	std::string path_;
	GovernedIncidentLifecycleOpenMode mode_ = GovernedIncidentLifecycleOpenMode::ReadWriteCreate;
	sqlite3* db_ = nullptr;
	mutable std::mutex mutex_;
};

nlohmann::json merge_governed_incident_lifecycle_snapshot(
	const nlohmann::json& snapshot,
	const GovernedIncidentLifecycleRepository& repository,
	std::size_t approval_limit = 8,
	std::size_t audit_limit = 64);

} // namespace sentum::operations
