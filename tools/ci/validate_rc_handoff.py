#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any


class HandoffValidationFailure(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_report(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise HandoffValidationFailure(f"missing RC package report: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise HandoffValidationFailure(f"invalid RC package report: {error}") from error
    if not isinstance(value, dict):
        raise HandoffValidationFailure("RC package report root must be an object")
    return value


def require_file(path: Path, description: str) -> None:
    if not path.is_file():
        raise HandoffValidationFailure(f"missing {description}: {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate the Sentum RC artifact handoff layout")
    parser.add_argument("--artifact-root", default="evidence/rc")
    parser.add_argument("--expected-sha", default=os.environ.get("GITHUB_SHA", "unknown"))
    args = parser.parse_args()

    root = Path(args.artifact_root)
    expected_sha = str(args.expected_sha)
    if not expected_sha or expected_sha == "unknown":
        print("RC handoff validation failure: expected Git SHA is required", file=sys.stderr)
        return 2

    report_path = root / "log" / "rc_package.json"
    summary_path = root / "log" / "rc_package.md"
    archive_relative = Path("artifacts") / f"sentum-rc-{expected_sha[:12]}.tar.gz"
    archive_path = root / archive_relative
    checksum_path = Path(str(archive_path) + ".sha256")

    try:
        require_file(summary_path, "RC package summary")
        require_file(archive_path, "RC archive")
        require_file(checksum_path, "RC archive checksum")
        report = load_report(report_path)

        if report.get("schema_version") != 1:
            raise HandoffValidationFailure(
                f"unsupported RC package report schema: {report.get('schema_version')}"
            )
        if report.get("status") != "PASS":
            raise HandoffValidationFailure("RC package report is not PASS")
        if report.get("git_sha") != expected_sha:
            raise HandoffValidationFailure(
                f"RC package report Git SHA mismatch: expected {expected_sha}, got {report.get('git_sha')}"
            )

        reported_archive = report.get("archive")
        if reported_archive != archive_relative.as_posix():
            raise HandoffValidationFailure(
                f"RC package report archive path mismatch: expected {archive_relative.as_posix()}, got {reported_archive}"
            )

        actual_digest = sha256(archive_path)
        reported_digest = report.get("archive_sha256")
        if reported_digest != actual_digest:
            raise HandoffValidationFailure(
                f"RC archive digest mismatch: report={reported_digest}, actual={actual_digest}"
            )

        checksum_line = checksum_path.read_text(encoding="utf-8").strip()
        expected_checksum_line = f"{actual_digest}  {archive_path.name}"
        if checksum_line != expected_checksum_line:
            raise HandoffValidationFailure(
                f"RC checksum evidence mismatch: expected '{expected_checksum_line}', got '{checksum_line}'"
            )

        source_run_id = os.environ.get("SOURCE_WORKFLOW_RUN_ID", os.environ.get("GITHUB_RUN_ID", "unknown"))
        print(
            "RC handoff validation PASS: "
            f"git_sha={expected_sha} source_workflow_run_id={source_run_id} "
            f"report={report_path} archive={archive_path}"
        )
        return 0
    except (HandoffValidationFailure, OSError) as error:
        print(f"RC handoff validation failure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
