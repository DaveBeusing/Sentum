#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import hashlib
import json
import os
from datetime import datetime, timezone
from pathlib import Path


REQUIRED_SECTIONS = [
    "## Production promotion prerequisites",
    "## Deployment procedure",
    "## Rollback procedure",
    "## Backup and recovery baseline",
    "## Incident baseline",
    "## Post-deployment verification",
    "## Required production handoff record",
]

REQUIRED_RECORD_FIELDS = [
    "Git SHA",
    "RC archive name and SHA-256",
    "deployed binary SHA-256",
    "target environment",
    "deployment start/end UTC timestamps",
    "operator/approver identifiers",
    "pre-deployment backup/recovery-point identifier",
    "release-readiness result",
    "reconciliation result",
    "rollback target",
    "final status",
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate the Sentum production-operations baseline")
    parser.add_argument("--runbook", default="docs/PRODUCTION_OPERATIONS.md")
    parser.add_argument("--rc-handoff", default="docs/RC_HANDOFF.md")
    parser.add_argument("--release-readiness", default="docs/RELEASE_READINESS.md")
    parser.add_argument("--live-safety", default="docs/LIVE_TRADING_SAFETY.md")
    parser.add_argument("--account-reconciliation", default="docs/ACCOUNT_RECONCILIATION.md")
    parser.add_argument("--rc-report", required=True)
    parser.add_argument("--expected-sha", default=os.environ.get("GITHUB_SHA", "unknown"))
    parser.add_argument("--report", default="log/production_operations_gate.json")
    parser.add_argument("--summary", default="log/production_operations_gate.md")
    args = parser.parse_args()

    paths = {
        "runbook": Path(args.runbook),
        "rc_handoff": Path(args.rc_handoff),
        "release_readiness": Path(args.release_readiness),
        "live_safety": Path(args.live_safety),
        "account_reconciliation": Path(args.account_reconciliation),
    }

    violations: list[str] = []
    for name, path in paths.items():
        if not path.is_file():
            violations.append(f"missing required operations document: {name} ({path})")

    rc_report_path = Path(args.rc_report)
    rc_report = None
    if not rc_report_path.is_file():
        violations.append(f"missing RC package report: {rc_report_path}")
    else:
        try:
            rc_report = json.loads(rc_report_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            violations.append(f"invalid RC package report JSON: {error}")
        if isinstance(rc_report, dict):
            if rc_report.get("status") != "PASS":
                violations.append("RC package report is not PASS")
            if rc_report.get("git_sha") != args.expected_sha:
                violations.append("RC package report Git SHA does not match the evaluated commit")

    runbook_text = paths["runbook"].read_text(encoding="utf-8") if paths["runbook"].is_file() else ""
    for section in REQUIRED_SECTIONS:
        if section not in runbook_text:
            violations.append(f"runbook missing section: {section}")
    for field in REQUIRED_RECORD_FIELDS:
        if field.lower() not in runbook_text.lower():
            violations.append(f"runbook handoff record missing field: {field}")

    forbidden = ["clear kill switch automatically", "synthesize fills", "copy credentials into rc"]
    lowered = runbook_text.lower()
    for phrase in forbidden:
        if phrase in lowered:
            violations.append(f"runbook contains forbidden operational shortcut: {phrase}")

    report = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "git_sha": args.expected_sha,
        "workflow_run_id": os.environ.get("GITHUB_RUN_ID", "unknown"),
        "status": "PASS" if not violations else "FAIL",
        "rc_package": {
            "path": str(rc_report_path),
            "sha256": sha256(rc_report_path) if rc_report_path.is_file() else None,
            "status": rc_report.get("status") if isinstance(rc_report, dict) else None,
            "git_sha": rc_report.get("git_sha") if isinstance(rc_report, dict) else None,
        },
        "documents": {
            name: {
                "path": str(path),
                "sha256": sha256(path) if path.is_file() else None,
            }
            for name, path in paths.items()
        },
        "required_sections": REQUIRED_SECTIONS,
        "required_handoff_fields": REQUIRED_RECORD_FIELDS,
        "violations": violations,
    }

    report_path = Path(args.report)
    summary_path = Path(args.summary)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    lines = [
        "# Production operations gate",
        "",
        f"Status: **{report['status']}**",
        f"Git SHA: `{report['git_sha']}`",
        f"RC package report: **{report['rc_package']['status'] or 'missing'}**",
        "",
        "| Document | SHA-256 |",
        "| --- | --- |",
    ]
    for name, item in report["documents"].items():
        lines.append(f"| {name} | `{item['sha256'] or 'missing'}` |")
    if violations:
        lines.extend(["", "## Violations"])
        lines.extend(f"- {item}" for item in violations)
    summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(summary_path.read_text(encoding="utf-8"), end="")
    return 0 if not violations else 1


if __name__ == "__main__":
    raise SystemExit(main())
