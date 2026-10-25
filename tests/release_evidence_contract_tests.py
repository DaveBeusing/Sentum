#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

from release_evidence_fixtures import (
    ROOT, RUN_ID, SHA, environment, prepare_package_workspace, run_package, run_readiness, write_inputs,
)

sys.path.insert(0, str(ROOT / "tools" / "ci"))
from release_evidence_contract import MAX_INPUT_AGE_HOURS, timestamp_problems


class ReleaseReadinessBoundaryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.inputs = write_inputs(self.root)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def update(self, name: str, change) -> None:
        payload = json.loads(self.inputs[name].read_text())
        change(payload)
        self.inputs[name].write_text(json.dumps(payload), encoding="utf-8")

    def assert_failed(self, result) -> dict:
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        report = json.loads((self.root / "release_readiness.json").read_text())
        self.assertEqual("FAIL", report["status"])
        self.assertTrue(report["violations"])
        self.assertIn("**FAIL**", (self.root / "release_readiness.md").read_text())
        return report

    def test_complete_named_set_preserves_every_original_identity_and_timestamp(self) -> None:
        result = run_readiness(self.root, self.inputs)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        report = json.loads((self.root / "release_readiness.json").read_text())
        self.assertEqual(5, len(report["inputs"]))
        for record in report["inputs"]:
            original = Path(record["path"])
            self.assertEqual(hashlib.sha256(original.read_bytes()).hexdigest(), record["sha256"])
            source = json.loads(original.read_text())
            self.assertEqual({field: source[field] for field in record["timestamps"]}, record["timestamps"])
            self.assertEqual(RUN_ID, record["workflow_run_id"])
        self.assertEqual(min(item["observed_at"] for item in report["inputs"]), report["oldest_evidence_at"])

    def test_missing_qualification_scenario_is_rejected(self) -> None:
        self.assert_failed(run_readiness(self.root, self.inputs, [self.inputs["paper-soak"]]))

    def test_duplicate_scenario_cannot_replace_the_other_scenario(self) -> None:
        self.assert_failed(run_readiness(self.root, self.inputs, [self.inputs["paper-soak"], self.inputs["paper-soak"]]))

    def test_additional_duplicate_scenario_is_rejected(self) -> None:
        self.assert_failed(run_readiness(self.root, self.inputs,
                                        [self.inputs["paper-soak"], self.inputs["all-faults"], self.inputs["all-faults"]]))

    def test_unknown_or_missing_scenario_never_uses_filename_as_identity(self) -> None:
        for value in ("unrelated-scenario", "", None, False, ["paper-soak"]):
            with self.subTest(scenario=value):
                self.update("paper-soak", lambda report: report.update(scenario=value))
                self.assert_failed(run_readiness(self.root, self.inputs))

    def test_filenames_and_argument_order_do_not_override_report_identity(self) -> None:
        renamed = self.root / "arbitrary-filename.json"
        renamed.write_bytes(self.inputs["paper-soak"].read_bytes())
        result = run_readiness(self.root, self.inputs, [self.inputs["all-faults"], renamed])
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_all_leaf_timestamp_contracts_reject_missing_invalid_future_and_stale_values(self) -> None:
        fields = {"performance": "generated_at", "operational": "generated_at", "research": "validated_at_utc",
                  "paper-soak": "started_at_utc", "all-faults": "ended_at_utc"}
        now = datetime.now(timezone.utc)
        bad = [None, "invalid", "2026-01-01T00:00:00", (now + timedelta(hours=1)).isoformat(),
               (now - timedelta(hours=337)).isoformat()]
        for name, field in fields.items():
            original = self.inputs[name].read_bytes()
            for value in bad:
                with self.subTest(name=name, value=value):
                    self.inputs[name].write_bytes(original)
                    self.update(name, lambda report: report.update({field: value}))
                    self.assert_failed(run_readiness(self.root, self.inputs))
            self.inputs[name].write_bytes(original)

    def test_reversed_qualification_observation_window_is_rejected(self) -> None:
        self.update("paper-soak", lambda report: report.update(ended_at_utc="2001-01-01T00:00:00Z"))
        self.assert_failed(run_readiness(self.root, self.inputs))

    def test_age_boundary_is_inclusive_and_does_not_allow_future_time(self) -> None:
        now = datetime(2026, 10, 25, tzinfo=timezone.utc)
        boundary = now - timedelta(hours=MAX_INPUT_AGE_HOURS)
        self.assertEqual([], timestamp_problems(boundary.isoformat(), now, MAX_INPUT_AGE_HOURS, "leaf"))
        self.assertTrue(timestamp_problems((boundary - timedelta(microseconds=1)).isoformat(), now, MAX_INPUT_AGE_HOURS, "leaf"))
        self.assertTrue(timestamp_problems((now + timedelta(microseconds=1)).isoformat(), now, MAX_INPUT_AGE_HOURS, "leaf"))

    def test_unknown_or_mismatched_ci_run_and_commit_are_rejected(self) -> None:
        for name in self.inputs:
            original = self.inputs[name].read_bytes()
            for field, value in (("workflow_run_id", "unknown"), ("workflow_run_id", "43"), ("git_sha", "b" * 40)):
                with self.subTest(name=name, field=field, value=value):
                    self.inputs[name].write_bytes(original)
                    self.update(name, lambda report: report.update({field: value}))
                    self.assert_failed(run_readiness(self.root, self.inputs))
            self.inputs[name].write_bytes(original)

    def test_research_top_level_and_persisted_code_identities_must_agree(self) -> None:
        self.update("research", lambda report: report.update(code_identity={"git_commit": "b" * 40}))
        self.assert_failed(run_readiness(self.root, self.inputs))
        self.update("research", lambda report: report.update(code_identity={"git_commit": SHA}))
        result = run_readiness(self.root, self.inputs)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_consistent_local_unknown_identity_is_preserved(self) -> None:
        self.inputs = write_inputs(self.root, run_id="unknown")
        result = run_readiness(self.root, self.inputs, run_id="unknown")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        report = json.loads((self.root / "release_readiness.json").read_text())
        self.assertEqual("unknown", report["workflow_run_id"])
        self.assert_failed(run_readiness(self.root, self.inputs, run_id=RUN_ID))

    def test_malformed_evidence_writes_structured_failure(self) -> None:
        for content in ("null", "[]", "{", '{"status":"PASS","other":NaN}', '{"status":"PASS","other":1e999}'):
            with self.subTest(content=content):
                self.inputs["performance"].write_text(content, encoding="utf-8")
                self.assert_failed(run_readiness(self.root, self.inputs))


class ReleasePackagingBoundaryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        prepare_package_workspace(self.root)
        self.inputs = write_inputs(self.root)
        result = run_readiness(self.root, self.inputs)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.valid = json.loads((self.root / "release_readiness.json").read_text())

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_readiness(self, payload) -> None:
        (self.root / "release_readiness.json").write_text(json.dumps(payload), encoding="utf-8")

    def assert_packaging_failure(self, payload) -> dict:
        self.write_readiness(payload)
        result = run_package(self.root)
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        report = json.loads((self.root / "log/rc_package.json").read_text())
        self.assertEqual("FAIL", report["status"])
        self.assertTrue(report["violations"])
        self.assertFalse(list((self.root / "artifacts").glob("*.tar.gz")))
        return report

    def handoff(self, run_id: str = RUN_ID) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(ROOT / "tools/ci/validate_rc_handoff.py"), "--artifact-root", ".",
                               "--expected-sha", SHA], cwd=self.root, env=environment(run_id),
                              capture_output=True, text=True, timeout=10)

    def test_complete_contract_packages_original_readiness_and_handoff_passes(self) -> None:
        result = run_package(self.root)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        report = json.loads((self.root / "log/rc_package.json").read_text())
        self.assertEqual(self.valid["oldest_evidence_at"], report["release_evidence"]["oldest_evidence_at"])
        self.assertEqual(self.valid["inputs"], report["release_evidence"]["inputs"])
        with tarfile.open(self.root / report["archive"], "r:gz") as archive:
            data = archive.extractfile(f"sentum-rc-{SHA[:12]}/evidence/release_readiness.json").read()
        self.assertEqual((self.root / "release_readiness.json").read_bytes(), data)
        self.assertEqual(hashlib.sha256(data).hexdigest(), report["release_evidence"]["readiness_sha256"])
        result = self.handoff()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_two_field_or_nonobject_readiness_is_rejected_with_failure_evidence(self) -> None:
        for value in ({"status": "PASS", "git_sha": SHA}, None, [], "PASS", 1):
            with self.subTest(value=value):
                self.assert_packaging_failure(value)

    def test_packaging_rechecks_schema_contract_environment_commit_and_source_run(self) -> None:
        for field, value in (("schema_version", 1), ("schema_version", True), ("contract_version", None),
                             ("environment_class", "ci_rehearsal"), ("git_sha", "b" * 40),
                             ("workflow_run_id", "43"), ("max_input_age_hours", 9999)):
            with self.subTest(field=field):
                payload = copy.deepcopy(self.valid)
                payload[field] = value
                self.assert_packaging_failure(payload)

    def test_missing_duplicate_failed_and_unknown_check_results_are_rejected(self) -> None:
        for kind in ("missing", "duplicate", "failed", "unknown", "nonobject"):
            with self.subTest(kind=kind):
                payload = copy.deepcopy(self.valid)
                if kind == "missing":
                    payload["checks"].pop()
                elif kind == "duplicate":
                    payload["checks"][0] = payload["checks"][1]
                elif kind == "failed":
                    payload["checks"][0]["status"] = "FAIL"
                elif kind == "unknown":
                    payload["checks"][0]["name"] = "unrelated"
                else:
                    payload["checks"] = None
                self.assert_packaging_failure(payload)

    def test_declared_violations_cannot_be_overridden_by_a_pass_status(self) -> None:
        for value in (["unresolved failure"], None, "", {}):
            with self.subTest(value=value):
                payload = copy.deepcopy(self.valid)
                payload["violations"] = value
                self.assert_packaging_failure(payload)

    def test_input_metadata_is_independently_rechecked_despite_all_pass_checks(self) -> None:
        changes = [lambda items: items.pop(), lambda items: items.append(items[0]),
                   lambda items: items[0].update(sha256="invalid"),
                   lambda items: items[0].update(environment_class="wrong"),
                   lambda items: items[1].update(cycles_completed=0),
                   lambda items: items[2].update(reproduction_status="NOT_CHECKED"),
                   lambda items: items[3].update(evidence_complete=False),
                   lambda items: items[3].update(scenario="all-faults")]
        for change in changes:
            with self.subTest(change=change):
                payload = copy.deepcopy(self.valid)
                change(payload["inputs"])
                self.assert_packaging_failure(payload)

    def test_fresh_readiness_wrapper_cannot_refresh_stale_leaf_evidence(self) -> None:
        payload = copy.deepcopy(self.valid)
        stale = (datetime.now(timezone.utc) - timedelta(hours=337)).isoformat()
        payload["generated_at"] = datetime.now(timezone.utc).isoformat()
        for record in payload["inputs"]:
            record["timestamps"] = {field: stale for field in record["timestamps"]}
            record["observed_at"] = stale
        payload["oldest_evidence_at"] = stale
        self.assert_packaging_failure(payload)

    def test_forged_new_observed_at_cannot_hide_old_original_timestamp(self) -> None:
        payload = copy.deepcopy(self.valid)
        payload["inputs"][0]["timestamps"]["generated_at"] = "2001-01-01T00:00:00Z"
        self.assert_packaging_failure(payload)

    def test_expired_readiness_and_future_wrapper_are_rejected(self) -> None:
        for delta in (-337, 1):
            with self.subTest(delta=delta):
                payload = copy.deepcopy(self.valid)
                payload["generated_at"] = (datetime.now(timezone.utc) + timedelta(hours=delta)).isoformat()
                self.assert_packaging_failure(payload)

    def test_handoff_checks_exact_source_workflow_run(self) -> None:
        self.assertEqual(0, run_package(self.root).returncode)
        result = self.handoff("43")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("workflow_run_id", result.stderr)

    def test_handoff_rejects_rc_snapshot_drift_from_immutable_embedded_readiness(self) -> None:
        self.assertEqual(0, run_package(self.root).returncode)
        path = self.root / "log/rc_package.json"
        report = json.loads(path.read_text())
        record = report["release_evidence"]["inputs"][0]
        old = datetime.fromisoformat(record["observed_at"])
        replacement = (old - timedelta(seconds=1)).isoformat()
        record["observed_at"] = replacement
        record["timestamps"]["generated_at"] = replacement
        path.write_text(json.dumps(report))
        result = self.handoff()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("original embedded readiness", result.stderr)


if __name__ == "__main__":
    unittest.main()
