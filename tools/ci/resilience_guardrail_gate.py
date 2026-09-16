#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import json
import os
from datetime import datetime, timezone
from pathlib import Path


def load_json(path: Path) -> dict:
	if not path.is_file():
		raise SystemExit(f"required file missing: {path}")
	return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
	parser = argparse.ArgumentParser(description="Validate Sentum resilience and autonomous-recovery guardrails")
	parser.add_argument("--policy", default="config/resilience_policy.json")
	parser.add_argument("--reliability", default="log/reliability_gate.json")
	parser.add_argument("--expected-sha", default=os.environ.get("GITHUB_SHA", "unknown"))
	parser.add_argument("--report", default="log/resilience_guardrail_gate.json")
	parser.add_argument("--summary", default="log/resilience_guardrail_gate.md")
	args = parser.parse_args()

	policy = load_json(Path(args.policy))
	reliability = load_json(Path(args.reliability))
	violations: list[str] = []

	expected_environment = str(policy.get("environment_class", "ci_rehearsal"))
	expected_reliability = str(policy.get("required_reliability_status", "PASS"))
	if reliability.get("status") != expected_reliability:
		violations.append("reliability evidence is not PASS")
	if reliability.get("git_sha") != args.expected_sha:
		violations.append("reliability evidence does not match expected Git SHA")
	if reliability.get("environment_class") != expected_environment:
		violations.append("reliability evidence has unexpected environment class")

	breaker = policy.get("circuit_breaker", {})
	trip_on = set(breaker.get("trip_on_states", []))
	degrade_on = set(breaker.get("degrade_on_states", []))
	reset_samples = int(breaker.get("reset_requires_ready_samples", 0))
	if "BLOCKED" not in trip_on:
		violations.append("circuit breaker must trip on BLOCKED")
	if "ATTENTION" not in degrade_on:
		violations.append("degradation policy must include ATTENTION")
	if reset_samples < 1:
		violations.append("reset_requires_ready_samples must be positive")

	autonomous = set(policy.get("autonomous_actions", []))
	approval = set(policy.get("operator_approval_actions", []))
	forbidden = set(policy.get("forbidden_autonomous_actions", []))
	if not autonomous:
		violations.append("autonomous_actions must not be empty")
	if autonomous & forbidden:
		violations.append("forbidden action present in autonomous_actions")
	if "clear_kill_switch" not in approval:
		violations.append("clearing kill switch must require operator approval")
	if "resume_entries" not in approval:
		violations.append("resuming entries must require operator approval")

	required_forbidden = {
		"clear_kill_switch",
		"enable_entries",
		"synthesize_fill",
		"mutate_risk_state",
		"mutate_execution_state",
		"override_exchange_confirmed_truth",
	}
	missing_forbidden = sorted(required_forbidden - forbidden)
	if missing_forbidden:
		violations.append("missing forbidden autonomous actions: " + ", ".join(missing_forbidden))

	scenarios = [
		("ready", "READY", False, "observe"),
		("attention", "ATTENTION", False, "degrade"),
		("blocked", "BLOCKED", True, "trip"),
	]
	scenario_results = []
	for name, reliability_state, expected_trip, expected_action in scenarios:
		trip = reliability_state in trip_on
		if trip:
			action = "trip"
		elif reliability_state in degrade_on:
			action = "degrade"
		else:
			action = "observe"
		scenario_results.append({
			"name": name,
			"reliability_state": reliability_state,
			"expected_trip": expected_trip,
			"actual_trip": trip,
			"expected_action": expected_action,
			"actual_action": action,
		})
		if trip != expected_trip or action != expected_action:
			violations.append(f"{name}: circuit-breaker action mismatch")

	reset_sequence = ["READY"] * reset_samples
	reset_allowed = len(reset_sequence) >= reset_samples and all(state == "READY" for state in reset_sequence)
	mixed_reset_sequence = ["READY"] * max(0, reset_samples - 1) + ["ATTENTION"]
	mixed_reset_allowed = len(mixed_reset_sequence) >= reset_samples and all(state == "READY" for state in mixed_reset_sequence)
	if not reset_allowed:
		violations.append("healthy reset sequence did not satisfy circuit-breaker reset policy")
	if mixed_reset_allowed:
		violations.append("degraded reset sequence incorrectly satisfied circuit-breaker reset policy")

	report = {
		"schema_version": 1,
		"generated_at": datetime.now(timezone.utc).isoformat(),
		"git_sha": args.expected_sha,
		"workflow_run_id": os.environ.get("SOURCE_WORKFLOW_RUN_ID", os.environ.get("GITHUB_RUN_ID", "unknown")),
		"environment_class": expected_environment,
		"status": "PASS" if not violations else "FAIL",
		"upstream": {
			"reliability_status": reliability.get("status"),
			"reliability_git_sha": reliability.get("git_sha"),
			"reliability_environment_class": reliability.get("environment_class"),
		},
		"circuit_breaker": {
			"trip_on_states": sorted(trip_on),
			"degrade_on_states": sorted(degrade_on),
			"reset_requires_ready_samples": reset_samples,
			"reset_sequence_status": "PASS" if reset_allowed else "FAIL",
			"mixed_reset_sequence_status": "PASS" if not mixed_reset_allowed else "FAIL",
		},
		"autonomous_actions": sorted(autonomous),
		"operator_approval_actions": sorted(approval),
		"forbidden_autonomous_actions": sorted(forbidden),
		"scenarios": scenario_results,
		"violations": violations,
	}

	report_path = Path(args.report)
	summary_path = Path(args.summary)
	report_path.parent.mkdir(parents=True, exist_ok=True)
	summary_path.parent.mkdir(parents=True, exist_ok=True)
	report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

	lines = [
		"# Resilience guardrail gate",
		"",
		f"Status: **{report['status']}**",
		f"Environment: `{report['environment_class']}`",
		f"Git SHA: `{report['git_sha']}`",
		"",
		"| Scenario | Expected action | Actual action |",
		"| --- | --- | --- |",
	]
	for item in scenario_results:
		lines.append(f"| {item['name']} | {item['expected_action']} | {item['actual_action']} |")
	if violations:
		lines.extend(["", "## Violations"])
		lines.extend(f"- {item}" for item in violations)
	summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
	print(summary_path.read_text(encoding="utf-8"), end="")
	return 0 if not violations else 1


if __name__ == "__main__":
	raise SystemExit(main())
