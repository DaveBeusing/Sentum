#include <sentum/operations/GovernedIncidentLifecycleRepository.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sqlite3.h>

namespace sentum::operations {
namespace {

class Statement {
public:
	Statement(sqlite3* db, const char* sql, const char* context) : db_(db) {
		if (sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr) != SQLITE_OK) {
			throw std::runtime_error(std::string(context) + ": " + sqlite3_errmsg(db_));
		}
	}

	~Statement() {
		if (statement_) sqlite3_finalize(statement_);
	}

	sqlite3_stmt* get() const noexcept { return statement_; }

private:
	sqlite3* db_ = nullptr;
	sqlite3_stmt* statement_ = nullptr;
};

std::string sqlite_message(sqlite3* db, const char* context) {
	return std::string(context) + ": " + (db ? sqlite3_errmsg(db) : "unknown SQLite error");
}

std::string column_text(sqlite3_stmt* statement, int column) {
	const auto* text = sqlite3_column_text(statement, column);
	return text ? reinterpret_cast<const char*>(text) : std::string{};
}

std::string utc_now() {
	const auto now = std::chrono::system_clock::now();
	const auto seconds = std::chrono::system_clock::to_time_t(now);
	std::tm utc{};
#if defined(_WIN32)
	gmtime_s(&utc, &seconds);
#else
	gmtime_r(&seconds, &utc);
#endif
	std::ostringstream out;
	out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
	return out.str();
}

std::uint64_t fnv1a64(const std::string& value) noexcept {
	std::uint64_t hash = 1469598103934665603ULL;
	for (const unsigned char c : value) {
		hash ^= static_cast<std::uint64_t>(c);
		hash *= 1099511628211ULL;
	}
	return hash;
}

std::string stable_id(const char* prefix, const std::string& seed) {
	std::ostringstream out;
	out << prefix << '-' << std::hex << std::setfill('0') << std::setw(16) << fnv1a64(seed);
	return out.str();
}

void bind_text(sqlite3_stmt* statement, int index, const std::string& value) {
	if (sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
		throw std::runtime_error("failed to bind governed incident lifecycle value");
	}
}

void step_done(sqlite3* db, sqlite3_stmt* statement, const char* context) {
	if (sqlite3_step(statement) != SQLITE_DONE) {
		throw std::runtime_error(sqlite_message(db, context));
	}
}

void require_identity(const std::string& actor, const std::string& reason) {
	if (actor.empty()) throw std::invalid_argument("governed incident lifecycle actor is required");
	if (reason.empty()) throw std::invalid_argument("governed incident lifecycle reason is required");
}

struct RequestRow {
	std::string request_id;
	std::string source_correlation_id;
	std::string actor;
	std::string reason;
	std::string status;
};

RequestRow load_request(sqlite3* db, const std::string& request_id) {
	Statement statement(
		db,
		"SELECT request_id,source_correlation_id,actor,reason,status FROM operations_incident_requests WHERE request_id=?;",
		"failed to prepare governed incident request lookup");
	bind_text(statement.get(), 1, request_id);
	if (sqlite3_step(statement.get()) != SQLITE_ROW) {
		throw std::runtime_error("governed incident request not found: " + request_id);
	}
	RequestRow row;
	row.request_id = column_text(statement.get(), 0);
	row.source_correlation_id = column_text(statement.get(), 1);
	row.actor = column_text(statement.get(), 2);
	row.reason = column_text(statement.get(), 3);
	row.status = column_text(statement.get(), 4);
	return row;
}

struct IncidentRow {
	std::string incident_id;
	std::string request_id;
	std::string source_correlation_id;
	std::string state;
};

IncidentRow load_incident(sqlite3* db, const std::string& incident_id) {
	Statement statement(
		db,
		"SELECT incident_id,request_id,source_correlation_id,state FROM operations_incidents WHERE incident_id=?;",
		"failed to prepare governed incident lookup");
	bind_text(statement.get(), 1, incident_id);
	if (sqlite3_step(statement.get()) != SQLITE_ROW) {
		throw std::runtime_error("governed incident not found: " + incident_id);
	}
	IncidentRow row;
	row.incident_id = column_text(statement.get(), 0);
	row.request_id = column_text(statement.get(), 1);
	row.source_correlation_id = column_text(statement.get(), 2);
	row.state = column_text(statement.get(), 3);
	return row;
}

void append_history(
	sqlite3* db,
	const std::string& event_id,
	const std::string& request_id,
	const std::string& incident_id,
	const std::string& source_correlation_id,
	const std::string& event_type,
	const std::string& from_state,
	const std::string& to_state,
	const std::string& actor,
	const std::string& reason,
	const std::string& reconciliation_evidence_id,
	const std::string& timestamp) {
	Statement statement(
		db,
		"INSERT INTO operations_incident_history("
		"event_id,request_id,incident_id,source_correlation_id,event_type,from_state,to_state,action,actor,reason,"
		"reconciliation_evidence_id,timestamp_utc,execution_authorized"
		") VALUES(?,?,?,?,?,?,?,'OPEN_INCIDENT',?,?,?,?,0);",
		"failed to prepare governed incident history insert");
	bind_text(statement.get(), 1, event_id);
	bind_text(statement.get(), 2, request_id);
	bind_text(statement.get(), 3, incident_id);
	bind_text(statement.get(), 4, source_correlation_id);
	bind_text(statement.get(), 5, event_type);
	bind_text(statement.get(), 6, from_state);
	bind_text(statement.get(), 7, to_state);
	bind_text(statement.get(), 8, actor);
	bind_text(statement.get(), 9, reason);
	bind_text(statement.get(), 10, reconciliation_evidence_id);
	bind_text(statement.get(), 11, timestamp);
	step_done(db, statement.get(), "failed to persist governed incident history");
}

void transition_incident(
	sqlite3* db,
	const IncidentRow& incident,
	const std::vector<std::string>& allowed_states,
	const std::string& next_state,
	const std::string& event_type,
	const std::string& actor,
	const std::string& reason,
	const std::string& reconciliation_evidence_id) {
	if (std::find(allowed_states.begin(), allowed_states.end(), incident.state) == allowed_states.end()) {
		throw std::logic_error(
			"invalid governed incident transition from " + incident.state + " to " + next_state);
	}
	const auto timestamp = utc_now();
	Statement update(
		db,
		"UPDATE operations_incidents SET state=?,last_actor=?,last_reason=?,reconciliation_evidence_id=?,updated_at_utc=? "
		"WHERE incident_id=? AND state=?;",
		"failed to prepare governed incident transition");
	bind_text(update.get(), 1, next_state);
	bind_text(update.get(), 2, actor);
	bind_text(update.get(), 3, reason);
	bind_text(update.get(), 4, reconciliation_evidence_id);
	bind_text(update.get(), 5, timestamp);
	bind_text(update.get(), 6, incident.incident_id);
	bind_text(update.get(), 7, incident.state);
	step_done(db, update.get(), "failed to persist governed incident transition");
	if (sqlite3_changes(db) != 1) {
		throw std::runtime_error("governed incident transition lost current-state match");
	}

	append_history(
		db,
		incident.incident_id + ":" + next_state,
		incident.request_id,
		incident.incident_id,
		incident.source_correlation_id,
		event_type,
		incident.state,
		next_state,
		actor,
		reason,
		reconciliation_evidence_id,
		timestamp);
}

std::size_t bounded(std::size_t requested, std::size_t maximum) noexcept {
	return std::clamp<std::size_t>(requested, 1, maximum);
}

std::size_t scalar_count(sqlite3* db, const char* sql) {
	Statement statement(db, sql, "failed to prepare governed incident count");
	if (sqlite3_step(statement.get()) != SQLITE_ROW) {
		throw std::runtime_error(sqlite_message(db, "failed to read governed incident count"));
	}
	return static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0));
}

} // namespace

GovernedIncidentLifecycleRepository::GovernedIncidentLifecycleRepository(
	std::string path,
	GovernedIncidentLifecycleOpenMode mode)
	: path_(std::move(path)), mode_(mode) {
	if (path_.empty()) throw std::invalid_argument("governed incident lifecycle database path is empty");

	const auto flags = mode_ == GovernedIncidentLifecycleOpenMode::ReadOnly
		? SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX
		: SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
	if (sqlite3_open_v2(path_.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
		const auto message = sqlite_message(db_, "failed to open governed incident lifecycle database");
		if (db_) sqlite3_close(db_);
		db_ = nullptr;
		throw std::runtime_error(message);
	}

	try {
		sqlite3_busy_timeout(db_, mode_ == GovernedIncidentLifecycleOpenMode::ReadOnly ? 1000 : 5000);
		exec_or_throw("PRAGMA foreign_keys=ON;");
		if (mode_ == GovernedIncidentLifecycleOpenMode::ReadOnly) return;

		exec_or_throw("PRAGMA journal_mode=WAL;");
		exec_or_throw("PRAGMA synchronous=NORMAL;");
		exec_or_throw(
			"CREATE TABLE IF NOT EXISTS operations_incident_requests ("
			"request_id TEXT PRIMARY KEY,"
			"source_correlation_id TEXT NOT NULL UNIQUE,"
			"source TEXT NOT NULL,"
			"action TEXT NOT NULL CHECK(action='OPEN_INCIDENT'),"
			"classification TEXT NOT NULL CHECK(classification='APPROVAL_REQUIRED'),"
			"actor TEXT NOT NULL,"
			"reason TEXT NOT NULL,"
			"git_sha TEXT NOT NULL,"
			"runtime_evidence_id TEXT NOT NULL,"
			"status TEXT NOT NULL CHECK(status IN ('APPROVAL_PENDING','APPROVED','DENIED')),"
			"created_at_utc TEXT NOT NULL,"
			"decided_at_utc TEXT NOT NULL DEFAULT '',"
			"approver TEXT NOT NULL DEFAULT '',"
			"decision_reason TEXT NOT NULL DEFAULT '',"
			"execution_authorized INTEGER NOT NULL CHECK(execution_authorized=0)"
			");");
		exec_or_throw(
			"CREATE INDEX IF NOT EXISTS idx_operations_incident_requests_status_created "
			"ON operations_incident_requests(status,created_at_utc DESC);");
		exec_or_throw(
			"CREATE TABLE IF NOT EXISTS operations_incident_approvals ("
			"sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
			"request_id TEXT NOT NULL,"
			"approver TEXT NOT NULL,"
			"decision TEXT NOT NULL CHECK(decision IN ('APPROVED','DENIED')),"
			"reason TEXT NOT NULL,"
			"decided_at_utc TEXT NOT NULL,"
			"execution_authorized INTEGER NOT NULL CHECK(execution_authorized=0),"
			"FOREIGN KEY(request_id) REFERENCES operations_incident_requests(request_id)"
			");");
		exec_or_throw(
			"CREATE UNIQUE INDEX IF NOT EXISTS ux_operations_incident_approval_decision "
			"ON operations_incident_approvals(request_id);");
		exec_or_throw(
			"CREATE TABLE IF NOT EXISTS operations_incidents ("
			"incident_id TEXT PRIMARY KEY,"
			"request_id TEXT NOT NULL UNIQUE,"
			"source_correlation_id TEXT NOT NULL,"
			"state TEXT NOT NULL CHECK(state IN ('OPEN','ACKNOWLEDGED','RECOVERY_IN_PROGRESS','RESOLVED','CLOSED')),"
			"opened_at_utc TEXT NOT NULL,"
			"updated_at_utc TEXT NOT NULL,"
			"last_actor TEXT NOT NULL,"
			"last_reason TEXT NOT NULL,"
			"reconciliation_evidence_id TEXT NOT NULL DEFAULT '',"
			"execution_authorized INTEGER NOT NULL CHECK(execution_authorized=0),"
			"FOREIGN KEY(request_id) REFERENCES operations_incident_requests(request_id)"
			");");
		exec_or_throw(
			"CREATE INDEX IF NOT EXISTS idx_operations_incidents_state_updated "
			"ON operations_incidents(state,updated_at_utc DESC);");
		exec_or_throw(
			"CREATE TABLE IF NOT EXISTS operations_incident_history ("
			"sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
			"event_id TEXT NOT NULL UNIQUE,"
			"request_id TEXT NOT NULL,"
			"incident_id TEXT NOT NULL,"
			"source_correlation_id TEXT NOT NULL,"
			"event_type TEXT NOT NULL,"
			"from_state TEXT NOT NULL,"
			"to_state TEXT NOT NULL,"
			"action TEXT NOT NULL,"
			"actor TEXT NOT NULL,"
			"reason TEXT NOT NULL,"
			"reconciliation_evidence_id TEXT NOT NULL,"
			"timestamp_utc TEXT NOT NULL,"
			"execution_authorized INTEGER NOT NULL CHECK(execution_authorized=0)"
			");");
		exec_or_throw(
			"CREATE INDEX IF NOT EXISTS idx_operations_incident_history_sequence "
			"ON operations_incident_history(sequence DESC);");
	} catch (...) {
		sqlite3_close(db_);
		db_ = nullptr;
		throw;
	}
}

GovernedIncidentLifecycleRepository::~GovernedIncidentLifecycleRepository() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (db_) sqlite3_close(db_);
}

void GovernedIncidentLifecycleRepository::exec_or_throw(const char* sql) {
	char* error = nullptr;
	if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) == SQLITE_OK) return;
	const std::string message = error ? error : sqlite3_errmsg(db_);
	sqlite3_free(error);
	throw std::runtime_error("governed incident lifecycle schema failure: " + message);
}

void GovernedIncidentLifecycleRepository::require_writable() const {
	if (mode_ == GovernedIncidentLifecycleOpenMode::ReadOnly) {
		throw std::logic_error("governed incident lifecycle repository is read-only");
	}
}

void GovernedIncidentLifecycleRepository::begin_transaction() {
	exec_or_throw("BEGIN IMMEDIATE;");
}

void GovernedIncidentLifecycleRepository::commit_transaction() {
	exec_or_throw("COMMIT;");
}

void GovernedIncidentLifecycleRepository::rollback_transaction() noexcept {
	char* error = nullptr;
	sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &error);
	sqlite3_free(error);
}

IncidentRequestSubmission GovernedIncidentLifecycleRepository::submit_open_incident_request(
	const std::string& source_correlation_id,
	const std::string& actor,
	const std::string& reason,
	const std::string& git_sha,
	const std::string& runtime_evidence_id) {
	require_writable();
	require_identity(actor, reason);
	if (source_correlation_id.empty()) {
		throw std::invalid_argument("governed incident source correlation id is required");
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const auto request_id = stable_id("incident-request", source_correlation_id);
	const auto timestamp = utc_now();

	begin_transaction();
	try {
		Statement insert(
			db_,
			"INSERT OR IGNORE INTO operations_incident_requests("
			"request_id,source_correlation_id,source,action,classification,actor,reason,git_sha,runtime_evidence_id,"
			"status,created_at_utc,execution_authorized"
			") VALUES(?,?,'NOTIFICATION_OPERATIONS','OPEN_INCIDENT','APPROVAL_REQUIRED',?,?,?,?,"
			"'APPROVAL_PENDING',?,0);",
			"failed to prepare governed incident request insert");
		bind_text(insert.get(), 1, request_id);
		bind_text(insert.get(), 2, source_correlation_id);
		bind_text(insert.get(), 3, actor);
		bind_text(insert.get(), 4, reason);
		bind_text(insert.get(), 5, git_sha);
		bind_text(insert.get(), 6, runtime_evidence_id);
		bind_text(insert.get(), 7, timestamp);
		step_done(db_, insert.get(), "failed to persist governed incident request");
		const bool created = sqlite3_changes(db_) > 0;

		if (created) {
			append_history(
				db_,
				request_id + ":REQUEST_SUBMITTED",
				request_id,
				"",
				source_correlation_id,
				"REQUEST_SUBMITTED",
				"NONE",
				"APPROVAL_PENDING",
				actor,
				reason,
				"",
				timestamp);
		} else {
			Statement existing(
				db_,
				"SELECT request_id FROM operations_incident_requests WHERE source_correlation_id=?;",
				"failed to prepare governed incident idempotency lookup");
			bind_text(existing.get(), 1, source_correlation_id);
			if (sqlite3_step(existing.get()) != SQLITE_ROW || column_text(existing.get(), 0) != request_id) {
				throw std::runtime_error("governed incident request correlation conflict");
			}
		}

		commit_transaction();
		return {request_id, created};
	} catch (...) {
		rollback_transaction();
		throw;
	}
}

IncidentApprovalResult GovernedIncidentLifecycleRepository::approve_open_incident_request(
	const std::string& request_id,
	const std::string& approver,
	const std::string& reason,
	const std::string& expected_source_correlation_id) {
	require_writable();
	require_identity(approver, reason);
	if (request_id.empty()) throw std::invalid_argument("governed incident request id is required");

	std::lock_guard<std::mutex> lock(mutex_);
	begin_transaction();
	try {
		const auto request = load_request(db_, request_id);
		if (!expected_source_correlation_id.empty() &&
			request.source_correlation_id != expected_source_correlation_id) {
			throw std::logic_error("governed incident approval correlation mismatch");
		}
		if (request.status != "APPROVAL_PENDING") {
			throw std::logic_error("governed incident approval is stale or already decided");
		}

		const auto timestamp = utc_now();
		Statement approval(
			db_,
			"INSERT INTO operations_incident_approvals(request_id,approver,decision,reason,decided_at_utc,execution_authorized) "
			"VALUES(?,?,'APPROVED',?,?,0);",
			"failed to prepare governed incident approval insert");
		bind_text(approval.get(), 1, request_id);
		bind_text(approval.get(), 2, approver);
		bind_text(approval.get(), 3, reason);
		bind_text(approval.get(), 4, timestamp);
		step_done(db_, approval.get(), "failed to persist governed incident approval");

		Statement update_request(
			db_,
			"UPDATE operations_incident_requests SET status='APPROVED',decided_at_utc=?,approver=?,decision_reason=? "
			"WHERE request_id=? AND status='APPROVAL_PENDING';",
			"failed to prepare governed incident approval state update");
		bind_text(update_request.get(), 1, timestamp);
		bind_text(update_request.get(), 2, approver);
		bind_text(update_request.get(), 3, reason);
		bind_text(update_request.get(), 4, request_id);
		step_done(db_, update_request.get(), "failed to persist governed incident approval state");
		if (sqlite3_changes(db_) != 1) throw std::runtime_error("governed incident approval lost pending-state match");

		append_history(
			db_,
			request_id + ":APPROVED",
			request_id,
			"",
			request.source_correlation_id,
			"APPROVAL_DECIDED",
			"APPROVAL_PENDING",
			"APPROVED",
			approver,
			reason,
			"",
			timestamp);

		const auto incident_id = stable_id("incident", request_id);
		Statement incident(
			db_,
			"INSERT INTO operations_incidents("
			"incident_id,request_id,source_correlation_id,state,opened_at_utc,updated_at_utc,last_actor,last_reason,"
			"reconciliation_evidence_id,execution_authorized"
			") VALUES(?,?,?,'OPEN',?,?,?,?, '',0);",
			"failed to prepare governed incident open");
		bind_text(incident.get(), 1, incident_id);
		bind_text(incident.get(), 2, request_id);
		bind_text(incident.get(), 3, request.source_correlation_id);
		bind_text(incident.get(), 4, timestamp);
		bind_text(incident.get(), 5, timestamp);
		bind_text(incident.get(), 6, approver);
		bind_text(incident.get(), 7, reason);
		step_done(db_, incident.get(), "failed to persist governed incident open");

		append_history(
			db_,
			incident_id + ":OPEN",
			request_id,
			incident_id,
			request.source_correlation_id,
			"INCIDENT_OPENED",
			"APPROVED",
			"OPEN",
			approver,
			reason,
			"",
			timestamp);

		commit_transaction();
		return {incident_id, true};
	} catch (...) {
		rollback_transaction();
		throw;
	}
}

bool GovernedIncidentLifecycleRepository::deny_open_incident_request(
	const std::string& request_id,
	const std::string& approver,
	const std::string& reason,
	const std::string& expected_source_correlation_id) {
	require_writable();
	require_identity(approver, reason);
	if (request_id.empty()) throw std::invalid_argument("governed incident request id is required");

	std::lock_guard<std::mutex> lock(mutex_);
	begin_transaction();
	try {
		const auto request = load_request(db_, request_id);
		if (!expected_source_correlation_id.empty() &&
			request.source_correlation_id != expected_source_correlation_id) {
			throw std::logic_error("governed incident denial correlation mismatch");
		}
		if (request.status != "APPROVAL_PENDING") {
			throw std::logic_error("governed incident denial is stale or already decided");
		}

		const auto timestamp = utc_now();
		Statement approval(
			db_,
			"INSERT INTO operations_incident_approvals(request_id,approver,decision,reason,decided_at_utc,execution_authorized) "
			"VALUES(?,?,'DENIED',?,?,0);",
			"failed to prepare governed incident denial insert");
		bind_text(approval.get(), 1, request_id);
		bind_text(approval.get(), 2, approver);
		bind_text(approval.get(), 3, reason);
		bind_text(approval.get(), 4, timestamp);
		step_done(db_, approval.get(), "failed to persist governed incident denial");

		Statement update_request(
			db_,
			"UPDATE operations_incident_requests SET status='DENIED',decided_at_utc=?,approver=?,decision_reason=? "
			"WHERE request_id=? AND status='APPROVAL_PENDING';",
			"failed to prepare governed incident denial state update");
		bind_text(update_request.get(), 1, timestamp);
		bind_text(update_request.get(), 2, approver);
		bind_text(update_request.get(), 3, reason);
		bind_text(update_request.get(), 4, request_id);
		step_done(db_, update_request.get(), "failed to persist governed incident denial state");
		if (sqlite3_changes(db_) != 1) throw std::runtime_error("governed incident denial lost pending-state match");

		append_history(
			db_,
			request_id + ":DENIED",
			request_id,
			"",
			request.source_correlation_id,
			"APPROVAL_DECIDED",
			"APPROVAL_PENDING",
			"DENIED",
			approver,
			reason,
			"",
			timestamp);

		commit_transaction();
		return true;
	} catch (...) {
		rollback_transaction();
		throw;
	}
}

bool GovernedIncidentLifecycleRepository::acknowledge_incident(
	const std::string& incident_id,
	const std::string& actor,
	const std::string& reason) {
	require_writable();
	require_identity(actor, reason);
	std::lock_guard<std::mutex> lock(mutex_);
	begin_transaction();
	try {
		const auto incident = load_incident(db_, incident_id);
		transition_incident(db_, incident, {"OPEN"}, "ACKNOWLEDGED", "INCIDENT_ACKNOWLEDGED", actor, reason, "");
		commit_transaction();
		return true;
	} catch (...) {
		rollback_transaction();
		throw;
	}
}

bool GovernedIncidentLifecycleRepository::begin_recovery(
	const std::string& incident_id,
	const std::string& actor,
	const std::string& reason,
	const std::string& reconciliation_evidence_id) {
	require_writable();
	require_identity(actor, reason);
	if (reconciliation_evidence_id.empty()) {
		throw std::invalid_argument("governed incident recovery requires reconciliation evidence");
	}
	std::lock_guard<std::mutex> lock(mutex_);
	begin_transaction();
	try {
		const auto incident = load_incident(db_, incident_id);
		transition_incident(
			db_,
			incident,
			{"ACKNOWLEDGED"},
			"RECOVERY_IN_PROGRESS",
			"RECOVERY_STARTED",
			actor,
			reason,
			reconciliation_evidence_id);
		commit_transaction();
		return true;
	} catch (...) {
		rollback_transaction();
		throw;
	}
}

bool GovernedIncidentLifecycleRepository::resolve_incident(
	const std::string& incident_id,
	const std::string& actor,
	const std::string& reason) {
	require_writable();
	require_identity(actor, reason);
	std::lock_guard<std::mutex> lock(mutex_);
	begin_transaction();
	try {
		const auto incident = load_incident(db_, incident_id);
		transition_incident(
			db_,
			incident,
			{"ACKNOWLEDGED", "RECOVERY_IN_PROGRESS"},
			"RESOLVED",
			"INCIDENT_RESOLVED",
			actor,
			reason,
			incident.state == "RECOVERY_IN_PROGRESS" ? "PRESERVED" : "");
		commit_transaction();
		return true;
	} catch (...) {
		rollback_transaction();
		throw;
	}
}

bool GovernedIncidentLifecycleRepository::close_incident(
	const std::string& incident_id,
	const std::string& actor,
	const std::string& reason) {
	require_writable();
	require_identity(actor, reason);
	std::lock_guard<std::mutex> lock(mutex_);
	begin_transaction();
	try {
		const auto incident = load_incident(db_, incident_id);
		transition_incident(db_, incident, {"RESOLVED"}, "CLOSED", "INCIDENT_CLOSED", actor, reason, "");
		commit_transaction();
		return true;
	} catch (...) {
		rollback_transaction();
		throw;
	}
}

std::optional<std::string> GovernedIncidentLifecycleRepository::request_id_for_correlation(
	const std::string& source_correlation_id) const {
	std::lock_guard<std::mutex> lock(mutex_);
	Statement statement(
		db_,
		"SELECT request_id FROM operations_incident_requests WHERE source_correlation_id=?;",
		"failed to prepare governed incident correlation lookup");
	bind_text(statement.get(), 1, source_correlation_id);
	const auto result = sqlite3_step(statement.get());
	if (result == SQLITE_DONE) return std::nullopt;
	if (result != SQLITE_ROW) throw std::runtime_error(sqlite_message(db_, "failed to read governed incident correlation"));
	return column_text(statement.get(), 0);
}

std::optional<std::string> GovernedIncidentLifecycleRepository::incident_id_for_request(
	const std::string& request_id) const {
	std::lock_guard<std::mutex> lock(mutex_);
	Statement statement(
		db_,
		"SELECT incident_id FROM operations_incidents WHERE request_id=?;",
		"failed to prepare governed incident request lookup");
	bind_text(statement.get(), 1, request_id);
	const auto result = sqlite3_step(statement.get());
	if (result == SQLITE_DONE) return std::nullopt;
	if (result != SQLITE_ROW) throw std::runtime_error(sqlite_message(db_, "failed to read governed incident request"));
	return column_text(statement.get(), 0);
}

nlohmann::json GovernedIncidentLifecycleRepository::control_plane_snapshot(
	std::size_t approval_limit,
	std::size_t audit_limit) const {
	std::lock_guard<std::mutex> lock(mutex_);
	const auto approval_bound = bounded(approval_limit, 64);
	const auto audit_bound = bounded(audit_limit, kMaximumAuditLimit);

	nlohmann::json control_plane = {
		{"governance_state", "CONTROLLED"},
		{"evidence_status", "AVAILABLE"},
		{"incident_state", "NONE"},
		{"recovery_state", "IDLE"},
		{"pending_approvals", 0},
		{"approval_queue", nlohmann::json::array()},
		{"audit_timeline", nlohmann::json::array()},
		{"incident_workflow", nlohmann::json::object()},
		{"recovery_workflow", nlohmann::json::object()}
	};

	const auto pending_total = scalar_count(
		db_,
		"SELECT COUNT(*) FROM operations_incident_requests WHERE status='APPROVAL_PENDING';");
	control_plane["pending_approvals"] = pending_total;

	{
		Statement statement(
			db_,
			"SELECT request_id,source_correlation_id,actor,reason,created_at_utc "
			"FROM operations_incident_requests WHERE status='APPROVAL_PENDING' "
			"ORDER BY created_at_utc ASC,request_id ASC LIMIT ?;",
			"failed to prepare governed incident approval queue");
		sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(approval_bound));
		while (true) {
			const auto result = sqlite3_step(statement.get());
			if (result == SQLITE_DONE) break;
			if (result != SQLITE_ROW) throw std::runtime_error(sqlite_message(db_, "failed to read governed incident approval queue"));
			control_plane["approval_queue"].push_back({
				{"request_id", column_text(statement.get(), 0)},
				{"source_correlation_id", column_text(statement.get(), 1)},
				{"action", "OPEN_INCIDENT"},
				{"classification", "APPROVAL_REQUIRED"},
				{"actor", column_text(statement.get(), 2)},
				{"reason", column_text(statement.get(), 3)},
				{"status", "PENDING"},
				{"created_at_utc", column_text(statement.get(), 4)},
				{"execution_authorized", false}
			});
		}
	}

	{
		Statement statement(
			db_,
			"SELECT request_id,incident_id,source_correlation_id,event_type,from_state,to_state,actor,reason,"
			"reconciliation_evidence_id,timestamp_utc "
			"FROM operations_incident_history ORDER BY sequence DESC LIMIT ?;",
			"failed to prepare governed incident audit timeline");
		sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(audit_bound));
		while (true) {
			const auto result = sqlite3_step(statement.get());
			if (result == SQLITE_DONE) break;
			if (result != SQLITE_ROW) throw std::runtime_error(sqlite_message(db_, "failed to read governed incident audit timeline"));
			control_plane["audit_timeline"].push_back({
				{"request_id", column_text(statement.get(), 0)},
				{"incident_id", column_text(statement.get(), 1)},
				{"source_correlation_id", column_text(statement.get(), 2)},
				{"event_type", column_text(statement.get(), 3)},
				{"from_state", column_text(statement.get(), 4)},
				{"to_state", column_text(statement.get(), 5)},
				{"action", "OPEN_INCIDENT"},
				{"actor", column_text(statement.get(), 6)},
				{"reason", column_text(statement.get(), 7)},
				{"reconciliation_evidence_id", column_text(statement.get(), 8)},
				{"timestamp_utc", column_text(statement.get(), 9)},
				{"outcome", column_text(statement.get(), 5)},
				{"execution_authorized", false}
			});
		}
	}

	bool projected = false;
	{
		Statement active(
			db_,
			"SELECT incident_id,request_id,source_correlation_id,state,last_actor,last_reason,reconciliation_evidence_id,updated_at_utc "
			"FROM operations_incidents WHERE state!='CLOSED' ORDER BY updated_at_utc DESC,incident_id DESC LIMIT 1;",
			"failed to prepare governed active incident projection");
		if (sqlite3_step(active.get()) == SQLITE_ROW) {
			const auto state = column_text(active.get(), 3);
			control_plane["incident_state"] = state;
			control_plane["recovery_state"] = state == "RECOVERY_IN_PROGRESS" ? "IN_PROGRESS" :
				(state == "RESOLVED" ? "RESOLVED" : "IDLE");
			control_plane["incident_workflow"] = {
				{"state", state},
				{"action", ""},
				{"classification", "FORBIDDEN"},
				{"incident_id", column_text(active.get(), 0)},
				{"request_id", column_text(active.get(), 1)},
				{"source_correlation_id", column_text(active.get(), 2)},
				{"actor", column_text(active.get(), 4)},
				{"reason", column_text(active.get(), 5)},
				{"updated_at_utc", column_text(active.get(), 7)},
				{"execution_authorized", false}
			};
			if (state == "RECOVERY_IN_PROGRESS") {
				control_plane["recovery_workflow"] = {
					{"state", "IN_PROGRESS"},
					{"incident_id", column_text(active.get(), 0)},
					{"request_id", column_text(active.get(), 1)},
					{"reconciliation_evidence_id", column_text(active.get(), 6)},
					{"execution_authorized", false}
				};
			}
			projected = true;
		}
	}

	if (!projected && pending_total > 0) {
		Statement pending(
			db_,
			"SELECT request_id,source_correlation_id,actor,reason,created_at_utc "
			"FROM operations_incident_requests WHERE status='APPROVAL_PENDING' "
			"ORDER BY created_at_utc ASC,request_id ASC LIMIT 1;",
			"failed to prepare governed pending incident projection");
		if (sqlite3_step(pending.get()) == SQLITE_ROW) {
			control_plane["incident_state"] = "APPROVAL_PENDING";
			control_plane["incident_workflow"] = {
				{"state", "APPROVAL_PENDING"},
				{"action", "OPEN_INCIDENT"},
				{"classification", "APPROVAL_REQUIRED"},
				{"request_id", column_text(pending.get(), 0)},
				{"source_correlation_id", column_text(pending.get(), 1)},
				{"actor", column_text(pending.get(), 2)},
				{"reason", column_text(pending.get(), 3)},
				{"created_at_utc", column_text(pending.get(), 4)},
				{"execution_authorized", false}
			};
			projected = true;
		}
	}

	if (!projected) {
		Statement latest(
			db_,
			"SELECT r.request_id,r.source_correlation_id,r.status,r.actor,r.reason,r.created_at_utc,"
			"COALESCE(i.incident_id,''),COALESCE(i.state,'') "
			"FROM operations_incident_requests r LEFT JOIN operations_incidents i ON i.request_id=r.request_id "
			"ORDER BY r.created_at_utc DESC,r.request_id DESC LIMIT 1;",
			"failed to prepare governed incident latest projection");
		if (sqlite3_step(latest.get()) == SQLITE_ROW) {
			const auto request_status = column_text(latest.get(), 2);
			const auto incident_state = column_text(latest.get(), 7);
			const auto state = !incident_state.empty() ? incident_state : request_status;
			control_plane["incident_state"] = state == "CLOSED" ? "NONE" : state;
			control_plane["recovery_state"] = state == "CLOSED" || state == "RESOLVED" ? state : "IDLE";
			control_plane["incident_workflow"] = {
				{"state", state},
				{"action", request_status == "APPROVAL_PENDING" ? "OPEN_INCIDENT" : ""},
				{"classification", request_status == "APPROVAL_PENDING" ? "APPROVAL_REQUIRED" : "FORBIDDEN"},
				{"request_id", column_text(latest.get(), 0)},
				{"source_correlation_id", column_text(latest.get(), 1)},
				{"actor", column_text(latest.get(), 3)},
				{"reason", column_text(latest.get(), 4)},
				{"created_at_utc", column_text(latest.get(), 5)},
				{"incident_id", column_text(latest.get(), 6)},
				{"execution_authorized", false}
			};
		}
	}

	control_plane["approval_queue_total"] = pending_total;
	control_plane["approval_queue_truncated"] = pending_total > control_plane["approval_queue"].size();
	control_plane["audit_timeline_truncated"] =
		scalar_count(db_, "SELECT COUNT(*) FROM operations_incident_history;") > control_plane["audit_timeline"].size();
	control_plane["execution_authorized"] = false;
	return control_plane;
}

nlohmann::json merge_governed_incident_lifecycle_snapshot(
	const nlohmann::json& snapshot,
	const GovernedIncidentLifecycleRepository& repository,
	std::size_t approval_limit,
	std::size_t audit_limit) {
	auto merged = snapshot;
	auto control_plane = merged.value("operations_control_plane", nlohmann::json::object());
	if (!control_plane.is_object()) control_plane = nlohmann::json::object();

	const auto lifecycle = repository.control_plane_snapshot(approval_limit, audit_limit);
	for (auto it = lifecycle.begin(); it != lifecycle.end(); ++it) {
		if ((it.key() == "governance_state" || it.key() == "evidence_status") && control_plane.contains(it.key())) {
			continue;
		}
		control_plane[it.key()] = it.value();
	}
	control_plane["execution_authorized"] = false;
	merged["operations_control_plane"] = std::move(control_plane);
	return merged;
}

} // namespace sentum::operations
