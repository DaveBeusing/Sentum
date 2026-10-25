# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

"""Shared, fail-closed release evidence contracts used at every handoff."""

from __future__ import annotations

import hashlib
import json
import math
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


CONTRACT_VERSION = 1
MAX_INPUT_AGE_HOURS = 336
MAX_RC_AGE_HOURS = 720
REQUIRED_SCENARIOS = ("paper-soak", "all-faults")
INPUT_CONTRACTS = {
    "performance": (2, "ci_hosted_runner", ("generated_at",)),
    "operational_acceptance": (1, "ci_rehearsal", ("generated_at",)),
    "independent_research_validation": (1, "research_validation", ("validated_at_utc",)),
    **{
        f"runtime_qualification:{scenario}":
            (1, "ci_rehearsal", ("started_at_utc", "ended_at_utc"))
        for scenario in REQUIRED_SCENARIOS
    },
}
INPUT_CHECKS = ("schema", "status", "sha", "environment", "run", "timestamps", "semantics")
REQUIRED_CHECKS = {"commit-identity", "workflow-run-identity", "qualification-scenarios"} | {
    f"{name}-{field}" for name in INPUT_CONTRACTS for field in INPUT_CHECKS
}


def parse_timestamp(value: Any) -> datetime | None:
    if not isinstance(value, str) or not value.strip():
        return None
    try:
        stamp = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if stamp.tzinfo is None or stamp.utcoffset() is None:
            return None
        return stamp.astimezone(timezone.utc)
    except (ValueError, OverflowError):
        return None


def valid_sha(value: Any) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{40}", value) is not None


def valid_digest(value: Any) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def valid_run_id(value: Any) -> bool:
    # Local reports explicitly retain unknown provenance. A CI consumer supplies
    # its expected source run, so unknown can never satisfy a remote handoff.
    return isinstance(value, str) and (value == "unknown" or re.fullmatch(r"[1-9][0-9]*", value) is not None)


def decode_json_object(text: str | bytes) -> dict[str, Any]:
    def reject_constant(value: str) -> None:
        raise ValueError(f"non-finite JSON value: {value}")

    def finite_float(value: str) -> float:
        number = float(value)
        if not math.isfinite(number):
            raise ValueError(f"non-finite JSON number: {value}")
        return number

    result = json.loads(text, parse_constant=reject_constant, parse_float=finite_float)
    if not isinstance(result, dict):
        raise ValueError("report root must be an object")
    return result


def load_json_object(path: Path) -> dict[str, Any]:
    return decode_json_object(path.read_text(encoding="utf-8"))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def timestamp_problems(value: Any, now: datetime, max_age_hours: int, label: str) -> list[str]:
    stamp = parse_timestamp(value)
    if stamp is None:
        return [f"{label} must be an ISO-8601 timestamp with an explicit UTC offset"]
    age = (now - stamp).total_seconds() / 3600
    if age < 0:
        return [f"{label} is in the future"]
    if age > max_age_hours:
        return [f"{label} is stale: {age:.3f}h exceeds {max_age_hours}h"]
    return []


def input_record(name: str, path: Path, report: dict[str, Any]) -> dict[str, Any]:
    fields = INPUT_CONTRACTS.get(name, (None, None, ()))[2]
    timestamps = {field: report.get(field) for field in fields}
    stamps = [parse_timestamp(value) for value in timestamps.values()]
    identity = report.get("code_identity")
    research_sha = identity.get("git_commit") if isinstance(identity, dict) else None
    record = {
        "name": name,
        "path": str(path),
        "sha256": sha256(path),
        "schema_version": report.get("schema_version"),
        "status": report.get("status"),
        "git_sha": (report.get("git_sha") or research_sha) if name == "independent_research_validation" else report.get("git_sha"),
        "environment_class": report.get("environment_class"),
        "workflow_run_id": report.get("workflow_run_id"),
        "timestamps": timestamps,
        "observed_at": min(stamps).isoformat() if stamps and all(stamps) else None,
    }
    if name == "operational_acceptance":
        record.update(cycles_requested=report.get("cycles_requested"), cycles_completed=report.get("cycles_completed"))
    elif name == "independent_research_validation":
        if "code_identity" in report and (not isinstance(identity, dict) or not valid_sha(research_sha)):
            raise ValueError("research code_identity must contain a valid git_commit")
        record["source_git_sha"] = report.get("git_sha")
        record["code_identity_git_commit"] = research_sha
        reproduction = report.get("reproduction")
        record["reproduction_status"] = reproduction.get("status") if isinstance(reproduction, dict) else None
    elif name.startswith("runtime_qualification:"):
        record.update(scenario=report.get("scenario"), evidence_complete=report.get("evidence_complete"))
    return record


def input_checks(record: dict[str, Any], expected_sha: str, expected_run_id: str, now: datetime) -> list[dict[str, str]]:
    name = record.get("name")
    schema, environment, fields = INPUT_CONTRACTS[name]
    timestamps = record.get("timestamps")
    timestamp_errors = []
    if not isinstance(timestamps, dict) or set(timestamps) != set(fields):
        timestamp_errors.append("original timestamp fields are missing or unexpected")
        timestamps = {}
    for field in fields:
        timestamp_errors.extend(timestamp_problems(timestamps.get(field), now, MAX_INPUT_AGE_HOURS, field))
    stamps = [parse_timestamp(timestamps.get(field)) for field in fields]
    if stamps and all(stamps):
        if stamps != sorted(stamps):
            timestamp_errors.append("observation timestamps are reversed")
        if parse_timestamp(record.get("observed_at")) != min(stamps):
            timestamp_errors.append("observed_at does not preserve the oldest original timestamp")

    semantic_ok = True
    detail = "no additional input constraints"
    if name == "operational_acceptance":
        requested, completed = record.get("cycles_requested"), record.get("cycles_completed")
        semantic_ok = type(requested) is int and requested > 0 and type(completed) is int and completed == requested
        detail = f"cycles={completed}/{requested}"
    elif name == "independent_research_validation":
        identities = (record.get("source_git_sha"), record.get("code_identity_git_commit"))
        semantic_ok = (record.get("reproduction_status") == "PASS"
                       and all(value is None or value == record.get("git_sha") for value in identities))
        detail = f"reproduction={record.get('reproduction_status')} source identities={identities}"
    elif name.startswith("runtime_qualification:"):
        semantic_ok = record.get("scenario") == name.split(":", 1)[1] and record.get("evidence_complete") is True
        detail = f"scenario={record.get('scenario')} complete={record.get('evidence_complete')}"

    conditions = {
        "schema": (type(record.get("schema_version")) is int and record.get("schema_version") == schema, str(record.get("schema_version"))),
        "status": (record.get("status") == "PASS", str(record.get("status"))),
        "sha": (valid_sha(expected_sha) and record.get("git_sha") == expected_sha, str(record.get("git_sha"))),
        "environment": (record.get("environment_class") == environment, str(record.get("environment_class"))),
        "run": (valid_run_id(expected_run_id) and record.get("workflow_run_id") == expected_run_id, str(record.get("workflow_run_id"))),
        "timestamps": (not timestamp_errors, "; ".join(timestamp_errors) or "original timestamps are valid and fresh"),
        "semantics": (semantic_ok, detail),
    }
    return [{"name": f"{name}-{field}", "status": "PASS" if ok else "FAIL", "detail": detail}
            for field, (ok, detail) in conditions.items()]


def validate_inputs(inputs: Any, expected_sha: str, expected_run_id: str, now: datetime, composed_at: Any) -> list[str]:
    if not isinstance(inputs, list) or any(not isinstance(item, dict) for item in inputs):
        return ["inputs must be an array of input records"]
    names = [item.get("name") for item in inputs]
    if any(not isinstance(name, str) for name in names) or len(names) != len(INPUT_CONTRACTS) or set(names) != set(INPUT_CONTRACTS):
        return ["inputs must contain each of the five required identities exactly once"]
    errors = []
    composed_stamp = parse_timestamp(composed_at)
    for record in inputs:
        name = record["name"]
        if not valid_digest(record.get("sha256")) or not isinstance(record.get("path"), str) or not record["path"].strip():
            errors.append(f"{name}: missing or invalid source path/SHA-256")
        for item in input_checks(record, expected_sha, expected_run_id, now):
            if item["status"] != "PASS":
                errors.append(f"{item['name']}: {item['detail']}")
        timestamps = record.get("timestamps")
        if isinstance(timestamps, dict) and composed_stamp:
            if any((stamp := parse_timestamp(value)) is not None and stamp > composed_stamp for value in timestamps.values()):
                errors.append(f"{name}: input observation is later than readiness composition")
    return errors


def oldest_evidence_at(inputs: Any) -> str | None:
    if not isinstance(inputs, list) or not inputs:
        return None
    stamps = [parse_timestamp(item.get("observed_at")) if isinstance(item, dict) else None for item in inputs]
    return min(stamps).isoformat() if all(stamps) else None


def validate_readiness(report: Any, expected_sha: str, now: datetime, expected_run_id: str | None = None) -> list[str]:
    if not isinstance(report, dict):
        return ["release-readiness report root must be an object"]
    errors = []
    for field, expected in (("schema_version", 2), ("contract_version", CONTRACT_VERSION), ("environment_class", "ci_release_gate"),
                            ("status", "PASS"), ("git_sha", expected_sha), ("max_input_age_hours", MAX_INPUT_AGE_HOURS)):
        if type(report.get(field)) is not type(expected) or report.get(field) != expected:
            errors.append(f"release readiness {field} expected {expected}, got {report.get(field)}")
    if not valid_sha(expected_sha):
        errors.append("expected full Git SHA is required")
    run_id = report.get("workflow_run_id")
    if not valid_run_id(run_id) or (expected_run_id is not None and run_id != expected_run_id):
        errors.append("release readiness workflow_run_id does not match its source run")
    errors.extend(timestamp_problems(report.get("generated_at"), now, MAX_INPUT_AGE_HOURS, "release readiness generated_at"))
    errors.extend(validate_inputs(report.get("inputs"), expected_sha, run_id, now, report.get("generated_at")))
    oldest = oldest_evidence_at(report.get("inputs"))
    if oldest is None or parse_timestamp(report.get("oldest_evidence_at")) != parse_timestamp(oldest):
        errors.append("release readiness oldest_evidence_at does not match its original inputs")
    declared_violations = report.get("violations")
    if not isinstance(declared_violations, list) or declared_violations:
        errors.append("release readiness violations must be an empty array")
    checks = report.get("checks")
    if not isinstance(checks, list) or any(not isinstance(item, dict) for item in checks):
        errors.append("release readiness checks must be an array of successful named checks")
    else:
        names = [item.get("name") for item in checks]
        if any(not isinstance(name, str) for name in names) or len(names) != len(REQUIRED_CHECKS) or set(names) != REQUIRED_CHECKS:
            errors.append("release readiness checks must contain every required check exactly once")
        if any(item.get("status") != "PASS" for item in checks):
            errors.append("release readiness contains a non-PASS check")
    return errors


def release_provenance(readiness: dict[str, Any], readiness_sha256: str) -> dict[str, Any]:
    return {
        "contract_version": CONTRACT_VERSION,
        "max_input_age_hours": MAX_INPUT_AGE_HOURS,
        "readiness_generated_at": readiness["generated_at"],
        "readiness_sha256": readiness_sha256,
        "workflow_run_id": readiness["workflow_run_id"],
        "oldest_evidence_at": readiness["oldest_evidence_at"],
        "inputs": readiness["inputs"],
    }


def validate_rc_provenance(report: Any, expected_sha: str, now: datetime, expected_run_id: str | None = None) -> list[str]:
    if not isinstance(report, dict):
        return ["RC report root must be an object"]
    errors = []
    for field, expected in (("schema_version", 1), ("status", "PASS"), ("environment_class", "ci_artifact"), ("git_sha", expected_sha)):
        if type(report.get(field)) is not type(expected) or report.get(field) != expected:
            errors.append(f"RC {field} expected {expected}, got {report.get(field)}")
    if not valid_sha(expected_sha):
        errors.append("expected full Git SHA is required")
    run_id = report.get("workflow_run_id")
    if not valid_run_id(run_id) or (expected_run_id is not None and run_id != expected_run_id):
        errors.append("RC workflow_run_id does not match its source run")
    errors.extend(timestamp_problems(report.get("generated_at"), now, MAX_RC_AGE_HOURS, "RC generated_at"))
    if "violations" in report and (not isinstance(report["violations"], list) or report["violations"]):
        errors.append("RC contains malformed or nonempty violations")
    provenance = report.get("release_evidence")
    if not isinstance(provenance, dict):
        return errors + ["RC release_evidence provenance is missing"]
    for field, expected in (("contract_version", CONTRACT_VERSION), ("max_input_age_hours", MAX_INPUT_AGE_HOURS), ("workflow_run_id", run_id)):
        if type(provenance.get(field)) is not type(expected) or provenance.get(field) != expected:
            errors.append(f"RC release_evidence.{field} does not match the release contract")
    errors.extend(timestamp_problems(provenance.get("readiness_generated_at"), now, MAX_INPUT_AGE_HOURS, "original readiness generated_at"))
    readiness_stamp = parse_timestamp(provenance.get("readiness_generated_at"))
    rc_stamp = parse_timestamp(report.get("generated_at"))
    if readiness_stamp and rc_stamp and readiness_stamp > rc_stamp:
        errors.append("readiness was generated after RC composition")
    errors.extend(validate_inputs(provenance.get("inputs"), expected_sha, run_id, now, provenance.get("readiness_generated_at")))
    oldest = oldest_evidence_at(provenance.get("inputs"))
    if oldest is None or parse_timestamp(provenance.get("oldest_evidence_at")) != parse_timestamp(oldest):
        errors.append("RC oldest_evidence_at does not match its original inputs")
    digest = provenance.get("readiness_sha256")
    manifest = report.get("manifest")
    if not valid_digest(digest) or not isinstance(manifest, dict) or manifest.get("release_readiness_sha256") != digest:
        errors.append("RC original readiness SHA-256 does not match the manifest")
    return errors
