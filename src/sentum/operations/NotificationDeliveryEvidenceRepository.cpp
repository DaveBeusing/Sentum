#include <sentum/operations/NotificationDeliveryEvidenceRepository.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include <sqlite3.h>

namespace sentum::operations {
namespace {

std::string sqlite_message(sqlite3* db, const char* context) {
	const auto* message = db ? sqlite3_errmsg(db) : "unknown SQLite error";
	return std::string(context) + ": " + message;
}

std::string column_text(sqlite3_stmt* statement, int column) {
	const auto* text = sqlite3_column_text(statement, column);
	return text ? reinterpret_cast<const char*>(text) : std::string{};
}

NotificationDeliveryEvidenceRecord read_record(sqlite3_stmt* statement) {
	NotificationDeliveryEvidenceRecord record;
	record.dedup_key = column_text(statement, 1);
	record.alert_id = column_text(statement, 2);
	record.generation = static_cast<std::size_t>(sqlite3_column_int64(statement, 3));
	record.channel = column_text(statement, 4);
	record.audience = column_text(statement, 5);
	record.state = column_text(statement, 6);
	record.attempt = static_cast<std::size_t>(sqlite3_column_int64(statement, 7));
	record.max_attempts = static_cast<std::size_t>(sqlite3_column_int64(statement, 8));
	record.retry_backoff_seconds = static_cast<std::size_t>(sqlite3_column_int64(statement, 9));
	record.terminal = sqlite3_column_int(statement, 10) != 0;
	record.provider_reference = column_text(statement, 11);
	record.failure_code = column_text(statement, 12);
	record.failure_reason = column_text(statement, 13);
	record.observed_at_utc = column_text(statement, 14);
	record.delivery_authorized = sqlite3_column_int(statement, 15) != 0;
	record.execution_authorized = false;
	return record;
}

NotificationDeliveryAttempt restore_attempt(const NotificationDeliveryEvidenceRecord& record) {
	NotificationDeliveryAttempt attempt;
	attempt.dedup_key = record.dedup_key;
	attempt.alert_id = record.alert_id;
	attempt.generation = record.generation;
	attempt.channel = record.channel;
	attempt.audience = record.audience;
	if (record.state == "PENDING") {
		attempt.state = NotificationDeliveryState::Pending;
	} else if (record.state == "DISPATCHED") {
		attempt.state = NotificationDeliveryState::Dispatched;
	} else {
		attempt.state = NotificationDeliveryState::Failed;
	}
	attempt.attempt = record.attempt;
	attempt.max_attempts = std::max<std::size_t>(1, record.max_attempts);
	attempt.retry_backoff_seconds = record.retry_backoff_seconds;
	attempt.terminal = record.terminal;
	attempt.provider_reference = record.provider_reference;
	attempt.failure_code = record.failure_code;
	attempt.failure_reason = record.failure_reason;
	attempt.delivery_authorized = record.delivery_authorized;
	attempt.execution_authorized = false;
	return attempt;
}

} // namespace

NotificationDeliveryEvidenceRepository::NotificationDeliveryEvidenceRepository(
	std::string path,
	NotificationDeliveryEvidenceOpenMode mode)
	: path_(std::move(path)), mode_(mode) {
	if (path_.empty()) throw std::invalid_argument("notification delivery evidence database path is empty");

	const auto flags = mode_ == NotificationDeliveryEvidenceOpenMode::ReadOnly
		? SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX
		: SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
	if (sqlite3_open_v2(path_.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
		const auto message = sqlite_message(db_, "failed to open notification delivery evidence database");
		if (db_) sqlite3_close(db_);
		db_ = nullptr;
		throw std::runtime_error(message);
	}

	try {
		sqlite3_busy_timeout(
			db_,
			mode_ == NotificationDeliveryEvidenceOpenMode::ReadOnly ? 1000 : 5000);
		if (mode_ == NotificationDeliveryEvidenceOpenMode::ReadOnly) return;

		exec_or_throw("PRAGMA journal_mode=WAL;");
		exec_or_throw("PRAGMA synchronous=NORMAL;");
		exec_or_throw(
			"CREATE TABLE IF NOT EXISTS notification_delivery_evidence ("
			"sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
			"dedup_key TEXT NOT NULL,"
			"alert_id TEXT NOT NULL,"
			"generation INTEGER NOT NULL,"
			"channel TEXT NOT NULL,"
			"audience TEXT NOT NULL,"
			"state TEXT NOT NULL CHECK(state IN ('PENDING','DISPATCHED','DELIVERED','FAILED')),"
			"attempt INTEGER NOT NULL,"
			"max_attempts INTEGER NOT NULL,"
			"retry_backoff_seconds INTEGER NOT NULL,"
			"terminal INTEGER NOT NULL,"
			"provider_reference TEXT NOT NULL,"
			"failure_code TEXT NOT NULL,"
			"failure_reason TEXT NOT NULL,"
			"observed_at_utc TEXT NOT NULL,"
			"delivery_authorized INTEGER NOT NULL,"
			"execution_authorized INTEGER NOT NULL CHECK(execution_authorized = 0)"
			");");
		exec_or_throw(
			"CREATE UNIQUE INDEX IF NOT EXISTS ux_notification_delivery_transition "
			"ON notification_delivery_evidence(dedup_key,state,attempt,provider_reference,failure_code);");
		exec_or_throw(
			"CREATE INDEX IF NOT EXISTS idx_notification_delivery_dedup_sequence "
			"ON notification_delivery_evidence(dedup_key,sequence DESC);");
		exec_or_throw(
			"CREATE INDEX IF NOT EXISTS idx_notification_delivery_sequence "
			"ON notification_delivery_evidence(sequence DESC);");
		exec_or_throw(
			"CREATE INDEX IF NOT EXISTS idx_notification_delivery_terminal_sequence "
			"ON notification_delivery_evidence(terminal,sequence DESC);");
	} catch (...) {
		sqlite3_close(db_);
		db_ = nullptr;
		throw;
	}
}

NotificationDeliveryEvidenceRepository::~NotificationDeliveryEvidenceRepository() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (db_) sqlite3_close(db_);
}

std::size_t NotificationDeliveryEvidenceRepository::bounded_limit(std::size_t requested) noexcept {
	return std::clamp<std::size_t>(requested, 1, kMaximumQueryLimit);
}

void NotificationDeliveryEvidenceRepository::exec_or_throw(const char* sql) {
	char* error = nullptr;
	if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) == SQLITE_OK) return;
	const std::string message = error ? error : sqlite3_errmsg(db_);
	sqlite3_free(error);
	throw std::runtime_error("notification delivery evidence schema failure: " + message);
}

bool NotificationDeliveryEvidenceRepository::append(const NotificationDeliveryEvidenceRecord& record) {
	if (mode_ == NotificationDeliveryEvidenceOpenMode::ReadOnly) {
		throw std::logic_error("notification delivery evidence repository is read-only");
	}
	if (record.dedup_key.empty() || record.state.empty()) return false;
	if (record.execution_authorized) {
		throw std::invalid_argument("notification delivery evidence cannot authorize execution");
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const char* sql =
		"INSERT OR IGNORE INTO notification_delivery_evidence("
		"dedup_key,alert_id,generation,channel,audience,state,attempt,max_attempts,retry_backoff_seconds,"
		"terminal,provider_reference,failure_code,failure_reason,observed_at_utc,delivery_authorized,execution_authorized"
		") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0);";
	sqlite3_stmt* statement = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
		throw std::runtime_error(sqlite_message(db_, "failed to prepare notification delivery evidence insert"));
	}

	sqlite3_bind_text(statement, 1, record.dedup_key.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(statement, 2, record.alert_id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(record.generation));
	sqlite3_bind_text(statement, 4, record.channel.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(statement, 5, record.audience.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(statement, 6, record.state.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(statement, 7, static_cast<sqlite3_int64>(record.attempt));
	sqlite3_bind_int64(statement, 8, static_cast<sqlite3_int64>(record.max_attempts));
	sqlite3_bind_int64(statement, 9, static_cast<sqlite3_int64>(record.retry_backoff_seconds));
	sqlite3_bind_int(statement, 10, record.terminal ? 1 : 0);
	sqlite3_bind_text(statement, 11, record.provider_reference.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(statement, 12, record.failure_code.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(statement, 13, record.failure_reason.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(statement, 14, record.observed_at_utc.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(statement, 15, record.delivery_authorized ? 1 : 0);

	const auto result = sqlite3_step(statement);
	if (result != SQLITE_DONE) {
		const auto message = sqlite_message(db_, "failed to persist notification delivery evidence");
		sqlite3_finalize(statement);
		throw std::runtime_error(message);
	}
	sqlite3_finalize(statement);
	return sqlite3_changes(db_) > 0;
}

std::size_t NotificationDeliveryEvidenceRepository::scalar_count(const char* sql) const {
	sqlite3_stmt* statement = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
		throw std::runtime_error(sqlite_message(db_, "failed to prepare notification delivery evidence count"));
	}
	const auto result = sqlite3_step(statement);
	if (result != SQLITE_ROW) {
		const auto message = sqlite_message(db_, "failed to read notification delivery evidence count");
		sqlite3_finalize(statement);
		throw std::runtime_error(message);
	}
	const auto count = static_cast<std::size_t>(sqlite3_column_int64(statement, 0));
	sqlite3_finalize(statement);
	return count;
}

NotificationDeliveryEvidenceBatch NotificationDeliveryEvidenceRepository::load_batch(
	const char* sql,
	std::size_t limit,
	std::size_t total) const {
	NotificationDeliveryEvidenceBatch batch;
	batch.total = total;
	const auto bounded = bounded_limit(limit);
	sqlite3_stmt* statement = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
		throw std::runtime_error(sqlite_message(db_, "failed to prepare notification delivery evidence query"));
	}
	sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(bounded));
	while (true) {
		const auto result = sqlite3_step(statement);
		if (result == SQLITE_DONE) break;
		if (result != SQLITE_ROW) {
			const auto message = sqlite_message(db_, "failed to read notification delivery evidence");
			sqlite3_finalize(statement);
			throw std::runtime_error(message);
		}
		PersistedNotificationDeliveryEvidence value;
		value.sequence = sqlite3_column_int64(statement, 0);
		value.record = read_record(statement);
		batch.records.push_back(std::move(value));
	}
	sqlite3_finalize(statement);
	batch.truncated = batch.records.size() < batch.total;
	return batch;
}

NotificationDeliveryEvidenceBatch NotificationDeliveryEvidenceRepository::load_recent(std::size_t limit) const {
	std::lock_guard<std::mutex> lock(mutex_);
	const auto total = scalar_count("SELECT COUNT(*) FROM notification_delivery_evidence;");
	return load_batch(
		"SELECT sequence,dedup_key,alert_id,generation,channel,audience,state,attempt,max_attempts,"
		"retry_backoff_seconds,terminal,provider_reference,failure_code,failure_reason,observed_at_utc,"
		"delivery_authorized,execution_authorized "
		"FROM (SELECT sequence,dedup_key,alert_id,generation,channel,audience,state,attempt,max_attempts,"
		"retry_backoff_seconds,terminal,provider_reference,failure_code,failure_reason,observed_at_utc,"
		"delivery_authorized,execution_authorized FROM notification_delivery_evidence "
		"ORDER BY sequence DESC LIMIT ?) ORDER BY sequence ASC;",
		limit,
		total);
}

NotificationDeliveryEvidenceBatch NotificationDeliveryEvidenceRepository::load_latest_per_dedup_key(
	std::size_t limit) const {
	std::lock_guard<std::mutex> lock(mutex_);
	const auto total = scalar_count("SELECT COUNT(DISTINCT dedup_key) FROM notification_delivery_evidence;");
	return load_batch(
		"SELECT e.sequence,e.dedup_key,e.alert_id,e.generation,e.channel,e.audience,e.state,e.attempt,e.max_attempts,"
		"e.retry_backoff_seconds,e.terminal,e.provider_reference,e.failure_code,e.failure_reason,e.observed_at_utc,"
		"e.delivery_authorized,e.execution_authorized FROM notification_delivery_evidence e "
		"WHERE e.sequence=(SELECT MAX(latest.sequence) FROM notification_delivery_evidence latest "
		"WHERE latest.dedup_key=e.dedup_key) ORDER BY e.sequence ASC LIMIT ?;",
		limit,
		total);
}

std::vector<NotificationDeliveryAttempt> NotificationDeliveryEvidenceRepository::restore_active_delivery_state(
	std::size_t limit) const {
	const auto batch = load_latest_per_dedup_key(limit);
	if (batch.truncated) {
		throw std::runtime_error("notification delivery recovery evidence exceeds bounded query limit");
	}

	std::vector<NotificationDeliveryAttempt> restored;
	restored.reserve(batch.records.size());
	for (const auto& persisted : batch.records) {
		const auto& record = persisted.record;
		if (record.terminal || record.state == "DELIVERED") continue;
		if (record.state != "PENDING" && record.state != "DISPATCHED" && record.state != "FAILED") continue;
		restored.push_back(restore_attempt(record));
	}
	return restored;
}

} // namespace sentum::operations
