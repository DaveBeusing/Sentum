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


EVIDENCE_STATES = {"ENFORCED", "OBSERVED"}


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
        help="Override configured repetitions for repeated timing benchmarks",
    )
    parser.add_argument(
        "--qualification-report",
        action="append",
        default=[],
        help="Optional runtime qualification JSON report to include as OBSERVED evidence",
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


def require_mapping(container: dict[str, Any], key: str) -> dict[str, Any]:
    value = container.get(key)
    if not isinstance(value, dict):
        raise GateFailure(f"performance budget is missing object '{key}'")
    return value


def require_list(container: dict[str, Any], key: str) -> list[Any]:
    value = container.get(key)
    if not isinstance(value, list) or not value:
        raise GateFailure(f"performance budget is missing non-empty list '{key}'")
    return value


def require_state(container: dict[str, Any], key: str) -> str:
    value = container.get(key)
    if value not in EVIDENCE_STATES:
        raise GateFailure(
            f"performance budget '{key}' must be one of {sorted(EVIDENCE_STATES)}, got {value!r}"
        )
    return str(value)


def require_positive_int(container: dict[str, Any], key: str) -> int:
    value = container.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise GateFailure(f"performance budget '{key}' must be a positive integer")
    return value


def require_non_negative_number(container: dict[str, Any], key: str) -> float:
    value = container.get(key)
    if not isinstance(value, (int, float)) or isinstance(value, bool) or value < 0:
        raise GateFailure(f"performance budget '{key}' must be a non-negative number")
    return float(value)


def validate_case(case: Any, required_numbers: tuple[str, ...]) -> None:
    if not isinstance(case, dict):
        raise GateFailure("performance budget case must be an object")
    if not isinstance(case.get("name"), str) or not case["name"]:
        raise GateFailure("performance budget case requires a non-empty name")
    for key in ("symbols", "events_per_symbol"):
        require_positive_int(case, key)
    for key in required_numbers:
        require_non_negative_number(case, key)


def validate_budgets(budgets: Any) -> dict[str, Any]:
    if not isinstance(budgets, dict):
        raise GateFailure("performance budget root must be an object")
    if budgets.get("schema_version") != 2:
        raise GateFailure("unsupported performance budget schema version")

    policy = require_mapping(budgets, "measurement_policy")
    if require_positive_int(policy, "repetitions") < 3:
        raise GateFailure("performance gates require at least three repetitions")

    market = require_mapping(budgets, "market_path")
    require_state(market, "timing_state")
    require_state(market, "correctness_state")
    for case in require_list(market, "cases"):
        validate_case(case, ("max_median_ns_per_event", "max_worst_ns_per_event"))

    parser = require_mapping(budgets, "parser")
    require_state(parser, "allocation_state")
    require_non_negative_number(parser, "max_allocations_per_parse")

    scanner = require_mapping(budgets, "scanner_hot_path")
    require_state(scanner, "timing_state")
    require_state(scanner, "correctness_state")
    if require_positive_int(scanner, "repetitions") < 3:
        raise GateFailure("scanner performance gate requires at least three repetitions")
    for case in require_list(scanner, "cases"):
        validate_case(case, ("max_median_ns_per_event", "max_worst_ns_per_event"))
        require_positive_int(case, "expected_top_count")

    dashboard = require_mapping(budgets, "dashboard_snapshot")
    require_state(dashboard, "timing_state")
    require_state(dashboard, "correctness_state")
    require_positive_int(dashboard, "iterations")
    require_non_negative_number(dashboard, "max_conditional_snapshot_copies")

    terminal = require_mapping(budgets, "terminal_render_pipeline")
    require_state(terminal, "timing_state")
    require_state(terminal, "correctness_state")
    if require_positive_int(terminal, "repetitions") < 3:
        raise GateFailure("terminal performance gate requires at least three repetitions")
    require_positive_int(terminal, "iterations")
    for key in (
        "max_unchanged_payload_bytes",
        "max_median_unchanged_ns_per_frame",
        "max_worst_unchanged_ns_per_frame",
        "max_median_changed_ns_per_frame",
        "max_worst_changed_ns_per_frame",
    ):
        require_non_negative_number(terminal, key)

    runtime = require_mapping(budgets, "runtime_qualification")
    require_state(runtime, "memory_state")
    require_state(runtime, "sampled_latency_state")
    return budgets


def load_budgets(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise GateFailure(f"cannot read performance budget file: {error}") from error
    return validate_budgets(value)


def evaluate_upper_bound(
    violations: list[str],
    label: str,
    value: float,
    maximum: float,
    evidence_state: str,
    unit: str,
) -> None:
    if evidence_state == "ENFORCED" and value > maximum:
        violations.append(f"{label}: {value:.2f} {unit} exceeds budget {maximum:.2f} {unit}")


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
    timing_state: str,
    correctness_state: str,
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

        if correctness_state == "ENFORCED":
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
    evaluate_upper_bound(violations, f"{name} median", median_ns, max_median, timing_state, "ns/event")
    evaluate_upper_bound(violations, f"{name} worst sample", worst_ns, max_worst, timing_state, "ns/event")

    return (
        {
            "name": name,
            "symbols": symbols,
            "events_per_symbol": events_per_symbol,
            "expected_events": expected_events,
            "repetitions": repetitions,
            "timing_state": timing_state,
            "correctness_state": correctness_state,
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


def parser_case(
    executable: Path,
    budget: dict[str, Any],
) -> tuple[dict[str, Any], list[str]]:
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
    state = str(budget["allocation_state"])
    violations: list[str] = []

    if state == "ENFORCED" and iterations <= 0:
        violations.append("parser: benchmark reported no iterations")
    evaluate_upper_bound(
        violations,
        "parser allocations",
        allocations_per_parse,
        maximum,
        state,
        "allocations/parse",
    )
    if state == "ENFORCED" and result.returncode != 0:
        violations.append(f"parser: benchmark exited {result.returncode}")

    return (
        {
            "iterations": iterations,
            "allocations": allocations,
            "allocations_per_parse": allocations_per_parse,
            "allocation_state": state,
            "budget": {"max_allocations_per_parse": maximum},
            "status": "PASS" if not violations else "FAIL",
        },
        violations,
    )


def scanner_case(
    executable: Path,
    case: dict[str, Any],
    repetitions: int,
    timing_state: str,
    correctness_state: str,
) -> tuple[dict[str, Any], list[str]]:
    name = str(case["name"])
    symbols = int(case["symbols"])
    events_per_symbol = int(case["events_per_symbol"])
    expected_events = symbols * events_per_symbol
    expected_top_count = int(case["expected_top_count"])
    max_median = float(case["max_median_ns_per_event"])
    max_worst = float(case["max_worst_ns_per_event"])
    samples: list[dict[str, Any]] = []
    violations: list[str] = []

    for iteration in range(repetitions):
        result = run([str(executable), str(symbols), str(events_per_symbol)])
        if result.returncode != 0:
            raise GateFailure(
                f"scanner benchmark {name} repetition {iteration + 1} exited {result.returncode}: "
                f"{result.stderr.strip() or result.stdout.strip()}"
            )
        values = parse_key_value(result.stdout)
        reported_symbols = require_int(values, "symbols")
        events = require_int(values, "events")
        top_count = require_int(values, "top_count")
        top_changes = require_int(values, "top_changes")
        ns_per_event = require_float(values, "nanoseconds_per_event")
        events_per_second = require_float(values, "events_per_second")
        seconds = require_float(values, "seconds")

        if correctness_state == "ENFORCED":
            if reported_symbols != symbols:
                violations.append(f"{name}: expected {symbols} symbols, benchmark reported {reported_symbols}")
            if events != expected_events:
                violations.append(f"{name}: expected {expected_events} events, benchmark reported {events}")
            if top_count != expected_top_count:
                violations.append(f"{name}: expected top_count={expected_top_count}, got {top_count}")
            if top_changes <= 0:
                violations.append(f"{name}: scanner did not report any top-symbol changes")
            if ns_per_event <= 0.0 or events_per_second <= 0.0 or seconds <= 0.0:
                violations.append(f"{name}: benchmark reported a non-positive timing measurement")

        samples.append(
            {
                "iteration": iteration + 1,
                "nanoseconds_per_event": ns_per_event,
                "events_per_second": events_per_second,
                "seconds": seconds,
                "events": events,
                "top_count": top_count,
                "top_changes": top_changes,
            }
        )

    ns_samples = [sample["nanoseconds_per_event"] for sample in samples]
    eps_samples = [sample["events_per_second"] for sample in samples]
    median_ns = float(statistics.median(ns_samples))
    worst_ns = float(max(ns_samples))
    median_eps = float(statistics.median(eps_samples))
    evaluate_upper_bound(violations, f"scanner {name} median", median_ns, max_median, timing_state, "ns/event")
    evaluate_upper_bound(violations, f"scanner {name} worst sample", worst_ns, max_worst, timing_state, "ns/event")

    return (
        {
            "name": name,
            "symbols": symbols,
            "events_per_symbol": events_per_symbol,
            "expected_events": expected_events,
            "repetitions": repetitions,
            "timing_state": timing_state,
            "correctness_state": correctness_state,
            "median_ns_per_event": median_ns,
            "worst_ns_per_event": worst_ns,
            "median_events_per_second": median_eps,
            "budget": {
                "max_median_ns_per_event": max_median,
                "max_worst_ns_per_event": max_worst,
                "expected_top_count": expected_top_count,
            },
            "samples": samples,
            "status": "PASS" if not violations else "FAIL",
        },
        violations,
    )


def dashboard_case(
    executable: Path,
    budget: dict[str, Any],
) -> tuple[dict[str, Any], list[str]]:
    iterations = int(budget["iterations"])
    timing_state = str(budget["timing_state"])
    correctness_state = str(budget["correctness_state"])
    max_copies = int(budget["max_conditional_snapshot_copies"])
    result = run([str(executable), str(iterations)])
    if result.returncode not in (0, 1):
        raise GateFailure(
            f"dashboard snapshot benchmark exited {result.returncode}: "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )

    values = parse_key_value(result.stdout)
    reported_iterations = require_int(values, "iterations")
    unconditional_ns = require_float(values, "unconditional_ns_per_poll")
    conditional_ns = require_float(values, "unchanged_conditional_ns_per_poll")
    conditional_copies = require_int(values, "conditional_snapshot_copies")
    copied_payload_bytes = require_int(values, "copied_payload_bytes")
    violations: list[str] = []

    if correctness_state == "ENFORCED":
        if reported_iterations != iterations:
            violations.append(
                f"dashboard: expected {iterations} iterations, benchmark reported {reported_iterations}"
            )
        evaluate_upper_bound(
            violations,
            "dashboard unchanged conditional copies",
            float(conditional_copies),
            float(max_copies),
            correctness_state,
            "copies",
        )
        if copied_payload_bytes <= 0:
            violations.append("dashboard: unconditional snapshot path produced no copied payload evidence")
        if result.returncode != 0:
            violations.append(f"dashboard: benchmark exited {result.returncode}")

    return (
        {
            "iterations": reported_iterations,
            "timing_state": timing_state,
            "correctness_state": correctness_state,
            "unconditional_ns_per_poll": unconditional_ns,
            "unchanged_conditional_ns_per_poll": conditional_ns,
            "conditional_snapshot_copies": conditional_copies,
            "copied_payload_bytes": copied_payload_bytes,
            "budget": {"max_conditional_snapshot_copies": max_copies},
            "status": "PASS" if not violations else "FAIL",
        },
        violations,
    )


def terminal_case(
    executable: Path,
    budget: dict[str, Any],
    repetitions: int,
) -> tuple[dict[str, Any], list[str]]:
    iterations = int(budget["iterations"])
    timing_state = str(budget["timing_state"])
    correctness_state = str(budget["correctness_state"])
    max_unchanged_payload = int(budget["max_unchanged_payload_bytes"])
    samples: list[dict[str, Any]] = []
    violations: list[str] = []

    for iteration in range(repetitions):
        result = run([str(executable), str(iterations)])
        if result.returncode not in (0, 2):
            raise GateFailure(
                f"terminal render benchmark repetition {iteration + 1} exited {result.returncode}: "
                f"{result.stderr.strip() or result.stdout.strip()}"
            )
        values = parse_key_value(result.stdout)
        reported_iterations = require_int(values, "iterations")
        unchanged_ns = require_float(values, "unchanged_ns_per_frame")
        unchanged_payload = require_int(values, "unchanged_payload_bytes")
        changed_ns = require_float(values, "changed_ns_per_frame")
        changed_payload = require_int(values, "changed_payload_bytes")

        if correctness_state == "ENFORCED":
            if reported_iterations != iterations:
                violations.append(
                    f"terminal: expected {iterations} iterations, benchmark reported {reported_iterations}"
                )
            evaluate_upper_bound(
                violations,
                "terminal unchanged payload",
                float(unchanged_payload),
                float(max_unchanged_payload),
                correctness_state,
                "bytes",
            )
            if changed_payload <= 0:
                violations.append("terminal: changed frame produced no payload")
            if result.returncode != 0:
                violations.append(f"terminal: benchmark exited {result.returncode}")

        samples.append(
            {
                "iteration": iteration + 1,
                "unchanged_ns_per_frame": unchanged_ns,
                "unchanged_payload_bytes": unchanged_payload,
                "changed_ns_per_frame": changed_ns,
                "changed_payload_bytes": changed_payload,
            }
        )

    unchanged_samples = [sample["unchanged_ns_per_frame"] for sample in samples]
    changed_samples = [sample["changed_ns_per_frame"] for sample in samples]
    median_unchanged = float(statistics.median(unchanged_samples))
    worst_unchanged = float(max(unchanged_samples))
    median_changed = float(statistics.median(changed_samples))
    worst_changed = float(max(changed_samples))
    evaluate_upper_bound(
        violations,
        "terminal unchanged median",
        median_unchanged,
        float(budget["max_median_unchanged_ns_per_frame"]),
        timing_state,
        "ns/frame",
    )
    evaluate_upper_bound(
        violations,
        "terminal unchanged worst sample",
        worst_unchanged,
        float(budget["max_worst_unchanged_ns_per_frame"]),
        timing_state,
        "ns/frame",
    )
    evaluate_upper_bound(
        violations,
        "terminal changed median",
        median_changed,
        float(budget["max_median_changed_ns_per_frame"]),
        timing_state,
        "ns/frame",
    )
    evaluate_upper_bound(
        violations,
        "terminal changed worst sample",
        worst_changed,
        float(budget["max_worst_changed_ns_per_frame"]),
        timing_state,
        "ns/frame",
    )

    return (
        {
            "iterations": iterations,
            "repetitions": repetitions,
            "timing_state": timing_state,
            "correctness_state": correctness_state,
            "median_unchanged_ns_per_frame": median_unchanged,
            "worst_unchanged_ns_per_frame": worst_unchanged,
            "median_changed_ns_per_frame": median_changed,
            "worst_changed_ns_per_frame": worst_changed,
            "budget": {
                "max_unchanged_payload_bytes": max_unchanged_payload,
                "max_median_unchanged_ns_per_frame": float(budget["max_median_unchanged_ns_per_frame"]),
                "max_worst_unchanged_ns_per_frame": float(budget["max_worst_unchanged_ns_per_frame"]),
                "max_median_changed_ns_per_frame": float(budget["max_median_changed_ns_per_frame"]),
                "max_worst_changed_ns_per_frame": float(budget["max_worst_changed_ns_per_frame"]),
            },
            "samples": samples,
            "status": "PASS" if not violations else "FAIL",
        },
        violations,
    )


def qualification_evidence(
    paths: list[str],
    budget: dict[str, Any],
) -> list[dict[str, Any]]:
    evidence: list[dict[str, Any]] = []
    for raw_path in paths:
        path = Path(raw_path)
        try:
            report = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise GateFailure(f"cannot read runtime qualification evidence {path}: {error}") from error
        if not isinstance(report, dict) or report.get("schema_version") != 1:
            raise GateFailure(f"runtime qualification evidence {path} has unsupported schema")
        if "scenario" not in report or "memory" not in report or "metrics" not in report:
            raise GateFailure(f"runtime qualification evidence {path} is missing required metrics")
        evidence.append(
            {
                "source": str(path),
                "scenario": report["scenario"],
                "status": report.get("status", "UNKNOWN"),
                "evidence_complete": bool(report.get("evidence_complete", False)),
                "memory_state": budget["memory_state"],
                "sampled_latency_state": budget["sampled_latency_state"],
                "actual_duration_seconds": report.get("actual_duration_seconds"),
                "memory": report["memory"],
                "latency": report["metrics"].get("latency"),
            }
        )
    return evidence


def write_summary(path: Path, report: dict[str, Any]) -> None:
    lines = [
        "# Sentum performance gate",
        "",
        f"**Status:** {report['status']}",
        "",
        "CI regression budgets are not production latency or memory SLAs.",
        "",
        "## Evidence states",
        "",
        "| Metric | Timing / allocation | Correctness |",
        "| --- | --- | --- |",
        f"| Market path | {report['evidence_states']['market_path_timing']} | {report['evidence_states']['market_path_correctness']} |",
        f"| Parser allocations | {report['evidence_states']['parser_allocations']} | ENFORCED |",
        f"| Scanner hot path | {report['evidence_states']['scanner_timing']} | {report['evidence_states']['scanner_correctness']} |",
        f"| Dashboard snapshot | {report['evidence_states']['dashboard_timing']} | {report['evidence_states']['dashboard_correctness']} |",
        f"| Terminal render pipeline | {report['evidence_states']['terminal_timing']} | {report['evidence_states']['terminal_correctness']} |",
        f"| Runtime RSS / trend | {report['evidence_states']['runtime_memory']} | n/a |",
        f"| Sampled runtime latency | {report['evidence_states']['runtime_latency']} | n/a |",
        "",
        "## Market path",
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
            "## Parser allocations",
            "",
            f"Allocations/parse: **{parser['allocations_per_parse']:.6f}** "
            f"(budget <= {parser['budget']['max_allocations_per_parse']:.6f}) — **{parser['status']}**",
            "",
            "## Scanner hot path",
            "",
            "| Workload | Median ns/event | Worst ns/event | Median budget | Worst guardrail | Status |",
            "| --- | ---: | ---: | ---: | ---: | --- |",
        ]
    )
    for case in report["scanner_hot_path"]:
        lines.append(
            "| {name} | {median:.2f} | {worst:.2f} | {median_budget:.2f} | {worst_budget:.2f} | {status} |".format(
                name=case["name"],
                median=case["median_ns_per_event"],
                worst=case["worst_ns_per_event"],
                median_budget=case["budget"]["max_median_ns_per_event"],
                worst_budget=case["budget"]["max_worst_ns_per_event"],
                status=case["status"],
            )
        )

    dashboard = report["dashboard_snapshot"]
    lines.extend(
        [
            "",
            "## Dashboard snapshot",
            "",
            f"Timing: **{dashboard['timing_state']}**; correctness: **{dashboard['correctness_state']}**.",
            f"Unconditional: {dashboard['unconditional_ns_per_poll']:.2f} ns/poll.",
            f"Unchanged conditional: {dashboard['unchanged_conditional_ns_per_poll']:.2f} ns/poll.",
            f"Conditional snapshot copies: **{dashboard['conditional_snapshot_copies']}** "
            f"(budget <= {dashboard['budget']['max_conditional_snapshot_copies']}) — **{dashboard['status']}**",
        ]
    )

    terminal = report["terminal_render_pipeline"]
    lines.extend(
        [
            "",
            "## Terminal render pipeline",
            "",
            f"Timing: **{terminal['timing_state']}**; correctness: **{terminal['correctness_state']}**.",
            f"Unchanged median/worst: {terminal['median_unchanged_ns_per_frame']:.2f} / "
            f"{terminal['worst_unchanged_ns_per_frame']:.2f} ns/frame.",
            f"Changed median/worst: {terminal['median_changed_ns_per_frame']:.2f} / "
            f"{terminal['worst_changed_ns_per_frame']:.2f} ns/frame.",
        ]
    )

    if report["runtime_qualification"]:
        lines.extend(["", "## Runtime qualification observations", ""])
        for item in report["runtime_qualification"]:
            memory = item["memory"]
            lines.append(
                "- {scenario}: status={status}, RSS start={start} KiB, peak={peak} KiB, end={end} KiB, "
                "growth={growth} KiB, trend={trend} KiB/min; memory evidence remains {state}.".format(
                    scenario=item["scenario"],
                    status=item["status"],
                    start=memory.get("starting_rss_kib"),
                    peak=memory.get("peak_rss_kib"),
                    end=memory.get("ending_rss_kib"),
                    growth=memory.get("growth_kib"),
                    trend=memory.get("trend_slope_kib_per_minute"),
                    state=item["memory_state"],
                )
            )

    if report["violations"]:
        lines.extend(["", "## Violations", ""])
        lines.extend(f"- {violation}" for violation in report["violations"])
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    args = parse_args()
    budget_path = Path(args.budget_file)
    build_dir = Path(args.build_dir)
    report_path = Path(args.report)
    summary_path = Path(args.summary)

    try:
        budgets = load_budgets(budget_path)
        configured_repetitions = int(budgets["measurement_policy"]["repetitions"])
        repetitions = args.repetitions if args.repetitions is not None else configured_repetitions
        if repetitions < 3:
            raise GateFailure("performance gates require at least three repetitions")

        scanner_repetitions = args.repetitions if args.repetitions is not None else int(
            budgets["scanner_hot_path"]["repetitions"]
        )
        terminal_repetitions = args.repetitions if args.repetitions is not None else int(
            budgets["terminal_render_pipeline"]["repetitions"]
        )
        if scanner_repetitions < 3 or terminal_repetitions < 3:
            raise GateFailure("repeated performance gates require at least three repetitions")

        executables = {
            "market": build_dir / "sentum_market_benchmark",
            "parser": build_dir / "sentum_parser_allocation_benchmark",
            "scanner": build_dir / "sentum_scanner_hot_path_benchmark",
            "dashboard": build_dir / "sentum_dashboard_snapshot_benchmark",
            "terminal": build_dir / "sentum_terminal_render_pipeline_benchmark",
        }
        for executable in executables.values():
            if not executable.is_file():
                raise GateFailure(f"benchmark executable not found: {executable}")

        violations: list[str] = []
        market_results: list[dict[str, Any]] = []
        market_budget = budgets["market_path"]
        for case in market_budget["cases"]:
            result, case_violations = market_case(
                executables["market"],
                case,
                repetitions,
                market_budget["timing_state"],
                market_budget["correctness_state"],
            )
            market_results.append(result)
            violations.extend(case_violations)

        parser_result, parser_violations = parser_case(executables["parser"], budgets["parser"])
        violations.extend(parser_violations)

        scanner_results: list[dict[str, Any]] = []
        scanner_budget = budgets["scanner_hot_path"]
        for case in scanner_budget["cases"]:
            result, case_violations = scanner_case(
                executables["scanner"],
                case,
                scanner_repetitions,
                scanner_budget["timing_state"],
                scanner_budget["correctness_state"],
            )
            scanner_results.append(result)
            violations.extend(case_violations)

        dashboard_result, dashboard_violations = dashboard_case(
            executables["dashboard"],
            budgets["dashboard_snapshot"],
        )
        violations.extend(dashboard_violations)

        terminal_result, terminal_violations = terminal_case(
            executables["terminal"],
            budgets["terminal_render_pipeline"],
            terminal_repetitions,
        )
        violations.extend(terminal_violations)

        runtime_evidence = qualification_evidence(
            args.qualification_report,
            budgets["runtime_qualification"],
        )
        for item in runtime_evidence:
            if item["status"] != "PASS" or not item["evidence_complete"]:
                violations.append(
                    f"runtime qualification evidence is incomplete or failing for {item['scenario']}"
                )

        report: dict[str, Any] = {
            "schema_version": 2,
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
                "market_repetitions": repetitions,
                "scanner_repetitions": scanner_repetitions,
                "terminal_repetitions": terminal_repetitions,
                "market_gate_statistic": budgets["measurement_policy"]["market_gate_statistic"],
                "market_guardrail_statistic": budgets["measurement_policy"]["market_guardrail_statistic"],
            },
            "baseline_evidence": budgets.get("baseline_evidence", {}),
            "evidence_states": {
                "market_path_timing": market_budget["timing_state"],
                "market_path_correctness": market_budget["correctness_state"],
                "parser_allocations": budgets["parser"]["allocation_state"],
                "scanner_timing": scanner_budget["timing_state"],
                "scanner_correctness": scanner_budget["correctness_state"],
                "dashboard_timing": budgets["dashboard_snapshot"]["timing_state"],
                "dashboard_correctness": budgets["dashboard_snapshot"]["correctness_state"],
                "terminal_timing": budgets["terminal_render_pipeline"]["timing_state"],
                "terminal_correctness": budgets["terminal_render_pipeline"]["correctness_state"],
                "runtime_memory": budgets["runtime_qualification"]["memory_state"],
                "runtime_latency": budgets["runtime_qualification"]["sampled_latency_state"],
            },
            "market_path": market_results,
            "parser": parser_result,
            "scanner_hot_path": scanner_results,
            "dashboard_snapshot": dashboard_result,
            "terminal_render_pipeline": terminal_result,
            "runtime_qualification": runtime_evidence,
            "violations": violations,
        }

        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        write_summary(summary_path, report)
        print(summary_path.read_text(encoding="utf-8"), end="")
        return 0 if report["status"] == "PASS" else 1
    except (GateFailure, KeyError, TypeError, ValueError, OSError) as error:
        print(f"performance gate configuration/execution failure: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
