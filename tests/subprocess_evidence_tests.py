#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


def load_module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / "ci" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


QUALIFICATION = load_module("runtime_qualification")
ACCEPTANCE = load_module("operational_acceptance")


class SubprocessEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def fixture(self, name, source):
        path = self.directory / name
        path.write_text(f"#!{sys.executable}\n{source}\n", encoding="utf-8")
        path.chmod(0o700)
        return path

    def run_qualification(self, source, timeout=3, limit=QUALIFICATION.MAX_OUTPUT_BYTES):
        return QUALIFICATION.monitor_command(
            [sys.executable, "-c", source], timeout, 0.02, max_output_bytes=limit,
        )

    def test_drains_large_stdout_and_stderr_without_blocking(self):
        result = self.run_qualification(
            "import sys; "
            "sys.stdout.write('o' * 262144); sys.stdout.flush(); "
            "sys.stderr.write('e' * 262144); sys.stderr.flush()"
        )
        self.assertEqual(0, result["returncode"])
        self.assertFalse(result["timed_out"])
        self.assertFalse(result["output_limit_exceeded"])
        self.assertEqual("o" * 262144, result["stdout"])
        self.assertEqual("e" * 262144, result["stderr"])

    def test_preserves_large_complete_scenario_json(self):
        result = self.run_qualification(
            "import json; print(json.dumps({'pass': True, 'payload': 'x' * 262144}))"
        )
        self.assertEqual(0, result["returncode"])
        evidence = QUALIFICATION.parse_json_line(result["stdout"])
        self.assertTrue(evidence["pass"])
        self.assertEqual(262144, len(evidence["payload"]))

    def test_output_at_limit_is_complete(self):
        result = self.run_qualification("import sys; sys.stdout.write('x' * 4096)", limit=4096)
        self.assertEqual(0, result["returncode"])
        self.assertEqual(4096, len(result["stdout"]))
        self.assertFalse(result["output_limit_exceeded"])

    def test_excess_output_is_bounded_and_fails(self):
        for stream in ("stdout", "stderr"):
            with self.subTest(stream=stream):
                result = self.run_qualification(
                    f"import sys; sys.{stream}.write('x' * 262144); sys.{stream}.flush()",
                    limit=4096,
                )
                self.assertEqual(125, result["returncode"])
                self.assertTrue(result["output_limit_exceeded"])
                self.assertEqual(4096, len(result[stream]))
                self.assertGreater(result["output_bytes"][stream], 4096)

    def test_timeout_preserves_diagnostics(self):
        result = self.run_qualification(
            "import sys, time; print('started', flush=True); "
            "print('waiting', file=sys.stderr, flush=True); time.sleep(30)",
            timeout=0.2,
        )
        self.assertEqual(124, result["returncode"])
        self.assertTrue(result["timed_out"])
        self.assertEqual("started\n", result["stdout"])
        self.assertEqual("waiting\n", result["stderr"])
        self.assertLess(result["elapsed_seconds"], 4)

    def test_ignored_termination_is_killed_within_bound(self):
        result = self.run_qualification(
            "import signal, time; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
            "print('ready', flush=True); time.sleep(30)",
            timeout=0.2,
        )
        self.assertEqual(124, result["returncode"])
        self.assertEqual("ready\n", result["stdout"])
        self.assertLess(result["elapsed_seconds"], 5)

    def test_inherited_pipes_do_not_escape_timeout(self):
        result = self.run_qualification(
            "import os, time\n"
            "pid = os.fork()\n"
            "if pid:\n"
            " print(pid, flush=True)\n"
            " os._exit(0)\n"
            "time.sleep(30)\n",
            timeout=0.2,
        )
        self.assertEqual(124, result["returncode"])
        self.assertLess(result["elapsed_seconds"], 4)
        self.assert_process_stopped(int(result["stdout"].strip()))

    def assert_process_stopped(self, pid):
        # A reparented child may briefly remain a zombie until the host reaps it.
        for _ in range(50):
            status = Path(f"/proc/{pid}/status")
            if not status.exists():
                return
            try:
                if any(line.startswith("State:") and "Z" in line for line in status.read_text().splitlines()):
                    return
            except FileNotFoundError:
                return
            time.sleep(0.01)
        self.fail(f"owned descendant {pid} is still running")

    def test_sampling_continues_while_both_streams_are_busy(self):
        # Host /proc visibility is optional; exercise scheduling with a known sample.
        with patch.object(QUALIFICATION, "read_process_status", return_value=(1024, 1)):
            result = self.run_qualification(
                "import sys, time\n"
                "for _ in range(6):\n"
                " sys.stdout.write('x' * 65536); sys.stdout.flush()\n"
                " sys.stderr.write('y' * 65536); sys.stderr.flush()\n"
                " time.sleep(0.04)\n"
            )
        self.assertEqual(0, result["returncode"])
        self.assertGreaterEqual(len(result["rss_samples"]), 2)

    def test_invalid_utf8_is_retained_as_replacement_text(self):
        result = self.run_qualification("import os; os.write(1, b'out\\xff'); os.write(2, b'err\\xfe')")
        self.assertEqual(0, result["returncode"])
        self.assertEqual("out\ufffd", result["stdout"])
        self.assertEqual("err\ufffd", result["stderr"])
        json.dumps(result, allow_nan=False)

    def test_nonfinite_monitor_time_limits_are_rejected(self):
        for value in (float("nan"), float("inf"), float("-inf")):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    QUALIFICATION.monitor_command([sys.executable, "-c", "pass"], value, 0.02)
                with self.assertRaises(ValueError):
                    QUALIFICATION.monitor_command([sys.executable, "-c", "pass"], 1, value)

    def test_cli_output_overflow_never_accepts_partial_evidence(self):
        self.fixture(
            "sentum_runtime_qualification_scenarios",
            "import sys; print('{\"pass\": true}', flush=True); "
            "sys.stderr.write('x' * 262144); sys.stderr.flush()",
        )
        report_path = self.directory / "qualification.json"
        summary_path = self.directory / "qualification.md"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/ci/runtime_qualification.py"),
             "--build-dir", str(self.directory), "--scenario", "paper-soak",
             "--max-output-bytes", "4096", "--timeout-seconds", "3",
             "--output", str(report_path), "--summary", str(summary_path)],
            capture_output=True, text=True, timeout=10,
            env={**os.environ, "GITHUB_SHA": "a" * 40},
        )
        self.assertEqual(1, result.returncode, result.stderr)
        report = json.loads(report_path.read_text())
        self.assertEqual("FAIL", report["status"])
        self.assertFalse(report["evidence_complete"])
        self.assertTrue(report["commands"][0]["output_limit_exceeded"])
        self.assertIn("output limit", summary_path.read_text())

    def test_acceptance_timeouts_always_produce_text(self):
        cases = (
            ("import time; time.sleep(30)", "", ""),
            ("import os, time; os.write(1, b'started\\xff'); time.sleep(30)", "started\ufffd", ""),
            ("import os, time; os.write(2, b'waiting\\xfe'); time.sleep(30)", "", "waiting\ufffd"),
        )
        for source, stdout, stderr in cases:
            with self.subTest(source=source):
                result = ACCEPTANCE.run_cycle([sys.executable, "-c", source], 0.2)
                self.assertEqual(124, result["returncode"])
                self.assertTrue(result["timed_out"])
                self.assertEqual(stdout, result["stdout"])
                self.assertEqual(stderr, result["stderr"])
                self.assertGreaterEqual(result["elapsed_seconds"], 0.2)
                self.assertLess(result["elapsed_seconds"], 3)
                json.dumps(result, allow_nan=False)

    def test_acceptance_timeout_stops_inherited_pipe_owners(self):
        result = ACCEPTANCE.run_cycle(
            [sys.executable, "-c",
             "import os, time\npid = os.fork()\nif pid:\n print(pid, flush=True)\n os._exit(0)\ntime.sleep(30)"],
            0.2,
        )
        self.assertEqual(124, result["returncode"])
        self.assertLess(result["elapsed_seconds"], 3)
        self.assert_process_stopped(int(result["stdout"].strip()))

    def test_acceptance_cli_persists_timeout_json_and_summary(self):
        lifecycle = self.fixture(
            "sentum_runtime_lifecycle_tests",
            "import os, time; os.write(1, b'started\\xff\\n'); os.write(2, b'waiting\\xfe\\n'); time.sleep(30)",
        )
        self.fixture("sentum_runtime_observability_tests", "print('healthy')")
        report_path = self.directory / "acceptance.json"
        summary_path = self.directory / "acceptance.md"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/ci/operational_acceptance.py"),
             "--build-dir", str(self.directory), "--cycles", "3", "--timeout-seconds", "1",
             "--report", str(report_path), "--summary", str(summary_path)],
            capture_output=True, text=True, timeout=8,
        )
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        report = json.loads(report_path.read_text())
        self.assertEqual("FAIL", report["status"])
        self.assertEqual(1, report["cycles_completed"])
        check = report["results"][0]["checks"][0]
        self.assertEqual([str(lifecycle)], check["command"])
        self.assertEqual(124, check["returncode"])
        self.assertTrue(check["timed_out"])
        self.assertEqual("started\ufffd\n", check["stdout"])
        self.assertEqual("waiting\ufffd\n", check["stderr"])
        summary = summary_path.read_text()
        self.assertIn("Status: **FAIL**", summary)
        self.assertIn("Elapsed:", summary)
        self.assertIn("started\ufffd", summary)
        self.assertIn("waiting\ufffd", summary)
        self.assertIn(str(lifecycle), summary)

    def test_acceptance_success_retains_normal_output(self):
        result = ACCEPTANCE.run_cycle([sys.executable, "-c", "print('success')"], 2)
        self.assertEqual(0, result["returncode"])
        self.assertFalse(result["timed_out"])
        self.assertEqual("success\n", result["stdout"])
        self.assertEqual("", result["stderr"])

    def test_acceptance_start_failure_is_structured(self):
        result = ACCEPTANCE.run_cycle([str(self.directory / "missing")], 1)
        self.assertEqual(127, result["returncode"])
        self.assertFalse(result["timed_out"])
        self.assertIn("could not be started", result["failure"])
        json.dumps(result, allow_nan=False)


if __name__ == "__main__":
    unittest.main()
