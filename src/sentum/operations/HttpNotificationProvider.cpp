#include <sentum/operations/HttpNotificationProvider.hpp>

#include <cstdlib>
#include <memory>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace sentum::operations {
namespace {

std::size_t discard_response(char* data, std::size_t size, std::size_t count, void*) {
	(void)data;
	return size * count;
}

int cancellation_progress(void* client, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	const auto* cancelled = static_cast<const std::atomic<bool>*>(client);
	return cancelled && cancelled->load(std::memory_order_acquire) ? 1 : 0;
}

NotificationDispatchResult failure(
	std::string code,
	std::string reason,
	bool retryable) {
	NotificationDispatchResult result;
	result.accepted = false;
	result.delivered = false;
	result.retryable = retryable;
	result.failure_code = std::move(code);
	result.failure_reason = std::move(reason);
	result.execution_authorized = false;
	return result;
}

} // namespace

HttpNotificationProvider::HttpNotificationProvider(NotificationProviderConfiguration configuration)
	: configuration_(std::move(configuration)) {}

NotificationDispatchResult HttpNotificationProvider::dispatch(
	const NotificationDispatchRequest& request,
	const std::atomic<bool>& cancellation_requested) {
	if (!request.delivery_authorized || request.execution_authorized) {
		return failure("NOT_AUTHORIZED", "notification dispatch request is not transport-authorized", false);
	}
	if (cancellation_requested.load(std::memory_order_acquire)) {
		return failure("CANCELLED", "notification dispatch cancelled before provider I/O", true);
	}

	std::string authorization;
	if (!configuration_.credential_environment.empty()) {
		const auto* credential = std::getenv(configuration_.credential_environment.c_str());
		if (credential == nullptr || *credential == '\0') {
			return failure("CREDENTIAL_UNAVAILABLE", "configured notification credential environment variable is unavailable", false);
		}
		authorization = "Authorization: Bearer ";
		authorization += credential;
	}

	const auto payload = nlohmann::json{
		{"dedup_key", request.dedup_key},
		{"alert_id", request.alert_id},
		{"generation", request.generation},
		{"channel", request.channel},
		{"audience", request.audience},
		{"attempt", request.attempt},
		{"delivery_authorized", request.delivery_authorized},
		{"execution_authorized", false}
	}.dump();

	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
	if (!curl) return failure("PROVIDER_INITIALIZATION", "failed to initialize HTTP notification provider", true);

	curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "Accept: application/json");
	const auto idempotency = std::string("Idempotency-Key: ") + request.dedup_key;
	headers = curl_slist_append(headers, idempotency.c_str());
	if (!authorization.empty()) headers = curl_slist_append(headers, authorization.c_str());

	curl_easy_setopt(curl.get(), CURLOPT_URL, configuration_.endpoint.c_str());
	curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
	curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.c_str());
	curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
	curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(configuration_.connect_timeout_ms));
	curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(configuration_.request_timeout_ms));
	curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "Sentum/notification-dispatch");
	curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &discard_response);
	curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, &cancellation_progress);
	curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &cancellation_requested);

	const auto code = curl_easy_perform(curl.get());
	long status = 0;
	if (code == CURLE_OK) curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);

	if (code == CURLE_ABORTED_BY_CALLBACK && cancellation_requested.load(std::memory_order_acquire)) {
		return failure("CANCELLED", "notification dispatch cancelled during provider I/O", true);
	}
	if (code == CURLE_OPERATION_TIMEDOUT) {
		return failure("TIMEOUT", "notification provider request timed out", true);
	}
	if (code != CURLE_OK) {
		return failure("TRANSPORT_ERROR", curl_easy_strerror(code), true);
	}

	if (status >= 200 && status < 300) {
		NotificationDispatchResult result;
		result.accepted = true;
		result.delivered = true;
		result.retryable = false;
		result.provider_reference = "http-" + std::to_string(status);
		result.execution_authorized = false;
		return result;
	}

	const bool retryable = status == 408 || status == 425 || status == 429 || status >= 500;
	return failure(
		"HTTP_" + std::to_string(status),
		"notification provider returned HTTP status " + std::to_string(status),
		retryable);
}

} // namespace sentum::operations
