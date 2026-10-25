#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tarfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from release_evidence_contract import (
    decode_json_object, release_provenance, validate_rc_provenance, validate_readiness,
)


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
        value = decode_json_object(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
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

        now = datetime.now(timezone.utc)
        source_run_id = os.environ.get("SOURCE_WORKFLOW_RUN_ID", os.environ.get("GITHUB_RUN_ID"))
        violations = validate_rc_provenance(report, expected_sha, now, source_run_id)
        if violations:
            raise HandoffValidationFailure("; ".join(violations))

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

        # Read the immutable readiness record without extracting untrusted paths.
        root_name = f"sentum-rc-{expected_sha[:12]}"
        with tarfile.open(archive_path, "r:gz") as archive:
            def embedded(name: str) -> bytes:
                members = [member for member in archive.getmembers() if member.name == f"{root_name}/{name}"]
                if len(members) != 1 or not members[0].isfile() or members[0].size > 2 * 1024 * 1024:
                    raise HandoffValidationFailure(f"missing, duplicated or oversized embedded {name}")
                stream = archive.extractfile(members[0])
                if stream is None:
                    raise HandoffValidationFailure(f"cannot read embedded {name}")
                return stream.read()

            readiness_bytes = embedded("evidence/release_readiness.json")
            readiness = decode_json_object(readiness_bytes)
            manifest = decode_json_object(embedded("MANIFEST.json"))
        readiness_digest = hashlib.sha256(readiness_bytes).hexdigest()
        if manifest != report.get("manifest") or manifest.get("git_sha") != expected_sha:
            raise HandoffValidationFailure("embedded manifest does not match the RC report")
        violations = validate_readiness(readiness, expected_sha, now, report.get("workflow_run_id"))
        if violations:
            raise HandoffValidationFailure("embedded readiness: " + "; ".join(violations))
        if release_provenance(readiness, readiness_digest) != report.get("release_evidence"):
            raise HandoffValidationFailure("RC provenance does not match the original embedded readiness")

        print(
            "RC handoff validation PASS: "
            f"git_sha={expected_sha} source_workflow_run_id={source_run_id or report.get('workflow_run_id')} "
            f"report={report_path} archive={archive_path}"
        )
        return 0
    except (HandoffValidationFailure, OSError, ValueError, TypeError, tarfile.TarError) as error:
        print(f"RC handoff validation failure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
