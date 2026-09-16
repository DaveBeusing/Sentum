#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import json
import os
import platform
import statistics
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


class GateFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class CommandResult:
    command: list[str]
    stdout: str
    stderr: str
    returncode: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run Sentum microbenchmarks and enforce repository performance budgets."
    )
    parser.add_argument("--build-dir", default="build", help="Directory containing benchmark executables")
    parser.add_argument(
        "--budget-file",
        default="benchmarks/performance_budgets.json",
        help="JSON budget definition",
    )
    parser.add_argument("--report", default="log/performance_gate.json", help="Machine-readable evidence output")
    parser.add_argument("--summary", default="log/performance_gate.md", help="Markdown evidence summary")
    parser.add_argument(
        "--repetitions",
        type=int,
        default=None,
        help="Override configured market benchmark repetitions",
    )
    return parser.parse_args()


def run(command: list[str]) -> CommandResult:
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    return CommandResult(command, completed.stdout, completed.stderr, completed.returncode)


def parse_key_value(output: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def require_float(values: dict[str, str], key: str) -> float:
    if key not in values:
        raise GateFailure(f"benchmark output is missing '{key}'")
    try:
        return float(values[key])
    except ValueError as error:
        raise GateFailure(f"benchmark output '{key}' is not numeric: {values[key]!r}") from error


def require_int(values: dict[str, str], key: str) -> int:
    if key not in values:
        raise GateFailure(f"benchmark output is missing '{key}'")
    try:
        return int(values[key])
    except ValueError as error:
        raise GateFailure(f"benchmark output '{key}' is not an integer: {values[key]!r}") from error


def cpu_model() -> str:
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text(errors="replace").splitlines():
            if line.lower().startswith("model name") and ":" in line:
                return line.split(":", 1)[1].strip()
    return platform.processor() or "unknown"


def market_case(
    executable: Path,
    case: dict[str, Any],
    repetitions: int,
) -> tuple[dict[str, Any], list[str]]:
    name = str(case["name"])
    symbols = int(case["symbols"])
    events_per_symbol = int(case["events_per_symbol"])
    expected_events = symbols * events_per_symbol
    max_median = float(case["max_median_ns_per_event"])
    max_worst = float(case["max_worst_ns_per_event"])

    samples: list[dict[str, Any]] = []
    violations: list[str] = []

    for iteration in range(repetitions):
        result = run([str(executable), str(symbols), str(events_per_symbol)])
        if result.returncode != 0:
            raise GateFailure(
                f"market benchmark {name} repetition {iteration + 1} exited {result.returncode}: "
                f"{result.stderr.strip() or result.stdout.strip()}"
            )
        values = parse_key_value(result.stdout)
        events = require_int(values, "events")
        delivered = require_int(values, "delivered_events")
        ns_per_event = require_float(values, "nanoseconds_per_event")
        events_per_second = require_float(values, "events_per_second")
        seconds = require_float(values, "seconds")

        if events != expected_events:
            violations.append(f"{name}: expected {expected_events} events, benchmark reported {events}")
        if delivered != expected_events:
            violations.append(f"{name}: expected {expected_events} delivered events, got {delivered}")
        if ns_per_event <= 0.0 or events_per_second <= 0.0 or seconds <= 0.0:
            violations.append(f"{name}: benchmark reported a non-positive timing measurement")

        samples.append(
            {
                "iteration": iteration + 1,
                "nanoseconds_per_event": ns_per_event,
                "events_per_second": events_per_second,
                "seconds": seconds,
                "events": events,
                "delivered_events": delivered,
            }
        )

    ns_samples = [sample["nanoseconds_per_event"] for sample in samples]
    eps_samples = [sample["events_per_second"] for sample in samples]
    median_ns = float(statistics.median(ns_samples))
    worst_ns = float(max(ns_samples))
    median_eps = float(statistics.median(eps_samples))

    if median_ns > max_median:
        violations.append(
            f"{name}: median {median_ns:.2f} ns/event exceeds budget {max_median:.2f} ns/event"
        )
    if worst_ns > max_worst:
        violations.append(
            f"{name}: worst sample {worst_ns:.2f} ns/event exceeds guardrail {max_worst:.2f} ns/event"
        )

    return (
        {
            "name": name,
            "symbols": symbols,
            "events_per_symbol": events_per_symbol,
            "expected_events": expected_events,
            "repetitions": repetitions,
            "median_ns_per_event": median_ns,
            "worst_ns_per_event": worst_ns,
            "median_events_per_second": median_eps,
            "budget": {
                "max_median_ns_per_event": max_median,
                "max_worst_ns_per_event": max_worst,
            },
            "samples": samples,
            "status": "PASS" if not violations else "FAIL",
        },
        violations,
    )


def parser_case(executable: Path, budget: dict[str, Any]) -> tuple[dict[str, Any], list[str]]:
    result = run([str(executable)])
    if result.returncode not in (0, 1):
        raise GateFailure(
            f"parser allocation benchmark exited {result.returncode}: "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )

    values = parse_key_value(result.stdout)
    iterations = require_int(values, "iterations")
    allocations = require_int(values, "allocations")
    allocations_per_parse = require_float(values, "allocations_per_parse")
    maximum = float(budget["max_allocations_per_parse"])
    violations: list[str] = []

    if iterations <= 0:
        violations.append("parser: benchmark reported no iterations")
    if allocations_per_parse > maximum:
        violations.append(
            f"parser: {allocations_per_parse:.6f} allocations/parse exceeds budget {maximum:.6f}"
        )
    if result.returncode != 0:
        violations.append(f"parser: benchmark exited {result.returncode}")

    return (
        {
            "iterations": iterations,
            "allocations": allocations,
            "allocations_per_parse": allocations_per_parse,
            "budget": {"max_allocations_per_parse": maximum},
            "status": "PASS" if not violations else "FAIL",
        },
        violations,
    )


def write_summary(path: Path, report: dict[str, Any]) -> None:
    lines = [
        "# Sentum performance gate",
        "",
        f"**Status:** {report['status']}",
        "",
        "| Workload | Median ns/event | Worst ns/event | Median events/s | Median budget | Worst guardrail | Status |",
        "| --- | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for case in report["market_path"]:
        lines.append(
            "| {name} | {median:.2f} | {worst:.2f} | {eps:.0f} | {median_budget:.2f} | {worst_budget:.2f} | {status} |".format(
                name=case["name"],
                median=case["median_ns_per_event"],
                worst=case["worst_ns_per_event"],
                eps=case["median_events_per_second"],
                median_budget=case["budget"]["max_median_ns_per_event"],
                worst_budget=case["budget"]["max_worst_ns_per_event"],
                status=case["status"],
            )
        )
    parser = report["parser"]
    lines.extend(
        [
            "",
            f"Parser allocations/parse: **{parser['allocations_per_parse']:.6f}** "
            f"(budget <= {parser['budget']['max_allocations_per_parse']:.6f}) — **{parser['status']}**",
        ]
    )
    if report["violations"]:
        lines.extend(["", "## Violations"])
        lines.extend(f"- {violation}" for violation in report["violations"])
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))


def main() -> int:
    args = parse_args()
    budget_path = Path(args.budget_file)
    build_dir = Path(args.build_dir)
    report_path = Path(args.report)
    summary_path = Path(args.summary)

    try:
        budgets = json.loads(budget_path.read_text())
        if int(budgets.get("schema_version", 0)) != 1:
            raise GateFailure("unsupported performance budget schema version")

        configured_repetitions = int(budgets["measurement_policy"]["repetitions"])
        repetitions = args.repetitions if args.repetitions is not None else configured_repetitions
        if repetitions < 3:
            raise GateFailure("performance gates require at least three repetitions")

        market_executable = build_dir / "sentum_market_benchmark"
        parser_executable = build_dir / "sentum_parser_allocation_benchmark"
        for executable in (market_executable, parser_executable):
            if not executable.is_file():
                raise GateFailure(f"benchmark executable not found: {executable}")

        market_results: list[dict[str, Any]] = []
        violations: list[str] = []
        for case in budgets["market_path"]:
            result, case_violations = market_case(market_executable, case, repetitions)
            market_results.append(result)
            violations.extend(case_violations)

        parser_result, parser_violations = parser_case(parser_executable, budgets["parser"])
        violations.extend(parser_violations)

        report: dict[str, Any] = {
            "schema_version": 1,
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "status": "PASS" if not violations else "FAIL",
            "git_sha": os.environ.get("GITHUB_SHA", "unknown"),
            "workflow_run_id": os.environ.get("GITHUB_RUN_ID", "unknown"),
            "environment": {
                "os": platform.platform(),
                "machine": platform.machine(),
                "cpu_model": cpu_model(),
                "cpu_count": os.cpu_count(),
                "python": platform.python_version(),
            },
            "budget_source": str(budget_path),
            "measurement_policy": {
                "repetitions": repetitions,
                "market_gate_statistic": budgets["measurement_policy"]["market_gate_statistic"],
                "market_guardrail_statistic": budgets["measurement_policy"]["market_guardrail_statistic"],
            },
            "baseline_evidence": budgets.get("baseline_evidence", {}),
            "market_path": market_results,
            "parser": parser_result,
            "violations": violations,
        }

        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        write_summary(summary_path, report)
        print(summary_path.read_text(), end="")
        return 0 if report["status"] == "PASS" else 1
    except (GateFailure, KeyError, TypeError, ValueError, json.JSONDecodeError, OSError) as error:
        print(f"performance gate configuration/execution failure: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
