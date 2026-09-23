#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import copy
import importlib.util
import json
import tempfile
import unittest
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "ci" / "performance_gate.py"
SPEC = importlib.util.spec_from_file_location("sentum_performance_gate", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot load performance_gate.py")
performance_gate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = performance_gate
SPEC.loader.exec_module(performance_gate)


def valid_budgets():
    return {
        "schema_version": 2,
        "measurement_policy": {
            "repetitions": 5,
            "market_gate_statistic": "median",
            "market_guardrail_statistic": "worst",
        },
        "market_path": {
            "timing_state": "ENFORCED",
            "correctness_state": "ENFORCED",
            "cases": [
                {
                    "name": "case",
                    "symbols": 1,
                    "events_per_symbol": 3,
                    "max_median_ns_per_event": 10.0,
                    "max_worst_ns_per_event": 20.0,
                }
            ],
        },
        "parser": {
            "allocation_state": "ENFORCED",
            "max_allocations_per_parse": 0.0,
        },
        "scanner_hot_path": {
            "timing_state": "ENFORCED",
            "correctness_state": "ENFORCED",
            "repetitions": 5,
            "cases": [
                {
                    "name": "case",
                    "symbols": 1,
                    "events_per_symbol": 3,
                    "expected_top_count": 1,
                    "max_median_ns_per_event": 10.0,
                    "max_worst_ns_per_event": 20.0,
                }
            ],
        },
        "dashboard_snapshot": {
            "timing_state": "OBSERVED",
            "correctness_state": "ENFORCED",
            "iterations": 10,
            "max_conditional_snapshot_copies": 0,
        },
        "terminal_render_pipeline": {
            "timing_state": "ENFORCED",
            "correctness_state": "ENFORCED",
            "repetitions": 5,
            "iterations": 10,
            "max_unchanged_payload_bytes": 0,
            "max_median_unchanged_ns_per_frame": 10.0,
            "max_worst_unchanged_ns_per_frame": 20.0,
            "max_median_changed_ns_per_frame": 30.0,
            "max_worst_changed_ns_per_frame": 40.0,
        },
        "runtime_qualification": {
            "memory_state": "OBSERVED",
            "sampled_latency_state": "OBSERVED",
        },
    }


class PerformanceGateConfigurationTests(unittest.TestCase):
    def test_malformed_budget_file_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "budgets.json"
            path.write_text("{not-json", encoding="utf-8")
            with self.assertRaises(performance_gate.GateFailure):
                performance_gate.load_budgets(path)

    def test_missing_metric_section_fails_closed(self):
        budgets = valid_budgets()
        del budgets["scanner_hot_path"]
        with self.assertRaises(performance_gate.GateFailure):
            performance_gate.validate_budgets(budgets)

    def test_wrong_schema_version_fails_closed(self):
        budgets = valid_budgets()
        budgets["schema_version"] = 1
        with self.assertRaises(performance_gate.GateFailure):
            performance_gate.validate_budgets(budgets)

    def test_missing_benchmark_output_metric_fails_closed(self):
        with self.assertRaises(performance_gate.GateFailure):
            performance_gate.require_float({}, "nanoseconds_per_event")

    def test_round_trip_valid_budget_file(self):
        budgets = valid_budgets()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "budgets.json"
            path.write_text(json.dumps(budgets), encoding="utf-8")
            self.assertEqual(performance_gate.load_budgets(path)["schema_version"], 2)


class PerformanceGateEvaluatorTests(unittest.TestCase):
    def test_median_violation_is_enforced(self):
        violations = []
        performance_gate.evaluate_upper_bound(
            violations, "scanner median", 11.0, 10.0, "ENFORCED", "ns/event"
        )
        self.assertEqual(len(violations), 1)
        self.assertIn("scanner median", violations[0])

    def test_worst_sample_violation_is_enforced(self):
        violations = []
        performance_gate.evaluate_upper_bound(
            violations, "scanner worst sample", 21.0, 20.0, "ENFORCED", "ns/event"
        )
        self.assertEqual(len(violations), 1)
        self.assertIn("worst sample", violations[0])

    def test_correctness_violation_fails_even_when_timing_passes(self):
        violations = []
        performance_gate.evaluate_upper_bound(
            violations, "terminal timing", 9.0, 10.0, "ENFORCED", "ns/frame"
        )
        performance_gate.evaluate_upper_bound(
            violations, "terminal unchanged payload", 1.0, 0.0, "ENFORCED", "bytes"
        )
        self.assertEqual(len(violations), 1)
        self.assertIn("unchanged payload", violations[0])

    def test_observed_metric_is_not_accidentally_enforced(self):
        violations = []
        performance_gate.evaluate_upper_bound(
            violations, "dashboard timing", 1000.0, 1.0, "OBSERVED", "ns/poll"
        )
        self.assertEqual(violations, [])

    def test_threshold_boundary_value_passes(self):
        violations = []
        performance_gate.evaluate_upper_bound(
            violations, "boundary", 10.0, 10.0, "ENFORCED", "ns/event"
        )
        self.assertEqual(violations, [])

    def test_invalid_evidence_state_is_rejected(self):
        budgets = copy.deepcopy(valid_budgets())
        budgets["dashboard_snapshot"]["timing_state"] = "UNKNOWN"
        with self.assertRaises(performance_gate.GateFailure):
            performance_gate.validate_budgets(budgets)


if __name__ == "__main__":
    unittest.main()
