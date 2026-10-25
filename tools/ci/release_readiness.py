#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import json
import os
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from release_evidence_contract import (
    CONTRACT_VERSION, INPUT_CONTRACTS, MAX_INPUT_AGE_HOURS, REQUIRED_SCENARIOS,
    input_checks, input_record, load_json_object, oldest_evidence_at,
    valid_run_id, valid_sha, validate_readiness,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate Sentum release-readiness evidence.")
    parser.add_argument("--performance", default="evidence/performance/performance_gate.json")
    parser.add_argument("--operational", default="evidence/operational/operational_acceptance.json")
    parser.add_argument("--research", default="evidence/research/research_validation.json")
    parser.add_argument("--qualification", action="append", default=None)
    parser.add_argument("--report", default="log/release_readiness.json")
    parser.add_argument("--summary", default="log/release_readiness.md")
    parser.add_argument("--expected-sha", default=os.environ.get("GITHUB_SHA", "unknown"))
    return parser.parse_args()


def check(name: str, condition: bool, detail: str) -> dict[str, str]:
    return {"name": name, "status": "PASS" if condition else "FAIL", "detail": detail}


def write_evidence(args: argparse.Namespace, payload: dict[str, Any]) -> None:
    report_path, summary_path = Path(args.report), Path(args.summary)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(payload, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")
    lines = ["# Release readiness", "", f"Status: **{payload['status']}**", "",
             "| Gate | Status | Detail |", "| --- | --- | --- |"]
    lines.extend(f"| {item['name']} | {item['status']} | {item['detail']} |" for item in payload["checks"])
    if payload["violations"]:
        lines.extend(["", "## Blocking evidence problems", ""])
        lines.extend(f"- {item}" for item in payload["violations"])
    lines.extend(["", "PASS is repository evidence only and is not target-environment production acceptance.", ""])
    summary_path.write_text("\n".join(lines), encoding="utf-8")
    print(summary_path.read_text(encoding="utf-8"), end="")


def main() -> int:
    args = parse_args()
    now = datetime.now(timezone.utc)
    expected_sha = str(args.expected_sha)
    run_id = os.environ.get("GITHUB_RUN_ID", "unknown")
    checks = [check("commit-identity", valid_sha(expected_sha), expected_sha),
              check("workflow-run-identity", valid_run_id(run_id), run_id)]
    violations: list[str] = []
    inputs: list[dict[str, Any]] = []
    sources = [("performance", Path(args.performance)),
               ("operational_acceptance", Path(args.operational)),
               ("independent_research_validation", Path(args.research))]
    qualifications = args.qualification or [f"evidence/qualification/{name}.json" for name in REQUIRED_SCENARIOS]
    scenario_names: list[Any] = []
    for raw_path in qualifications:
        path = Path(raw_path)
        try:
            report = load_json_object(path)
            scenario = report.get("scenario")
            scenario_names.append(scenario)
            if not isinstance(scenario, str) or scenario not in REQUIRED_SCENARIOS:
                violations.append(f"{path}: unknown or missing qualification scenario {scenario!r}")
                continue
            name = f"runtime_qualification:{scenario}"
            record = input_record(name, path, report)
            inputs.append(record)
            checks.extend(input_checks(record, expected_sha, run_id, now))
        except (OSError, ValueError, TypeError) as error:
            violations.append(f"{path}: invalid qualification evidence: {error}")
    complete_scenarios = (len(scenario_names) == len(REQUIRED_SCENARIOS)
                          and all(isinstance(name, str) for name in scenario_names)
                          and set(scenario_names) == set(REQUIRED_SCENARIOS))
    checks.append(check("qualification-scenarios", complete_scenarios,
                        f"expected exactly {list(REQUIRED_SCENARIOS)}, got {scenario_names}"))
    for name, path in sources:
        try:
            report = load_json_object(path)
            record = input_record(name, path, report)
            inputs.append(record)
            checks.extend(input_checks(record, expected_sha, run_id, now))
        except (OSError, ValueError, TypeError) as error:
            violations.append(f"{name}: invalid evidence: {error}")
    # Stable logical ordering is independent of filenames and argument order.
    inputs.sort(key=lambda item: list(INPUT_CONTRACTS).index(item["name"]))
    payload = {
        "schema_version": 2,
        "contract_version": CONTRACT_VERSION,
        "generated_at": now.isoformat(),
        "environment_class": "ci_release_gate",
        "status": "PASS",
        "git_sha": expected_sha,
        "workflow_run_id": run_id,
        "max_input_age_hours": MAX_INPUT_AGE_HOURS,
        "oldest_evidence_at": oldest_evidence_at(inputs),
        "inputs": inputs,
        "checks": checks,
        "violations": list(violations),
        "limitations": [
            "PASS is repository release-readiness evidence for the exact commit only.",
            "Local unknown workflow identity cannot satisfy a CI-bound source-run handoff.",
            "PASS does not prove target-environment production acceptance, exchange availability or profitability.",
        ],
    }
    violations.extend(validate_readiness(payload, expected_sha, now, run_id))
    payload["violations"] = list(dict.fromkeys(violations))
    payload["status"] = "FAIL" if violations else "PASS"
    write_evidence(args, payload)
    return 0 if payload["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
