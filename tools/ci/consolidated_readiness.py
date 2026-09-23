#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


class EvidenceFailure(RuntimeError):
	pass


def sha256(path: Path) -> str:
	digest = hashlib.sha256()
	with path.open("rb") as stream:
		for chunk in iter(lambda: stream.read(1024 * 1024), b""):
			digest.update(chunk)
	return digest.hexdigest()


def parse_timestamp(value: Any) -> datetime | None:
	if not isinstance(value, str) or not value.strip():
		return None
	try:
		parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
	except ValueError:
		return None
	if parsed.tzinfo is None:
		parsed = parsed.replace(tzinfo=timezone.utc)
	return parsed.astimezone(timezone.utc)


def nested_value(payload: dict[str, Any], path: str) -> Any:
	value: Any = payload
	for part in path.split("."):
		if not isinstance(value, dict) or part not in value:
			return None
		value = value[part]
	return value


def load_json(path: Path, label: str) -> dict[str, Any]:
	if not path.is_file():
		raise EvidenceFailure(f"missing {label} evidence: {path}")
	try:
		payload = json.loads(path.read_text(encoding="utf-8"))
	except (OSError, json.JSONDecodeError) as error:
		raise EvidenceFailure(f"invalid {label} evidence: {error}") from error
	if not isinstance(payload, dict):
		raise EvidenceFailure(f"{label} evidence root must be an object")
	return payload


def evidence_age_hours(
	payload: dict[str, Any],
	contract: dict[str, Any],
	now: datetime,
) -> tuple[float | None, str | None]:
	for field in contract.get("timestamp_fields", []):
		stamp = parse_timestamp(nested_value(payload, str(field)))
		if stamp is not None:
			return max(0.0, (now - stamp).total_seconds() / 3600.0), str(field)
	return None, None


def validate_evidence(
	name: str,
	path: Path,
	contract: dict[str, Any],
	expected_sha: str,
	now: datetime,
) -> dict[str, Any]:
	payload = load_json(path, name)
	violations: list[str] = []

	expected_schema = contract.get("schema_version")
	if expected_schema is not None and payload.get("schema_version") != expected_schema:
		violations.append(
			f"schema_version expected {expected_schema}, got {payload.get('schema_version')}"
		)

	status_field = str(contract.get("status_field", "status"))
	status = nested_value(payload, status_field)
	accepted = {str(value) for value in contract.get("accepted_statuses", ["PASS"])}
	if str(status) not in accepted:
		violations.append(f"{status_field} expected one of {sorted(accepted)}, got {status}")

	sha_field = str(contract.get("git_sha_field", "git_sha"))
	actual_sha = nested_value(payload, sha_field)
	if actual_sha != expected_sha:
		violations.append(f"{sha_field} does not match expected Git SHA")

	environment_field = contract.get("environment_field")
	allowed_environments = contract.get("allowed_environment_classes", [])
	environment = nested_value(payload, str(environment_field)) if environment_field else None
	if allowed_environments and environment not in allowed_environments:
		violations.append(
			f"environment class expected one of {allowed_environments}, got {environment}"
		)

	for field in contract.get("required_fields", []):
		value = nested_value(payload, str(field))
		if value is None or value == "":
			violations.append(f"required field missing: {field}")

	age_hours, timestamp_field = evidence_age_hours(payload, contract, now)
	max_age_hours = contract.get("max_age_hours")
	if max_age_hours is not None:
		if age_hours is None:
			violations.append("no supported evidence timestamp is present")
		elif age_hours > float(max_age_hours):
			violations.append(
				f"evidence is stale: age={age_hours:.2f}h limit={float(max_age_hours):.2f}h"
			)

	return {
		"name": name,
		"path": str(path),
		"sha256": sha256(path),
		"schema_version": payload.get("schema_version"),
		"status": status,
		"git_sha": actual_sha,
		"environment_class": environment,
		"timestamp_field": timestamp_field,
		"age_hours": age_hours,
		"workflow_run_id": payload.get("workflow_run_id"),
		"result": "PASS" if not violations else "FAIL",
		"violations": violations,
	}


def evaluate(
	policy: dict[str, Any],
	evidence_paths: dict[str, Path],
	expected_sha: str,
	now: datetime,
	target_acceptance_path: Path | None = None,
) -> dict[str, Any]:
	if policy.get("schema_version") != 1:
		raise EvidenceFailure("unsupported readiness evidence policy schema")

	required_contracts = policy.get("required_repository_evidence")
	if not isinstance(required_contracts, dict) or not required_contracts:
		raise EvidenceFailure("readiness evidence policy has no required repository evidence")

	results: list[dict[str, Any]] = []
	violations: list[str] = []
	for name, contract in required_contracts.items():
		path = evidence_paths.get(name)
		if path is None:
			results.append({
				"name": name,
				"result": "FAIL",
				"violations": ["required evidence was not supplied"],
			})
			violations.append(f"{name}: required evidence was not supplied")
			continue
		try:
			result = validate_evidence(name, path, contract, expected_sha, now)
		except EvidenceFailure as error:
			result = {
				"name": name,
				"path": str(path),
				"result": "FAIL",
				"violations": [str(error)],
			}
		results.append(result)
		violations.extend(f"{name}: {item}" for item in result.get("violations", []))

	target_result: dict[str, Any] = {
		"state": "NOT_PROVIDED",
		"result": "NOT_CHECKED",
		"detail": "Target-environment acceptance is not supplied by repository CI.",
	}
	if target_acceptance_path is not None:
		target_contract = policy.get("target_acceptance")
		if not isinstance(target_contract, dict):
			raise EvidenceFailure("target acceptance contract is missing from policy")
		try:
			checked = validate_evidence(
				"target_acceptance",
				target_acceptance_path,
				target_contract,
				expected_sha,
				now,
			)
		except EvidenceFailure as error:
			checked = {
				"name": "target_acceptance",
				"path": str(target_acceptance_path),
				"result": "FAIL",
				"violations": [str(error)],
			}
		target_result = {
			"state": "ACCEPTED" if checked["result"] == "PASS" else "REJECTED",
			"result": checked["result"],
			"evidence": checked,
		}
		if checked["result"] != "PASS":
			violations.extend(
				f"target_acceptance: {item}" for item in checked.get("violations", [])
			)

	repository_ready = not any(item.get("result") != "PASS" for item in results)
	if not repository_ready or target_result["result"] == "FAIL":
		state = "BLOCKED"
	elif target_result["result"] == "PASS":
		state = "TARGET_ACCEPTED"
	else:
		state = "REPOSITORY_READY"

	return {
		"schema_version": 1,
		"generated_at": now.astimezone(timezone.utc).isoformat(),
		"git_sha": expected_sha,
		"status": state,
		"repository_evidence_status": "PASS" if repository_ready else "FAIL",
		"production_acceptance_status": target_result["state"],
		"evidence": results,
		"target_acceptance": target_result,
		"violations": violations,
		"limitations": [
			"REPOSITORY_READY proves only that repository-defined release and operational rehearsal evidence is complete for the exact commit.",
			"CI rehearsal evidence never satisfies target-environment production acceptance.",
			"TARGET_ACCEPTED requires a separate target-environment acceptance record and does not authorize production-money trading.",
		],
	}


def parse_assignment(value: str) -> tuple[str, Path]:
	if "=" not in value:
		raise argparse.ArgumentTypeError("evidence must use NAME=PATH")
	name, raw_path = value.split("=", 1)
	name = name.strip()
	raw_path = raw_path.strip()
	if not name or not raw_path:
		raise argparse.ArgumentTypeError("evidence must use NAME=PATH")
	return name, Path(raw_path)


def write_summary(path: Path, report: dict[str, Any]) -> None:
	lines = [
		"# Consolidated readiness evidence",
		"",
		f"Status: **{report['status']}**",
		f"Git SHA: `{report['git_sha']}`",
		f"Repository evidence: **{report['repository_evidence_status']}**",
		f"Target-environment acceptance: **{report['production_acceptance_status']}**",
		"",
		"| Evidence | Result | Environment | Age (h) | SHA-256 |",
		"| --- | --- | --- | ---: | --- |",
	]
	for item in report["evidence"]:
		age = item.get("age_hours")
		age_text = f"{age:.2f}" if isinstance(age, (int, float)) else "n/a"
		digest = item.get("sha256", "missing")
		lines.append(
			f"| {item['name']} | {item['result']} | {item.get('environment_class') or 'n/a'} | "
			f"{age_text} | `{digest}` |"
		)
	if report["violations"]:
		lines.extend(["", "## Blocking evidence problems", ""])
		lines.extend(f"- {item}" for item in report["violations"])
	lines.extend([
		"",
		"Repository readiness is not target-environment production acceptance and does not authorize production-money trading.",
		"",
	])
	path.parent.mkdir(parents=True, exist_ok=True)
	path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
	parser = argparse.ArgumentParser(
		description="Consolidate Sentum readiness evidence without widening authority."
	)
	parser.add_argument("--policy", default="config/readiness_evidence_policy.json")
	parser.add_argument("--expected-sha", required=True)
	parser.add_argument("--evidence", action="append", default=[], type=parse_assignment)
	parser.add_argument("--target-acceptance", type=Path, default=None)
	parser.add_argument("--report", default="log/consolidated_readiness.json")
	parser.add_argument("--summary", default="log/consolidated_readiness.md")
	parser.add_argument("--now", default=None, help=argparse.SUPPRESS)
	args = parser.parse_args()

	try:
		policy = load_json(Path(args.policy), "policy")
		evidence_paths = dict(args.evidence)
		now = parse_timestamp(args.now) if args.now else datetime.now(timezone.utc)
		if now is None:
			raise EvidenceFailure("--now must be ISO-8601 when supplied")
		report = evaluate(
			policy,
			evidence_paths,
			args.expected_sha,
			now,
			args.target_acceptance,
		)
		report_path = Path(args.report)
		report_path.parent.mkdir(parents=True, exist_ok=True)
		report_path.write_text(
			json.dumps(report, indent=2, sort_keys=True) + "\n",
			encoding="utf-8",
		)
		write_summary(Path(args.summary), report)
		print(Path(args.summary).read_text(encoding="utf-8"), end="")
		return 0 if report["status"] != "BLOCKED" else 1
	except (EvidenceFailure, OSError, TypeError, ValueError) as error:
		print(f"consolidated readiness failure: {error}", file=sys.stderr)
		return 2


if __name__ == "__main__":
	raise SystemExit(main())
