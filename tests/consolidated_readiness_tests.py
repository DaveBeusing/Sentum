#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
	"consolidated_readiness",
	ROOT / "tools" / "ci" / "consolidated_readiness.py",
)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class ConsolidatedReadinessTests(unittest.TestCase):
	def setUp(self) -> None:
		self.now = datetime(2026, 9, 23, 12, 0, tzinfo=timezone.utc)
		self.sha = "a" * 40
		self.temp = tempfile.TemporaryDirectory()
		self.root = Path(self.temp.name)
		self.policy = json.loads(
			(ROOT / "config" / "readiness_evidence_policy.json").read_text(encoding="utf-8")
		)

	def tearDown(self) -> None:
		self.temp.cleanup()

	def write(self, name: str, payload: dict) -> Path:
		path = self.root / f"{name}.json"
		path.write_text(json.dumps(payload), encoding="utf-8")
		return path

	def base_payload(self, name: str) -> dict:
		contract = self.policy["required_repository_evidence"][name]
		payload = {
			"schema_version": contract["schema_version"],
			"status": "PASS",
			"git_sha": self.sha,
			"environment_class": contract["allowed_environment_classes"][0],
			"generated_at": self.now.isoformat(),
			"workflow_run_id": "42",
		}
		if name == "rc_package":
			payload["binary_sha256"] = "d" * 64
			payload["archive_sha256"] = "b" * 64
			payload["manifest"] = {"release_readiness_sha256": "c" * 64}
		return payload

	def complete_evidence(self) -> dict[str, Path]:
		evidence: dict[str, Path] = {}
		links = self.policy.get("artifact_links", [])
		for name in self.policy["required_repository_evidence"]:
			payload = self.base_payload(name)
			for link in links:
				if link.get("downstream") != name:
					continue
				upstream = str(link["upstream"])
				field = str(link["downstream_field"])
				self.assertIn(upstream, evidence)
				value = MODULE.sha256(evidence[upstream])
				target = payload
				parts = field.split(".")
				for part in parts[:-1]:
					target = target.setdefault(part, {})
				target[parts[-1]] = value
			evidence[name] = self.write(name, payload)
		return evidence

	def test_complete_same_commit_is_repository_ready(self) -> None:
		report = MODULE.evaluate(self.policy, self.complete_evidence(), self.sha, self.now)
		self.assertEqual("REPOSITORY_READY", report["status"])
		self.assertEqual("PASS", report["repository_evidence_status"])
		self.assertEqual("NOT_PROVIDED", report["production_acceptance_status"])

	def test_missing_upstream_is_blocked(self) -> None:
		evidence = self.complete_evidence()
		evidence.pop("reliability")
		report = MODULE.evaluate(self.policy, evidence, self.sha, self.now)
		self.assertEqual("BLOCKED", report["status"])

	def test_wrong_git_sha_is_blocked(self) -> None:
		evidence = self.complete_evidence()
		payload = self.base_payload("production_operations")
		payload["git_sha"] = "b" * 40
		evidence["production_operations"] = self.write("wrong-sha", payload)
		report = MODULE.evaluate(self.policy, evidence, self.sha, self.now)
		self.assertEqual("BLOCKED", report["status"])

	def test_expired_evidence_is_blocked(self) -> None:
		evidence = self.complete_evidence()
		payload = self.base_payload("continuous_readiness")
		payload["generated_at"] = (self.now - timedelta(hours=721)).isoformat()
		evidence["continuous_readiness"] = self.write("stale", payload)
		report = MODULE.evaluate(self.policy, evidence, self.sha, self.now)
		self.assertEqual("BLOCKED", report["status"])

	def test_wrong_environment_class_is_blocked(self) -> None:
		evidence = self.complete_evidence()
		payload = self.base_payload("operational_validation")
		payload["environment_class"] = "target_environment"
		evidence["operational_validation"] = self.write("wrong-env", payload)
		report = MODULE.evaluate(self.policy, evidence, self.sha, self.now)
		self.assertEqual("BLOCKED", report["status"])

	def test_failed_evidence_cannot_produce_ready_state(self) -> None:
		evidence = self.complete_evidence()
		payload = self.base_payload("resilience")
		payload["status"] = "FAIL"
		evidence["resilience"] = self.write("failed", payload)
		report = MODULE.evaluate(self.policy, evidence, self.sha, self.now)
		self.assertEqual("BLOCKED", report["status"])

	def test_future_evidence_timestamp_is_blocked(self) -> None:
		evidence = self.complete_evidence()
		payload = json.loads(evidence["production_operations"].read_text(encoding="utf-8"))
		payload["generated_at"] = (self.now + timedelta(minutes=1)).isoformat()
		evidence["production_operations"] = self.write("future", payload)
		report = MODULE.evaluate(self.policy, evidence, self.sha, self.now)
		self.assertEqual("BLOCKED", report["status"])

	def test_artifact_link_mismatch_is_blocked(self) -> None:
		evidence = self.complete_evidence()
		payload = json.loads(evidence["production_operations"].read_text(encoding="utf-8"))
		payload["rc_package"]["sha256"] = "0" * 64
		evidence["production_operations"] = self.write("link-mismatch", payload)
		report = MODULE.evaluate(self.policy, evidence, self.sha, self.now)
		self.assertEqual("BLOCKED", report["status"])

	def test_ci_rehearsal_never_satisfies_target_acceptance(self) -> None:
		target = {
			"schema_version": 1,
			"status": "ACCEPTED",
			"git_sha": self.sha,
			"environment_class": "ci_rehearsal",
			"validated_at_utc": self.now.isoformat(),
			"artifact_sha256": "d" * 64,
			"target_environment": "example",
			"operator": "operator",
			"observation_window": {
				"start_utc": (self.now - timedelta(minutes=30)).isoformat(),
				"end_utc": self.now.isoformat(),
			},
		}
		report = MODULE.evaluate(
			self.policy,
			self.complete_evidence(),
			self.sha,
			self.now,
			self.write("target-invalid", target),
		)
		self.assertEqual("BLOCKED", report["status"])

	def test_target_acceptance_must_match_rc_binary(self) -> None:
		target = {
			"schema_version": 1,
			"status": "ACCEPTED",
			"git_sha": self.sha,
			"environment_class": "target_environment",
			"validated_at_utc": self.now.isoformat(),
			"artifact_sha256": "e" * 64,
			"target_environment": "staging-like-target",
			"operator": "operator",
			"observation_window": {
				"start_utc": (self.now - timedelta(minutes=30)).isoformat(),
				"end_utc": self.now.isoformat(),
			},
		}
		report = MODULE.evaluate(
			self.policy,
			self.complete_evidence(),
			self.sha,
			self.now,
			self.write("target-mismatched-artifact", target),
		)
		self.assertEqual("BLOCKED", report["status"])

	def test_reversed_target_observation_window_is_blocked(self) -> None:
		target = {
			"schema_version": 1,
			"status": "ACCEPTED",
			"git_sha": self.sha,
			"environment_class": "target_environment",
			"validated_at_utc": self.now.isoformat(),
			"artifact_sha256": "d" * 64,
			"target_environment": "staging-like-target",
			"operator": "operator",
			"observation_window": {
				"start_utc": self.now.isoformat(),
				"end_utc": (self.now - timedelta(minutes=30)).isoformat(),
			},
		}
		report = MODULE.evaluate(
			self.policy,
			self.complete_evidence(),
			self.sha,
			self.now,
			self.write("target-reversed-window", target),
		)
		self.assertEqual("BLOCKED", report["status"])

	def test_valid_target_acceptance_is_distinct_state(self) -> None:
		target = {
			"schema_version": 1,
			"status": "ACCEPTED",
			"git_sha": self.sha,
			"environment_class": "target_environment",
			"validated_at_utc": self.now.isoformat(),
			"artifact_sha256": "d" * 64,
			"target_environment": "staging-like-target",
			"operator": "operator",
			"observation_window": {
				"start_utc": (self.now - timedelta(minutes=30)).isoformat(),
				"end_utc": self.now.isoformat(),
			},
		}
		report = MODULE.evaluate(
			self.policy,
			self.complete_evidence(),
			self.sha,
			self.now,
			self.write("target-valid", target),
		)
		self.assertEqual("TARGET_ACCEPTED", report["status"])


if __name__ == "__main__":
	unittest.main()
