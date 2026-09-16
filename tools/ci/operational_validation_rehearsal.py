#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sqlite3
import tempfile
from datetime import datetime, timezone
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def backup_restore_drill(root: Path) -> dict[str, object]:
    source = root / "source.db"
    backup = root / "backup.db"
    restored = root / "restored.db"

    with sqlite3.connect(source) as database:
        database.execute("CREATE TABLE sentinel (id INTEGER PRIMARY KEY, value TEXT NOT NULL)")
        database.execute("INSERT INTO sentinel(value) VALUES (?)", ("sentum-operational-validation",))
        database.commit()
        with sqlite3.connect(backup) as backup_database:
            database.backup(backup_database)

    shutil.copy2(backup, restored)
    with sqlite3.connect(restored) as database:
        integrity = database.execute("PRAGMA quick_check").fetchone()[0]
        value = database.execute("SELECT value FROM sentinel WHERE id = 1").fetchone()[0]

    passed = integrity == "ok" and value == "sentum-operational-validation"
    return {
        "name": "backup_restore",
        "status": "PASS" if passed else "FAIL",
        "source_sha256": sha256(source),
        "backup_sha256": sha256(backup),
        "restored_sha256": sha256(restored),
        "integrity": integrity,
        "sentinel": value,
    }


def rollback_drill(root: Path) -> dict[str, object]:
    deployments = root / "deployments"
    deployments.mkdir()
    previous = deployments / "previous.bin"
    candidate = deployments / "candidate.bin"
    active = deployments / "active.bin"
    previous.write_bytes(b"sentum-previous-release\n")
    candidate.write_bytes(b"sentum-release-candidate\n")

    shutil.copy2(previous, active)
    previous_hash = sha256(active)
    shutil.copy2(candidate, active)
    candidate_hash = sha256(active)
    shutil.copy2(previous, active)
    rollback_hash = sha256(active)

    passed = previous_hash == rollback_hash and candidate_hash != rollback_hash
    return {
        "name": "artifact_rollback",
        "status": "PASS" if passed else "FAIL",
        "previous_sha256": previous_hash,
        "candidate_sha256": candidate_hash,
        "rollback_sha256": rollback_hash,
    }


def incident_recovery_drill() -> dict[str, object]:
    sequence = [
        "halt_entries",
        "preserve_execution_truth",
        "capture_health_and_queue_state",
        "reconcile_exchange_confirmed_state",
        "select_recovery_or_rollback",
        "resume_only_after_explicit_approval",
    ]
    required = {
        "halt_entries",
        "preserve_execution_truth",
        "reconcile_exchange_confirmed_state",
        "resume_only_after_explicit_approval",
    }
    passed = required.issubset(sequence)
    return {
        "name": "incident_recovery_sequence",
        "status": "PASS" if passed else "FAIL",
        "steps": sequence,
        "note": "This is a procedure rehearsal only; it does not mutate a live exchange or production runtime.",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Sentum non-production operational validation rehearsals")
    parser.add_argument("--report", default="log/operational_validation_rehearsal.json")
    parser.add_argument("--summary", default="log/operational_validation_rehearsal.md")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="sentum-operational-validation-") as temp_dir:
        root = Path(temp_dir)
        checks = [backup_restore_drill(root), rollback_drill(root), incident_recovery_drill()]

    status = "PASS" if all(check["status"] == "PASS" for check in checks) else "FAIL"
    report = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "git_sha": os.environ.get("GITHUB_SHA", "unknown"),
        "workflow_run_id": os.environ.get("GITHUB_RUN_ID", "unknown"),
        "environment_class": "ci_rehearsal",
        "status": status,
        "checks": checks,
        "limitations": [
            "No production target is contacted.",
            "No live exchange state is changed.",
            "PASS is rehearsal evidence and must not be presented as production deployment acceptance.",
        ],
    }

    report_path = Path(args.report)
    summary_path = Path(args.summary)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    lines = [
        "# Operational validation rehearsal",
        "",
        f"Status: **{status}**",
        f"Git SHA: `{report['git_sha']}`",
        "",
        "| Drill | Result |",
        "| --- | --- |",
    ]
    for check in checks:
        lines.append(f"| {check['name']} | {check['status']} |")
    lines.extend([
        "",
        "This evidence is a CI rehearsal only. It is not proof of a real production deployment, restore or incident exercise.",
        "",
    ])
    summary_path.write_text("\n".join(lines), encoding="utf-8")
    print(summary_path.read_text(encoding="utf-8"), end="")
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
