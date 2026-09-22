#include <sentum/dashboard/DashboardOperationsOverlay.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

void test_overlay_is_injected_before_body_end() {
	const std::string base = "<html><body><main>base</main></body></html>";
	const auto html = sentum::dashboard::dashboard_html_with_operations(base);
	const auto overlay = html.find("sentum-operations-overlay-script");
	const auto body_end = html.find("</body>");
	require(overlay != std::string::npos, "operations overlay script missing");
	require(body_end != std::string::npos && overlay < body_end, "operations overlay not injected before body end");
	require(html.find("/api/operations") != std::string::npos, "operations endpoint not consumed by overlay");
}

void test_overlay_is_idempotent() {
	const std::string base = "<html><body></body></html>";
	const auto once = sentum::dashboard::dashboard_html_with_operations(base);
	const auto twice = sentum::dashboard::dashboard_html_with_operations(once);
	require(once == twice, "operations overlay injection was not idempotent");
}

void test_overlay_falls_back_without_body_tag() {
	const std::string base = "<html>partial";
	const auto html = sentum::dashboard::dashboard_html_with_operations(base);
	require(html.rfind(base, 0) == 0, "fallback injection replaced base HTML");
	require(html.find("operationsView") != std::string::npos, "fallback injection lost operations view");
}

void test_overlay_exposes_no_write_routes() {
	const std::string overlay(sentum::dashboard::kOperationsDashboardOverlay);
	require(overlay.find("fetch('/api/operations'") != std::string::npos, "overlay does not use operations read endpoint");
	require(overlay.find("method:'POST'") == std::string::npos, "overlay introduced POST request");
	require(overlay.find("method:'PUT'") == std::string::npos, "overlay introduced PUT request");
	require(overlay.find("method:'PATCH'") == std::string::npos, "overlay introduced PATCH request");
	require(overlay.find("method:'DELETE'") == std::string::npos, "overlay introduced DELETE request");
}

void test_overlay_validates_cross_surface_contract() {
	const std::string overlay(sentum::dashboard::kOperationsDashboardOverlay);
	require(overlay.find("OPERATIONS_SCHEMA_VERSION=1") != std::string::npos, "overlay does not validate operations schema version");
	require(overlay.find("sentum.operations.v1") != std::string::npos, "overlay does not validate operations contract id");
	require(overlay.find("READ_ONLY_PRESENTATION") != std::string::npos, "overlay does not validate read-only authority");
	require(overlay.find("Operations contract mismatch - fail closed") != std::string::npos, "contract mismatch does not fail closed visibly");
}

void test_overlay_uses_bounded_resilient_refresh() {
	const std::string overlay(sentum::dashboard::kOperationsDashboardOverlay);
	require(overlay.find("OPERATIONS_REFRESH_MS=2000") != std::string::npos, "operations base refresh interval missing");
	require(overlay.find("OPERATIONS_MAX_BACKOFF_MS=30000") != std::string::npos, "operations backoff is not bounded");
	require(overlay.find("Math.min(OPERATIONS_REFRESH_MS*Math.pow(2") != std::string::npos, "operations retry does not use bounded exponential backoff");
	require(overlay.find("setInterval(") == std::string::npos, "operations overlay still uses unbounded interval polling");
	require(overlay.find("setTimeout(") != std::string::npos, "operations overlay does not use single-shot scheduling");
	require(overlay.find("operationsInFlight") != std::string::npos, "operations overlay does not suppress overlapping requests");
}

void test_overlay_exposes_transport_stale_and_recovery_state() {
	const std::string overlay(sentum::dashboard::kOperationsDashboardOverlay);
	require(overlay.find("opsTransport") != std::string::npos, "transport state is not visible");
	require(overlay.find("renderTransport('STALE'") != std::string::npos, "transport failure does not mark cached evidence stale");
	require(overlay.find("renderTransport('UNAVAILABLE'") != std::string::npos, "initial transport failure is not unavailable");
	require(overlay.find("renderTransport('LIVE')") != std::string::npos, "successful recovery does not restore live state");
	require(overlay.find("operationsLastSuccess") != std::string::npos, "last successful transport timestamp is not tracked");
}

void test_overlay_renders_read_only_escalation_timeline() {
	const std::string overlay(sentum::dashboard::kOperationsDashboardOverlay);
	require(overlay.find("Escalation Timeline") != std::string::npos, "escalation timeline is not visible");
	require(overlay.find("opsEscalationState") != std::string::npos, "due/overdue escalation summary missing");
	require(overlay.find("alert_escalation") != std::string::npos, "web overlay does not consume escalation contract");
	require(overlay.find("OVERDUE") != std::string::npos, "overdue escalation styling missing");
}

void test_overlay_renders_notification_operations_read_only() {
	const std::string overlay(sentum::dashboard::kOperationsDashboardOverlay);
	require(overlay.find("notification_operations") != std::string::npos, "notification operations contract is not consumed");
	require(overlay.find("opsNotificationHealth") != std::string::npos, "notification health is not visible");
	require(overlay.find("dispatch_runtime") != std::string::npos, "notification dispatch runtime metrics are not consumed");
	require(overlay.find("opsNotificationDispatch") != std::string::npos, "notification dispatch runtime status is not visible");
	require(overlay.find("opsNotificationQueue") != std::string::npos, "notification dispatch queue metrics are not visible");
	require(overlay.find("opsNotificationTimeouts") != std::string::npos, "notification dispatch timeout metrics are not visible");
	require(overlay.find("opsNotificationLatency") != std::string::npos, "notification provider latency is not visible");
	require(overlay.find("opsNotificationBacklog") != std::string::npos, "notification backlog is not visible");
	require(overlay.find("opsNotificationFailures") != std::string::npos, "terminal notification failures are not visible");
	require(overlay.find("opsNotificationIncident") != std::string::npos, "notification incident candidate is not visible");
	require(overlay.find("Notification Channels") != std::string::npos, "notification channel projection is not visible");
	require(overlay.find("INCIDENT_CANDIDATE") != std::string::npos, "incident candidate is not visually fail-closed");
}

void test_overlay_renders_governed_notification_incident_workflow() {
	const std::string overlay(sentum::dashboard::kOperationsDashboardOverlay);
	require(overlay.find("Notification Incident Workflow") != std::string::npos, "notification incident workflow section missing");
	require(overlay.find("notification_incident_workflow") != std::string::npos, "notification incident workflow contract is not consumed");
	require(overlay.find("opsNotificationIncidentWorkflow") != std::string::npos, "notification incident workflow target missing");
	require(overlay.find("PROPOSAL_READY") != std::string::npos, "proposal-ready state is not visibly represented");
	require(overlay.find("approval_status") != std::string::npos, "approval evidence is not rendered");
	require(overlay.find("recovery_state") != std::string::npos, "recovery evidence is not rendered");
}

} // namespace

int main() {
	try {
		test_overlay_is_injected_before_body_end();
		test_overlay_is_idempotent();
		test_overlay_falls_back_without_body_tag();
		test_overlay_exposes_no_write_routes();
		test_overlay_validates_cross_surface_contract();
		test_overlay_uses_bounded_resilient_refresh();
		test_overlay_exposes_transport_stale_and_recovery_state();
		test_overlay_renders_read_only_escalation_timeline();
		test_overlay_renders_notification_operations_read_only();
		test_overlay_renders_governed_notification_incident_workflow();
		std::cout << "dashboard operations overlay tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "dashboard operations overlay test failure: " << error.what() << '\n';
		return 1;
	}
}
