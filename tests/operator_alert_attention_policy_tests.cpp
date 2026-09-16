#include <sentum/ui/OperatorAlertAttentionPolicy.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

sentum::ui::OperatorAlertLifecycleItem make_item(
	std::string id,
	sentum::ui::OperatorAlertSeverity severity,
	sentum::ui::OperatorAlertLifecycleState state = sentum::ui::OperatorAlertLifecycleState::Active,
	std::size_t generation = 1) {
	sentum::ui::OperatorAlertLifecycleItem item;
	item.alert.id = std::move(id);
	item.alert.severity = severity;
	item.state = state;
	item.generation = generation;
	item.active = state != sentum::ui::OperatorAlertLifecycleState::Cleared;
	return item;
}

void test_high_severity_is_never_suppressed() {
	using namespace sentum::ui;
	std::vector<OperatorAlertLifecycleItem> items;
	items.push_back(make_item("critical", OperatorAlertSeverity::Critical));
	items.push_back(make_item("warning", OperatorAlertSeverity::Warning, OperatorAlertLifecycleState::Acknowledged));
	const auto view = derive_operator_alert_attention_view(items, 0);
	require(view.prominent == 2, "critical/warning alerts must stay prominent");
	require(view.suppressed == 0, "high severity alert was suppressed");
	require(!view.decisions[0].suppressed && !view.decisions[1].suppressed, "high severity decision suppressed");
}

void test_low_severity_budget_limits_storm_noise() {
	using namespace sentum::ui;
	std::vector<OperatorAlertLifecycleItem> items;
	for (int index = 0; index < 6; ++index) {
		items.push_back(make_item("attention-" + std::to_string(index), OperatorAlertSeverity::Attention));
	}
	const auto view = derive_operator_alert_attention_view(items, 2);
	require(view.visible == 2, "low-severity budget was not enforced");
	require(view.suppressed == 4, "overflow alerts were not suppressed");
	require(view.storm_limited, "storm limiting was not reported");
}

void test_acknowledged_and_cleared_low_severity_are_deprioritized() {
	using namespace sentum::ui;
	std::vector<OperatorAlertLifecycleItem> items;
	items.push_back(make_item("ack", OperatorAlertSeverity::Attention, OperatorAlertLifecycleState::Acknowledged));
	items.push_back(make_item("cleared", OperatorAlertSeverity::Info, OperatorAlertLifecycleState::Cleared));
	const auto view = derive_operator_alert_attention_view(items, 4);
	require(view.suppressed == 2, "acknowledged/cleared low-severity alerts should be suppressed");
	require(view.decisions[0].suppressed && view.decisions[1].suppressed, "low-severity suppression missing");
}

void test_flapping_low_severity_remains_visible() {
	using namespace sentum::ui;
	std::vector<OperatorAlertLifecycleItem> items;
	items.push_back(make_item("flapping", OperatorAlertSeverity::Attention, OperatorAlertLifecycleState::Active, 3));
	const auto view = derive_operator_alert_attention_view(items, 0);
	require(view.flapping == 1, "flapping alert was not detected");
	require(view.visible == 1, "flapping low-severity alert should remain visible");
	require(!view.decisions[0].suppressed, "flapping alert was suppressed");
}

void test_attention_policy_never_grants_execution_authority() {
	using namespace sentum::ui;
	std::vector<OperatorAlertLifecycleItem> items;
	items.push_back(make_item("critical", OperatorAlertSeverity::Critical));
	const auto view = derive_operator_alert_attention_view(items, 1);
	require(!view.execution_authorized, "attention policy granted execution authority");
}

} // namespace

int main() {
	try {
		test_high_severity_is_never_suppressed();
		test_low_severity_budget_limits_storm_noise();
		test_acknowledged_and_cleared_low_severity_are_deprioritized();
		test_flapping_low_severity_remains_visible();
		test_attention_policy_never_grants_execution_authority();
		std::cout << "operator alert attention policy tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "operator alert attention policy test failure: " << error.what() << '\n';
		return 1;
	}
}
