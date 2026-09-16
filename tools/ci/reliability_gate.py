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
    parser = argparse.ArgumentParser(description="Validate Sentum reliability policy and error-budget state")
    parser.add_argument("--policy", default="config/reliability_policy.json")
    parser.add_argument("--continuous-readiness", default="log/continuous_readiness_rehearsal.json")
    parser.add_argument("--expected-sha", default=os.environ.get("GITHUB_SHA", "unknown"))
    parser.add_argument("--report", default="log/reliability_gate.json")
    parser.add_argument("--summary", default="log/reliability_gate.md")
    args = parser.parse_args()

    policy = load_json(Path(args.policy))
    readiness = load_json(Path(args.continuous_readiness))
    violations: list[str] = []

    target = float(policy.get("availability_target", 0.0))
    window_minutes = float(policy.get("window_minutes", 0.0))
    if target <= 0.0 or target > 1.0:
        violations.append("availability_target must be in (0, 1]")
    if window_minutes <= 0.0:
        violations.append("window_minutes must be positive")

    allowed_unavailable_minutes = window_minutes * max(0.0, 1.0 - target)
    warning_fraction = float(policy.get("warning_budget_consumed_fraction", 0.5))
    critical_fraction = float(policy.get("critical_budget_consumed_fraction", 0.8))
    block_fraction = float(policy.get("block_budget_consumed_fraction", 1.0))
    if not (0.0 <= warning_fraction <= critical_fraction <= block_fraction):
        violations.append("budget thresholds must be monotonically increasing")

    expected_status = str(policy.get("required_continuous_readiness_status", "PASS"))
    expected_environment = str(policy.get("required_environment_class", "ci_rehearsal"))
    if readiness.get("status") != expected_status:
        violations.append("continuous readiness evidence is not PASS")
    if readiness.get("git_sha") != args.expected_sha:
        violations.append("continuous readiness evidence does not match expected Git SHA")
    if readiness.get("environment_class") != expected_environment:
        violations.append("continuous readiness evidence has unexpected environment class")
    if readiness.get("observation_window", {}).get("status") != "PASS":
        violations.append("continuous readiness observation window is not PASS")

    scenarios = [
        ("healthy_budget", 0.0, "READY"),
        ("warning_budget", allowed_unavailable_minutes * warning_fraction, "ATTENTION"),
        ("critical_budget", allowed_unavailable_minutes * critical_fraction, "ATTENTION"),
        ("exhausted_budget", allowed_unavailable_minutes * block_fraction, "BLOCKED"),
    ]

    scenario_results = []
    for name, consumed, expected_state in scenarios:
        if allowed_unavailable_minutes <= 0.0:
            fraction = 1.0 if consumed > 0.0 else 0.0
        else:
            fraction = consumed / allowed_unavailable_minutes
        if fraction >= block_fraction:
            state = "BLOCKED"
        elif fraction >= warning_fraction:
            state = "ATTENTION"
        else:
            state = "READY"
        scenario_results.append({
            "name": name,
            "consumed_unavailable_minutes": consumed,
            "budget_consumed_fraction": fraction,
            "expected": expected_state,
            "actual": state,
        })
        if state != expected_state:
            violations.append(f"{name}: expected {expected_state}, got {state}")

    report = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "git_sha": args.expected_sha,
        "workflow_run_id": os.environ.get("SOURCE_WORKFLOW_RUN_ID", os.environ.get("GITHUB_RUN_ID", "unknown")),
        "environment_class": "ci_rehearsal",
        "status": "PASS" if not violations else "FAIL",
        "policy": {
            "window_minutes": window_minutes,
            "availability_target": target,
            "allowed_unavailable_minutes": allowed_unavailable_minutes,
            "warning_budget_consumed_fraction": warning_fraction,
            "critical_budget_consumed_fraction": critical_fraction,
            "block_budget_consumed_fraction": block_fraction,
        },
        "upstream": {
            "continuous_readiness_status": readiness.get("status"),
            "continuous_readiness_git_sha": readiness.get("git_sha"),
            "continuous_readiness_environment_class": readiness.get("environment_class"),
        },
        "scenarios": scenario_results,
        "violations": violations,
    }

    report_path = Path(args.report)
    summary_path = Path(args.summary)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    lines = [
        "# Reliability gate",
        "",
        f"Status: **{report['status']}**",
        f"Environment: `{report['environment_class']}`",
        f"Git SHA: `{report['git_sha']}`",
        f"Availability policy target: `{target:.5f}` over `{window_minutes:.0f}` minutes",
        f"Error budget: `{allowed_unavailable_minutes:.3f}` unavailable minutes",
        "",
        "| Scenario | Expected | Actual | Budget consumed |",
        "| --- | --- | --- | ---: |",
    ]
    for item in scenario_results:
        lines.append(
            f"| {item['name']} | {item['expected']} | {item['actual']} | {item['budget_consumed_fraction']:.3f} |"
        )
    if violations:
        lines.extend(["", "## Violations"])
        lines.extend(f"- {item}" for item in violations)
    summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(summary_path.read_text(encoding="utf-8"), end="")
    return 0 if not violations else 1


if __name__ == "__main__":
    raise SystemExit(main())
