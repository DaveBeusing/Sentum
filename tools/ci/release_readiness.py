#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


class ReadinessFailure(RuntimeError):
	pass


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


def sha256(path: Path) -> str:
	digest = hashlib.sha256()
	with path.open("rb") as stream:
		for chunk in iter(lambda: stream.read(1024 * 1024), b""):
			digest.update(chunk)
	return digest.hexdigest()


def check(name: str, condition: bool, detail: str) -> dict[str, str]:
	return {"name": name, "status": "PASS" if condition else "FAIL", "detail": detail}


def input_record(
	name: str,
	path: Path,
	report: dict[str, Any],
	git_sha: Any,
	environment_class: Any,
) -> dict[str, Any]:
	return {
		"name": name,
		"path": str(path),
		"sha256": sha256(path),
		"schema_version": report.get("schema_version"),
		"status": report.get("status"),
		"git_sha": git_sha,
		"environment_class": environment_class,
	}


def main() -> int:
	args = parse_args()
	checks: list[dict[str, str]] = []
	try:
		performance_path = Path(args.performance)
		operational_path = Path(args.operational)
		research_path = Path(args.research)
		qualification_paths = [
			Path(path)
			for path in (
				args.qualification
				or [
					"evidence/qualification/paper-soak.json",
					"evidence/qualification/all-faults.json",
				]
			)
		]

		performance = load_report(performance_path, "performance")
		operational = load_report(operational_path, "operational")
		research = load_report(research_path, "research validation")
		qualifications = [
			(path, load_report(path, f"runtime qualification {path.name}"))
			for path in qualification_paths
		]
		expected_sha = str(args.expected_sha)

		checks.append(check("performance-status", performance.get("status") == "PASS", str(performance.get("status"))))
		checks.append(check("operational-status", operational.get("status") == "PASS", str(operational.get("status"))))
		checks.append(check("research-status", research.get("status") == "PASS", str(research.get("status"))))
		checks.append(check("performance-schema", performance.get("schema_version") == 2, str(performance.get("schema_version"))))
		checks.append(check("operational-schema", operational.get("schema_version") == 1, str(operational.get("schema_version"))))
		checks.append(check("research-schema", research.get("schema_version") == 1, str(research.get("schema_version"))))
		checks.append(check("performance-sha", performance.get("git_sha") == expected_sha, str(performance.get("git_sha"))))
		checks.append(check("operational-sha", operational.get("git_sha") == expected_sha, str(operational.get("git_sha"))))
		research_sha = research.get("git_sha") or research.get("code_identity", {}).get("git_commit")
		checks.append(check("research-sha", research_sha == expected_sha, str(research_sha)))
		checks.append(check(
			"operational-cycles",
			operational.get("cycles_completed") == operational.get("cycles_requested")
			and int(operational.get("cycles_requested", 0)) > 0,
			f"{operational.get('cycles_completed')}/{operational.get('cycles_requested')}",
		))
		checks.append(check(
			"research-reproduction",
			research.get("reproduction", {}).get("status") == "PASS",
			str(research.get("reproduction", {}).get("status")),
		))

		for path, qualification in qualifications:
			label = str(qualification.get("scenario") or path.stem)
			checks.append(check(
				f"qualification-{label}-schema",
				qualification.get("schema_version") == 1,
				str(qualification.get("schema_version")),
			))
			checks.append(check(
				f"qualification-{label}-status",
				qualification.get("status") == "PASS" and qualification.get("evidence_complete") is True,
				f"status={qualification.get('status')} complete={qualification.get('evidence_complete')}",
			))
			checks.append(check(
				f"qualification-{label}-sha",
				qualification.get("git_sha") == expected_sha,
				str(qualification.get("git_sha")),
			))

		status = "PASS" if all(item["status"] == "PASS" for item in checks) else "FAIL"
		inputs = [
			input_record(
				"performance",
				performance_path,
				performance,
				performance.get("git_sha"),
				performance.get("environment_class"),
			),
			input_record(
				"operational_acceptance",
				operational_path,
				operational,
				operational.get("git_sha"),
				operational.get("environment_class"),
			),
			input_record(
				"independent_research_validation",
				research_path,
				research,
				research_sha,
				research.get("environment_class"),
			),
		]
		for path, qualification in qualifications:
			inputs.append(
				input_record(
					f"runtime_qualification:{qualification.get('scenario', path.stem)}",
					path,
					qualification,
					qualification.get("git_sha"),
					qualification.get("environment_class"),
				)
			)

		payload = {
			"schema_version": 2,
			"generated_at": datetime.now(timezone.utc).isoformat(),
			"environment_class": "ci_release_gate",
			"status": status,
			"git_sha": expected_sha,
			"workflow_run_id": os.environ.get("GITHUB_RUN_ID", "unknown"),
			"inputs": inputs,
			"checks": checks,
			"limitations": [
				"PASS is repository release-readiness evidence for the exact commit only.",
				"PASS does not prove target-environment production acceptance, exchange availability or profitability.",
			],
		}
		Path(args.report).parent.mkdir(parents=True, exist_ok=True)
		Path(args.report).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
		lines = [
			"# Release readiness",
			"",
			f"Status: **{status}**",
			"",
			"| Gate | Status | Detail |",
			"| --- | --- | --- |",
		]
		for item in checks:
			lines.append(f"| {item['name']} | {item['status']} | {item['detail']} |")
		lines.extend([
			"",
			"PASS is repository evidence only and is not target-environment production acceptance.",
			"",
		])
		Path(args.summary).write_text("\n".join(lines), encoding="utf-8")
		print(Path(args.summary).read_text(encoding="utf-8"), end="")
		return 0 if status == "PASS" else 1
	except (ReadinessFailure, TypeError, ValueError) as error:
		print(f"release readiness failure: {error}", file=sys.stderr)
		return 2


if __name__ == "__main__":
	raise SystemExit(main())
