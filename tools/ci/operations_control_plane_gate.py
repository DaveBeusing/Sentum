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
	parser = argparse.ArgumentParser(description="Validate Sentum production control-plane governance")
	parser.add_argument("--policy", default="config/operations_governance_policy.json")
	parser.add_argument("--resilience", default="log/resilience_guardrail_gate.json")
	parser.add_argument("--expected-sha", default=os.environ.get("GITHUB_SHA", "unknown"))
	parser.add_argument("--report", default="log/operations_control_plane_gate.json")
	parser.add_argument("--summary", default="log/operations_control_plane_gate.md")
	args = parser.parse_args()

	policy = load_json(Path(args.policy))
	resilience = load_json(Path(args.resilience))
	violations: list[str] = []

	expected_environment = str(policy.get("environment_class", "ci_rehearsal"))
	if resilience.get("status") != policy.get("required_resilience_status", "PASS"):
		violations.append("resilience evidence is not PASS")
	if resilience.get("git_sha") != args.expected_sha:
		violations.append("resilience evidence does not match expected Git SHA")
	if resilience.get("environment_class") != expected_environment:
		violations.append("resilience evidence has unexpected environment class")

	automated = set(policy.get("automated_actions", []))
	approval = set(policy.get("approval_required_actions", []))
	forbidden = set(policy.get("forbidden_actions", []))
	if not automated or not approval or not forbidden:
		violations.append("all governance action classes must be non-empty")
	if automated & approval or automated & forbidden or approval & forbidden:
		violations.append("governance action classes must be disjoint")

	required_protected = {"resume_entries", "clear_kill_switch", "accept_reconciliation"}
	missing_protected = sorted(required_protected - approval)
	if missing_protected:
		violations.append("missing approval-required actions: " + ", ".join(missing_protected))

	required_forbidden = {
		"synthesize_fill",
		"mutate_risk_state",
		"mutate_execution_state",
		"override_exchange_confirmed_truth",
	}
	missing_forbidden = sorted(required_forbidden - forbidden)
	if missing_forbidden:
		violations.append("missing forbidden actions: " + ", ".join(missing_forbidden))

	required_audit_fields = list(policy.get("required_audit_fields", []))
	required_audit_set = set(required_audit_fields)
	for field in {"action", "classification", "actor", "reason", "git_sha", "timestamp_utc"}:
		if field not in required_audit_set:
			violations.append(f"missing required audit field: {field}")

	def classify(action: str) -> str:
		if action in forbidden:
			return "FORBIDDEN"
		if action in approval:
			return "APPROVAL_REQUIRED"
		if action in automated:
			return "AUTOMATED"
		return "UNCLASSIFIED"

	scenarios = [
		("collect_diagnostics", "AUTOMATED"),
		("resume_entries", "APPROVAL_REQUIRED"),
		("clear_kill_switch", "APPROVAL_REQUIRED"),
		("synthesize_fill", "FORBIDDEN"),
		("mutate_execution_state", "FORBIDDEN"),
	]
	scenario_results = []
	for action, expected in scenarios:
		actual = classify(action)
		scenario_results.append({"action": action, "expected": expected, "actual": actual})
		if actual != expected:
			violations.append(f"{action}: expected {expected}, got {actual}")

	governance_state = "BLOCKED" if violations else "CONTROLLED"
	report = {
		"schema_version": 1,
		"generated_at": datetime.now(timezone.utc).isoformat(),
		"git_sha": args.expected_sha,
		"workflow_run_id": os.environ.get("SOURCE_WORKFLOW_RUN_ID", os.environ.get("GITHUB_RUN_ID", "unknown")),
		"environment_class": expected_environment,
		"status": "PASS" if not violations else "FAIL",
		"governance_state": governance_state,
		"upstream": {
			"resilience_status": resilience.get("status"),
			"resilience_git_sha": resilience.get("git_sha"),
			"resilience_environment_class": resilience.get("environment_class"),
		},
		"action_classes": {
			"automated": sorted(automated),
			"approval_required": sorted(approval),
			"forbidden": sorted(forbidden),
		},
		"required_audit_fields": required_audit_fields,
		"scenarios": scenario_results,
		"violations": violations,
	}

	report_path = Path(args.report)
	summary_path = Path(args.summary)
	report_path.parent.mkdir(parents=True, exist_ok=True)
	summary_path.parent.mkdir(parents=True, exist_ok=True)
	report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

	lines = [
		"# Operations control-plane governance gate",
		"",
		f"Status: **{report['status']}**",
		f"Governance state: **{governance_state}**",
		f"Environment: `{expected_environment}`",
		f"Git SHA: `{args.expected_sha}`",
		"",
		"| Action | Expected | Actual |",
		"| --- | --- | --- |",
	]
	for item in scenario_results:
		lines.append(f"| {item['action']} | {item['expected']} | {item['actual']} |")
	if violations:
		lines.extend(["", "## Violations"])
		lines.extend(f"- {item}" for item in violations)
	summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
	print(summary_path.read_text(encoding="utf-8"), end="")
	return 0 if not violations else 1


if __name__ == "__main__":
	raise SystemExit(main())
