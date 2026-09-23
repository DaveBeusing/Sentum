#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any


class ReadinessFailure(RuntimeError):
	pass


def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(description="Validate Sentum release-readiness evidence.")
	parser.add_argument("--performance", default="evidence/performance/performance_gate.json")
	parser.add_argument("--operational", default="evidence/operational/operational_acceptance.json")
	parser.add_argument("--report", default="log/release_readiness.json")
	parser.add_argument("--summary", default="log/release_readiness.md")
	parser.add_argument("--expected-sha", default=os.environ.get("GITHUB_SHA", "unknown"))
	return parser.parse_args()


def load_report(path: Path, name: str) -> dict[str, Any]:
	if not path.is_file():
		raise ReadinessFailure(f"missing {name} evidence: {path}")
	try:
		value = json.loads(path.read_text(encoding="utf-8"))
	except (OSError, json.JSONDecodeError) as error:
		raise ReadinessFailure(f"invalid {name} evidence: {error}") from error
	if not isinstance(value, dict):
		raise ReadinessFailure(f"{name} evidence root must be an object")
	return value


def check(name: str, condition: bool, detail: str) -> dict[str, str]:
	return {"name": name, "status": "PASS" if condition else "FAIL", "detail": detail}


def main() -> int:
	args = parse_args()
	checks: list[dict[str, str]] = []
	try:
		performance = load_report(Path(args.performance), "performance")
		operational = load_report(Path(args.operational), "operational")
		expected_sha = str(args.expected_sha)

		checks.append(check("performance-status", performance.get("status") == "PASS", str(performance.get("status"))))
		checks.append(check("operational-status", operational.get("status") == "PASS", str(operational.get("status"))))
		checks.append(check("performance-schema", performance.get("schema_version") == 2, str(performance.get("schema_version"))))
		checks.append(check("operational-schema", operational.get("schema_version") == 1, str(operational.get("schema_version"))))
		checks.append(check("performance-sha", performance.get("git_sha") == expected_sha, str(performance.get("git_sha"))))
		checks.append(check("operational-sha", operational.get("git_sha") == expected_sha, str(operational.get("git_sha"))))
		checks.append(check(
			"operational-cycles",
			operational.get("cycles_completed") == operational.get("cycles_requested") and int(operational.get("cycles_requested", 0)) > 0,
			f"{operational.get('cycles_completed')}/{operational.get('cycles_requested')}",
		))
		status = "PASS" if all(item["status"] == "PASS" for item in checks) else "FAIL"
		payload = {
			"schema_version": 1,
			"status": status,
			"git_sha": expected_sha,
			"workflow_run_id": os.environ.get("GITHUB_RUN_ID", "unknown"),
			"checks": checks,
		}
		Path(args.report).parent.mkdir(parents=True, exist_ok=True)
		Path(args.report).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
		lines = ["# Release readiness", "", f"Status: **{status}**", "", "| Gate | Status | Detail |", "| --- | --- | --- |"]
		for item in checks:
			lines.append(f"| {item['name']} | {item['status']} | {item['detail']} |")
		Path(args.summary).write_text("\n".join(lines) + "\n", encoding="utf-8")
		print(Path(args.summary).read_text(encoding="utf-8"), end="")
		return 0 if status == "PASS" else 1
	except (ReadinessFailure, TypeError, ValueError) as error:
		print(f"release readiness failure: {error}", file=sys.stderr)
		return 2


if __name__ == "__main__":
	raise SystemExit(main())
