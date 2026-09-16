#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import json
import os
from datetime import datetime, timezone
from pathlib import Path


def classify(snapshot: dict) -> tuple[str, str]:
	health = str(snapshot.get("health", "starting"))
	kill_switch = bool(snapshot.get("kill_switch_active", False))
	market_connected = bool(snapshot.get("market_data_connected", False))
	pressure = str(snapshot.get("performance", {}).get("queue_pressure", "normal"))

	if health in {"stopped", "stopping"}:
		return "BLOCKED", f"runtime {health}"
	if kill_switch:
		return "BLOCKED", "kill switch active"
	if health != "healthy":
		return "BLOCKED", f"runtime health {health}"
	if not market_connected:
		return "ATTENTION", "market data disconnected"
	if pressure in {"critical", "saturated"}:
		return "ATTENTION", f"persistence pressure {pressure}"
	return "READY", "runtime operational"


def main() -> int:
	parser = argparse.ArgumentParser(description="Exercise Sentum continuous-readiness monitoring policy")
	parser.add_argument("--report", default="log/continuous_readiness_rehearsal.json")
	parser.add_argument("--summary", default="log/continuous_readiness_rehearsal.md")
	args = parser.parse_args()

	scenarios = [
		("healthy", {"health": "healthy", "kill_switch_active": False, "market_data_connected": True, "performance": {"queue_pressure": "normal"}}, "READY"),
		("market_disconnect", {"health": "healthy", "kill_switch_active": False, "market_data_connected": False, "performance": {"queue_pressure": "normal"}}, "ATTENTION"),
		("persistence_critical", {"health": "healthy", "kill_switch_active": False, "market_data_connected": True, "performance": {"queue_pressure": "critical"}}, "ATTENTION"),
		("persistence_saturated", {"health": "healthy", "kill_switch_active": False, "market_data_connected": True, "performance": {"queue_pressure": "saturated"}}, "ATTENTION"),
		("kill_switch", {"health": "healthy", "kill_switch_active": True, "market_data_connected": True, "performance": {"queue_pressure": "normal"}}, "BLOCKED"),
		("runtime_unhealthy", {"health": "degraded", "kill_switch_active": False, "market_data_connected": True, "performance": {"queue_pressure": "normal"}}, "BLOCKED"),
	]

	results = []
	violations = []
	for name, snapshot, expected in scenarios:
		state, reason = classify(snapshot)
		results.append({"name": name, "expected": expected, "actual": state, "reason": reason, "snapshot": snapshot})
		if state != expected:
			violations.append(f"{name}: expected {expected}, got {state}")

	observation_window = [scenarios[0][1] for _ in range(5)]
	window_states = [classify(sample)[0] for sample in observation_window]
	window_pass = all(state == "READY" for state in window_states)
	if not window_pass:
		violations.append("healthy observation window did not remain READY")

	degraded_window = [scenarios[0][1], scenarios[1][1], scenarios[0][1]]
	degraded_states = [classify(sample)[0] for sample in degraded_window]
	if all(state == "READY" for state in degraded_states):
		violations.append("degraded observation window incorrectly remained READY")

	report = {
		"schema_version": 1,
		"generated_at": datetime.now(timezone.utc).isoformat(),
		"git_sha": os.environ.get("GITHUB_SHA", "unknown"),
		"workflow_run_id": os.environ.get("SOURCE_WORKFLOW_RUN_ID", os.environ.get("GITHUB_RUN_ID", "unknown")),
		"environment_class": "ci_rehearsal",
		"status": "PASS" if not violations else "FAIL",
		"scenarios": results,
		"observation_window": {"samples": len(observation_window), "states": window_states, "status": "PASS" if window_pass else "FAIL"},
		"degraded_window": {"states": degraded_states, "status": "PASS" if not all(state == "READY" for state in degraded_states) else "FAIL"},
		"violations": violations,
	}

	report_path = Path(args.report)
	summary_path = Path(args.summary)
	report_path.parent.mkdir(parents=True, exist_ok=True)
	summary_path.parent.mkdir(parents=True, exist_ok=True)
	report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

	lines = [
		"# Continuous readiness rehearsal",
		"",
		f"Status: **{report['status']}**",
		f"Environment: `{report['environment_class']}`",
		f"Git SHA: `{report['git_sha']}`",
		"",
		"| Scenario | Expected | Actual |",
		"| --- | --- | --- |",
	]
	for item in results:
		lines.append(f"| {item['name']} | {item['expected']} | {item['actual']} |")
	if violations:
		lines.extend(["", "## Violations"])
		lines.extend(f"- {item}" for item in violations)
	summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
	print(summary_path.read_text(encoding="utf-8"), end="")
	return 0 if not violations else 1


if __name__ == "__main__":
	raise SystemExit(main())
