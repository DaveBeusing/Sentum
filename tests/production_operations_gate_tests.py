#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "tools" / "ci" / "production_operations_gate.py"
SHA = "a" * 40


def valid_rc_report() -> dict:
    observed = (datetime.now(timezone.utc) - timedelta(minutes=1)).isoformat()
    generated = datetime.now(timezone.utc).isoformat()
    records = []
    contracts = (
        ("performance", 2, "ci_hosted_runner", {"generated_at": observed}),
        ("operational_acceptance", 1, "ci_rehearsal", {"generated_at": observed}),
        ("independent_research_validation", 1, "research_validation", {"validated_at_utc": observed}),
        ("runtime_qualification:paper-soak", 1, "ci_rehearsal", {"started_at_utc": observed, "ended_at_utc": observed}),
        ("runtime_qualification:all-faults", 1, "ci_rehearsal", {"started_at_utc": observed, "ended_at_utc": observed}),
    )
    for name, schema, environment, timestamps in contracts:
        record = {"name": name, "path": f"evidence/{name}.json", "sha256": "b" * 64,
                  "schema_version": schema, "environment_class": environment, "status": "PASS",
                  "git_sha": SHA, "workflow_run_id": "42", "timestamps": timestamps, "observed_at": observed}
        if name == "operational_acceptance":
            record.update(cycles_requested=5, cycles_completed=5)
        elif name == "independent_research_validation":
            record["reproduction_status"] = "PASS"
        elif name.startswith("runtime_qualification:"):
            record.update(scenario=name.split(":", 1)[1], evidence_complete=True)
        records.append(record)
    return {"schema_version": 1, "status": "PASS", "environment_class": "ci_artifact",
            "git_sha": SHA, "workflow_run_id": "42", "generated_at": generated,
            "manifest": {"release_readiness_sha256": "c" * 64},
            "release_evidence": {"contract_version": 1, "max_input_age_hours": 336,
                                 "readiness_generated_at": generated, "readiness_sha256": "c" * 64,
                                 "workflow_run_id": "42", "oldest_evidence_at": observed, "inputs": records}}


class ProductionOperationsGateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def evaluate(self, payload=None, *, raw=None, expected_sha=SHA, missing=False, expected_run="42"):
        source = self.root / "rc.json"
        if missing:
            source.unlink(missing_ok=True)
        else:
            source.write_text(raw if raw is not None else json.dumps(payload), encoding="utf-8")
        report = self.root / "gate.json"
        summary = self.root / "gate.md"
        environment = {**os.environ, "SOURCE_WORKFLOW_RUN_ID": expected_run}
        result = subprocess.run(
            [sys.executable, str(GATE), "--rc-report", str(source), "--expected-sha", expected_sha,
             "--report", str(report), "--summary", str(summary)],
            cwd=ROOT, env=environment, text=True, capture_output=True, check=False,
        )
        self.assertNotIn("Traceback", result.stderr)
        self.assertTrue(report.is_file(), result.stderr)
        output = json.loads(report.read_text(), parse_constant=lambda value: self.fail(f"nonstandard output JSON: {value}"))
        self.assertIn(output["status"], summary.read_text())
        return result, output

    def assert_failure(self, payload=None, **kwargs):
        result, report = self.evaluate(payload, **kwargs)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(report["violations"])
        return report

    def test_complete_same_commit_and_source_run_pass_directly(self):
        result, report = self.evaluate(valid_rc_report())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["rc_package"]["git_sha"], SHA)
        self.assertEqual(report["violations"], [])

    def test_every_nonobject_root_fails_without_a_preceding_validator(self):
        for payload in (None, [], [valid_rc_report()], "PASS", "", 0, 1.5, True, False):
            with self.subTest(root=type(payload).__name__):
                report = self.assert_failure(payload)
                self.assertIsNone(report["rc_package"]["status"])
                self.assertTrue(any("object" in item for item in report["violations"]))

    def test_missing_and_invalid_json_retain_failure_evidence(self):
        self.assert_failure(missing=True)
        for raw in ("{not-json", "", "NaN", "Infinity", "-Infinity", "1e999", '{"status": 1e999}'):
            with self.subTest(raw=raw):
                self.assert_failure(raw=raw)

    def test_required_rc_fields_are_enforced(self):
        mutations = {"schema_version": (None, 0, 2, True, 1.0, "1"),
                     "status": (None, "FAIL", True, {}, 1e300),
                     "git_sha": (None, "b" * 40, "", [], True),
                     "environment_class": (None, "ci_rehearsal"),
                     "workflow_run_id": (None, "43", 42)}
        for field, values in mutations.items():
            for value in values:
                with self.subTest(field=field, value=value):
                    report = valid_rc_report()
                    report[field] = value
                    self.assert_failure(report)
            report = valid_rc_report()
            del report[field]
            with self.subTest(missing=field):
                self.assert_failure(report)

    def test_minimal_pass_wrapper_is_not_accepted(self):
        self.assert_failure({"status": "PASS", "git_sha": SHA})

    def test_expected_identity_must_be_real_and_match_source_run(self):
        self.assert_failure(valid_rc_report(), expected_sha="unknown")
        self.assert_failure(valid_rc_report(), expected_run="43")

    def test_malformed_or_expired_original_provenance_is_rejected(self):
        for provenance in (None, [], {}, "PASS"):
            report = valid_rc_report()
            report["release_evidence"] = provenance
            with self.subTest(provenance=provenance):
                self.assert_failure(report)
        report = valid_rc_report()
        for item in report["release_evidence"]["inputs"]:
            item["timestamps"] = {key: "2001-01-01T00:00:00Z" for key in item["timestamps"]}
            item["observed_at"] = "2001-01-01T00:00:00Z"
        report["release_evidence"]["oldest_evidence_at"] = "2001-01-01T00:00:00Z"
        self.assert_failure(report)


if __name__ == "__main__":
    unittest.main()
