# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

"""Small release input fixtures shared by evidence-boundary regression tests."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SHA = "a" * 40
RUN_ID = "42"


def environment(run_id: str = RUN_ID) -> dict[str, str]:
    result = os.environ.copy()
    result.update(GITHUB_SHA=SHA, GITHUB_RUN_ID=run_id, SOURCE_WORKFLOW_RUN_ID=run_id)
    return result


def write_inputs(root: Path, now: datetime | None = None, run_id: str = RUN_ID) -> dict[str, Path]:
    now = now or datetime.now(timezone.utc) - timedelta(minutes=1)
    base = {"status": "PASS", "git_sha": SHA, "workflow_run_id": run_id}
    reports = {
        "performance": dict(base, schema_version=2, environment_class="ci_hosted_runner", generated_at=now.isoformat()),
        "operational": dict(base, schema_version=1, environment_class="ci_rehearsal", generated_at=now.isoformat(), cycles_requested=5, cycles_completed=5),
        "research": dict(base, schema_version=1, environment_class="research_validation", validated_at_utc=now.isoformat(), reproduction={"status": "PASS"}),
    }
    for scenario in ("paper-soak", "all-faults"):
        reports[scenario] = dict(base, schema_version=1, environment_class="ci_rehearsal", scenario=scenario,
                                started_at_utc=(now - timedelta(seconds=10)).isoformat(),
                                ended_at_utc=now.isoformat(), evidence_complete=True)
    directory = root / "inputs"
    directory.mkdir(parents=True, exist_ok=True)
    paths = {}
    for name, report in reports.items():
        paths[name] = directory / f"{name}.json"
        paths[name].write_text(json.dumps(report), encoding="utf-8")
    return paths


def run_readiness(root: Path, inputs: dict[str, Path], qualifications: list[Path] | None = None, run_id: str = RUN_ID) -> subprocess.CompletedProcess[str]:
    args = [sys.executable, str(ROOT / "tools/ci/release_readiness.py"),
            "--performance", str(inputs["performance"]), "--operational", str(inputs["operational"]),
            "--research", str(inputs["research"]), "--expected-sha", SHA,
            "--report", str(root / "release_readiness.json"), "--summary", str(root / "release_readiness.md")]
    for path in qualifications if qualifications is not None else [inputs["paper-soak"], inputs["all-faults"]]:
        args.extend(["--qualification", str(path)])
    return subprocess.run(args, cwd=ROOT, env=environment(run_id), capture_output=True, text=True, timeout=10)


def prepare_package_workspace(root: Path) -> None:
    (root / "docs").mkdir(exist_ok=True)
    for name in ("ACCOUNT_RECONCILIATION.md", "LIVE_TRADING_SAFETY.md", "OPERATIONAL_ACCEPTANCE.md",
                 "OPERATIONAL_SAFETY_UX.md", "RELEASE_READINESS.md", "READINESS_EVIDENCE.md", "RUNTIME_LIFECYCLE.md"):
        shutil.copy2(ROOT / "docs" / name, root / "docs" / name)
    for name in ("README.md", "LICENSE.md", "DISCLAIMER.md"):
        shutil.copy2(ROOT / name, root / name)
    (root / "sentum").write_bytes(b"release binary fixture\n")


def run_package(root: Path, run_id: str = RUN_ID) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(ROOT / "tools/ci/package_release_candidate.py"),
                           "--binary", "sentum", "--readiness", "release_readiness.json", "--git-sha", SHA],
                          cwd=root, env=environment(run_id), capture_output=True, text=True, timeout=10)


def static_rc_provenance(now: datetime, sha: str = SHA, run_id: str = RUN_ID) -> dict:
    """A complete snapshot for tests that independently control evaluation time."""
    contracts = [
        ("performance", 2, "ci_hosted_runner", ("generated_at",)),
        ("operational_acceptance", 1, "ci_rehearsal", ("generated_at",)),
        ("independent_research_validation", 1, "research_validation", ("validated_at_utc",)),
        ("runtime_qualification:paper-soak", 1, "ci_rehearsal", ("started_at_utc", "ended_at_utc")),
        ("runtime_qualification:all-faults", 1, "ci_rehearsal", ("started_at_utc", "ended_at_utc")),
    ]
    inputs = []
    for name, schema, environment_class, fields in contracts:
        record = {"name": name, "path": name + ".json", "sha256": "f" * 64, "schema_version": schema,
                  "status": "PASS", "git_sha": sha, "environment_class": environment_class,
                  "workflow_run_id": run_id, "timestamps": {field: now.isoformat() for field in fields},
                  "observed_at": now.isoformat()}
        if name == "operational_acceptance":
            record.update(cycles_requested=5, cycles_completed=5)
        if name == "independent_research_validation":
            record["reproduction_status"] = "PASS"
        if name.startswith("runtime_qualification:"):
            record.update(scenario=name.split(":", 1)[1], evidence_complete=True)
        inputs.append(record)
    return {"contract_version": 1, "max_input_age_hours": 336, "readiness_generated_at": now.isoformat(),
            "readiness_sha256": "c" * 64, "workflow_run_id": run_id, "oldest_evidence_at": now.isoformat(),
            "inputs": inputs}
