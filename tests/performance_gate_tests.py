#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import copy
import importlib.util
import json
import subprocess
import tempfile
import unittest
import sys
from pathlib import Path
from unittest.mock import patch


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
    def test_nonfinite_and_boolean_numeric_budgets_fail_closed(self):
        paths = [
            ("market_path", "cases", 0, "max_median_ns_per_event"),
            ("market_path", "cases", 0, "max_worst_ns_per_event"),
            ("parser", "max_allocations_per_parse"),
            ("scanner_hot_path", "cases", 0, "max_median_ns_per_event"),
            ("scanner_hot_path", "cases", 0, "max_worst_ns_per_event"),
            ("dashboard_snapshot", "max_conditional_snapshot_copies"),
            ("terminal_render_pipeline", "max_unchanged_payload_bytes"),
            *(("terminal_render_pipeline", key) for key in (
                "max_median_unchanged_ns_per_frame", "max_worst_unchanged_ns_per_frame",
                "max_median_changed_ns_per_frame", "max_worst_changed_ns_per_frame")),
        ]
        for path in paths:
            for invalid in (float("nan"), float("inf"), float("-inf"), True, -1):
                with self.subTest(path=path, value=invalid):
                    budgets = valid_budgets()
                    container = budgets
                    for key in path[:-1]:
                        container = container[key]
                    container[path[-1]] = invalid
                    with self.assertRaises(performance_gate.GateFailure):
                        performance_gate.validate_budgets(budgets)

    def test_fractional_count_budgets_cannot_be_silently_truncated(self):
        for section, key in (("dashboard_snapshot", "max_conditional_snapshot_copies"),
                             ("terminal_render_pipeline", "max_unchanged_payload_bytes")):
            budgets = valid_budgets()
            budgets[section][key] = 0.5
            with self.subTest(section=section), self.assertRaises(performance_gate.GateFailure):
                performance_gate.validate_budgets(budgets)

    def test_nonfinite_copied_baseline_evidence_fails_closed(self):
        budgets = valid_budgets()
        budgets["baseline_evidence"] = {"observed": [float("nan")]}
        with self.assertRaises(performance_gate.GateFailure):
            performance_gate.validate_budgets(budgets)

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
    def test_invalid_upper_bound_operands_fail_even_for_observed_metrics(self):
        for state in ("ENFORCED", "OBSERVED"):
            for value in (float("nan"), float("inf"), float("-inf"), -1, True):
                for measured, maximum in ((value, 10.0), (1.0, value)):
                    with self.subTest(state=state, value=value, maximum=maximum):
                        with self.assertRaises(performance_gate.GateFailure):
                            performance_gate.evaluate_upper_bound([], "metric", measured, maximum, state, "unit")

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


class BenchmarkMeasurementTests(unittest.TestCase):
    def setUp(self):
        self.budgets = valid_budgets()
        self.outputs = {
            "market": {"events": 3, "delivered_events": 3, "nanoseconds_per_event": 5,
                       "events_per_second": 200000000, "seconds": 0.01},
            "scanner": {"symbols": 1, "events": 3, "top_count": 1, "top_changes": 1,
                        "nanoseconds_per_event": 5, "events_per_second": 200000000, "seconds": 0.01},
            "parser": {"iterations": 3, "allocations": 0, "allocations_per_parse": 0},
            "dashboard": {"iterations": 10, "unconditional_ns_per_poll": 20,
                          "unchanged_conditional_ns_per_poll": 1, "conditional_snapshot_copies": 0,
                          "copied_payload_bytes": 100},
            "terminal": {"iterations": 10, "unchanged_ns_per_frame": 5, "unchanged_payload_bytes": 0,
                         "changed_ns_per_frame": 25, "changed_payload_bytes": 100},
        }

    def evaluate(self, name, values):
        result = performance_gate.CommandResult([name], "\n".join(f"{key}={value}" for key, value in values.items()), "", 0)
        executable = Path(name)
        with patch.object(performance_gate, "run", return_value=result):
            if name == "market":
                return performance_gate.market_case(executable, self.budgets["market_path"]["cases"][0], 3, "ENFORCED", "ENFORCED")
            if name == "scanner":
                return performance_gate.scanner_case(executable, self.budgets["scanner_hot_path"]["cases"][0], 3, "ENFORCED", "ENFORCED")
            if name == "parser":
                return performance_gate.parser_case(executable, self.budgets["parser"])
            if name == "dashboard":
                return performance_gate.dashboard_case(executable, self.budgets["dashboard_snapshot"])
            return performance_gate.terminal_case(executable, self.budgets["terminal_render_pipeline"], 3)

    def test_valid_measurements_pass_for_every_benchmark(self):
        for name, output in self.outputs.items():
            with self.subTest(name=name):
                report, violations = self.evaluate(name, output)
                self.assertEqual(report["status"], "PASS")
                self.assertEqual(violations, [])

    def test_every_measurement_rejects_nonfinite_negative_and_boolean_values(self):
        for name, output in self.outputs.items():
            for field in output:
                for invalid in ("nan", "inf", "-inf", "-1", "true"):
                    with self.subTest(name=name, field=field, invalid=invalid):
                        with self.assertRaises(performance_gate.GateFailure):
                            self.evaluate(name, {**output, field: invalid})

    def test_count_measurements_reject_fractional_values(self):
        fields = {"market": ("events", "delivered_events"),
                  "scanner": ("symbols", "events", "top_count", "top_changes"),
                  "parser": ("iterations", "allocations"),
                  "dashboard": ("iterations", "conditional_snapshot_copies", "copied_payload_bytes"),
                  "terminal": ("iterations", "unchanged_payload_bytes", "changed_payload_bytes")}
        for name, names in fields.items():
            for field in names:
                with self.subTest(name=name, field=field), self.assertRaises(performance_gate.GateFailure):
                    self.evaluate(name, {**self.outputs[name], field: "0.5"})

    def test_positive_measurements_cannot_be_zero(self):
        fields = {"market": ("events", "nanoseconds_per_event", "events_per_second", "seconds"),
                  "scanner": ("symbols", "events", "nanoseconds_per_event", "events_per_second", "seconds"),
                  "parser": ("iterations",),
                  "dashboard": ("iterations", "unconditional_ns_per_poll", "unchanged_conditional_ns_per_poll"),
                  "terminal": ("iterations", "unchanged_ns_per_frame", "changed_ns_per_frame")}
        for name, names in fields.items():
            for field in names:
                with self.subTest(name=name, field=field), self.assertRaises(performance_gate.GateFailure):
                    self.evaluate(name, {**self.outputs[name], field: 0})

    def test_inconsistent_counts_and_allocation_ratio_are_rejected(self):
        for name, changes in (("market", {"delivered_events": 4}), ("scanner", {"top_count": 2}),
                              ("scanner", {"top_changes": 4}), ("parser", {"allocations": 1}),
                              ("parser", {"allocations_per_parse": 1e-13}),
                              ("dashboard", {"conditional_snapshot_copies": 11})):
            with self.subTest(name=name, changes=changes), self.assertRaises(performance_gate.GateFailure):
                self.evaluate(name, {**self.outputs[name], **changes})

    def test_wrong_workload_counts_fail_correctness_gate(self):
        for name, changes in (("market", {"events": 4}), ("scanner", {"events": 4}),
                              ("dashboard", {"iterations": 11}), ("terminal", {"iterations": 11})):
            with self.subTest(name=name):
                report, violations = self.evaluate(name, {**self.outputs[name], **changes})
                self.assertEqual(report["status"], "FAIL")
                self.assertTrue(violations)

    def test_parser_ratio_tolerates_only_output_rounding(self):
        self.budgets["parser"]["max_allocations_per_parse"] = 1
        report, _ = self.evaluate("parser", {"iterations": 3, "allocations": 1, "allocations_per_parse": "0.333333"})
        self.assertEqual(report["status"], "PASS")

    def test_real_cli_retains_strict_failure_json_for_nan_output_and_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            names = {"market": "sentum_market_benchmark", "parser": "sentum_parser_allocation_benchmark",
                     "scanner": "sentum_scanner_hot_path_benchmark", "dashboard": "sentum_dashboard_snapshot_benchmark",
                     "terminal": "sentum_terminal_render_pipeline_benchmark"}
            for name, filename in names.items():
                payload = "\n".join(f"{key}={value}" for key, value in self.outputs[name].items())
                path = root / filename
                path.write_text(f"#!{sys.executable}\nprint({payload!r})\n", encoding="utf-8")
                path.chmod(0o755)
            budgets = root / "budgets.json"
            budgets.write_text(json.dumps(self.budgets), encoding="utf-8")
            command = [sys.executable, str(MODULE_PATH), "--build-dir", str(root), "--budget-file", str(budgets),
                       "--report", str(root / "report.json"), "--summary", str(root / "report.md")]
            positive = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(positive.returncode, 0, positive.stderr)
            path = root / names["market"]
            path.write_text(path.read_text().replace("nanoseconds_per_event=5", "nanoseconds_per_event=nan"))
            for invalid_budget in (False, True):
                if invalid_budget:
                    self.budgets["parser"]["max_allocations_per_parse"] = float("nan")
                    budgets.write_text(json.dumps(self.budgets))
                result = subprocess.run(command, capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode, 2, result.stderr)
                report = json.loads((root / "report.json").read_text(),
                                    parse_constant=lambda value: self.fail(f"nonstandard JSON {value}"))
                self.assertEqual(report["status"], "FAIL")
                self.assertTrue(report["violations"])
                self.assertIn("FAIL", (root / "report.md").read_text())
                self.assertNotIn("Traceback", result.stderr)


class RuntimeObservationTests(unittest.TestCase):
    def fixture(self):
        return {"schema_version": 1, "scenario": "paper-soak", "status": "PASS", "evidence_complete": True,
                "actual_duration_seconds": 1.0,
                "memory": {"available": True, "starting_rss_kib": 10, "peak_rss_kib": 12,
                           "ending_rss_kib": 9, "growth_kib": -1, "trend_slope_kib_per_minute": -1.5,
                           "unbounded_growth_detected": False, "guardrail_kib": 64, "samples": 2},
                "metrics": {"latency": {"queue_pressure": "normal", "queue_depth": 0,
                                        "parse_latency": {"count": 2, "avg_us": 1.5, "p99_us": 2, "max_us": 2}}}}

    def evaluate(self, value):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime.json"
            path.write_text(json.dumps(value), encoding="utf-8")
            return performance_gate.qualification_evidence([str(path)], valid_budgets()["runtime_qualification"])

    def test_finite_signed_memory_growth_is_valid_observed_evidence(self):
        self.assertEqual(self.evaluate(self.fixture())[0]["memory"]["growth_kib"], -1)

    def test_invalid_runtime_observation_numbers_fail(self):
        for path in (("actual_duration_seconds",), ("memory", "starting_rss_kib"),
                     ("memory", "trend_slope_kib_per_minute"), ("metrics", "latency", "parse_latency", "avg_us")):
            for invalid in (float("nan"), float("inf"), float("-inf"), True):
                report = self.fixture()
                target = report
                for key in path[:-1]:
                    target = target[key]
                target[path[-1]] = invalid
                with self.subTest(path=path, invalid=invalid), self.assertRaises(performance_gate.GateFailure):
                    self.evaluate(report)

    def test_inconsistent_memory_counts_and_latency_domains_fail(self):
        cases = [("memory", "samples", 0), ("memory", "growth_kib", 10), ("memory", "peak_rss_kib", 8)]
        for section, key, value in cases:
            report = self.fixture()
            report[section][key] = value
            with self.subTest(key=key), self.assertRaises(performance_gate.GateFailure):
                self.evaluate(report)
        for key, value in (("count", -1), ("count", 0.5), ("avg_us", -1), ("p99_us", True)):
            report = self.fixture()
            report["metrics"]["latency"]["parse_latency"][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(performance_gate.GateFailure):
                self.evaluate(report)


if __name__ == "__main__":
    unittest.main()
