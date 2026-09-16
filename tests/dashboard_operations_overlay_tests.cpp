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
	require(html.find("READ_ONLY_PRESENTATION") == std::string::npos, "overlay must render server authority instead of hardcoding it");
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

} // namespace

int main() {
	try {
		test_overlay_is_injected_before_body_end();
		test_overlay_is_idempotent();
		test_overlay_falls_back_without_body_tag();
		test_overlay_exposes_no_write_routes();
		std::cout << "dashboard operations overlay tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "dashboard operations overlay test failure: " << error.what() << '\n';
		return 1;
	}
}
