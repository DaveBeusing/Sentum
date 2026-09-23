#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


FAULT_SCENARIOS = (
    "market-data-reconnect",
    "user-stream-interruption",
    "listen-key-keepalive",
    "partial-fill",
    "unresolved-order-restart",
    "balance-mismatch",
    "persistence-pressure",
    "dashboard-recovery",
    "kill-switch-recovery",
)
SPECIAL_SCENARIOS = ("paper-soak", "all-faults", "runtime-restart", "persistence-write-failure")
SCENARIOS = FAULT_SCENARIOS + SPECIAL_SCENARIOS


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def resolve_git_sha():
    github_sha = os.environ.get("GITHUB_SHA", "").strip()
    if github_sha:
        return github_sha
    try:
        completed = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
            timeout=5,
        )
        return completed.stdout.strip() or "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def read_process_status(pid):
    rss_kib = None
    threads = None
    try:
        with open(f"/proc/{pid}/status", "r", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    parts = line.split()
                    rss_kib = int(parts[1])
                elif line.startswith("Threads:"):
                    parts = line.split()
                    threads = int(parts[1])
    except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError):
        return None, None
    return rss_kib, threads


def terminate_process(process):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


def monitor_command(command, timeout_seconds, sample_interval_seconds):
    started = time.monotonic()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    samples = []
    timed_out = False

    while process.poll() is None:
        elapsed = time.monotonic() - started
        rss, threads = read_process_status(process.pid)
        if rss is not None or threads is not None:
            sample = {"elapsed_seconds": elapsed}
            if rss is not None:
                sample["rss_kib"] = rss
            if threads is not None:
                sample["threads"] = threads
            samples.append(sample)
        if elapsed >= timeout_seconds:
            timed_out = True
            terminate_process(process)
            break
        time.sleep(sample_interval_seconds)

    if not timed_out:
        rss, threads = read_process_status(process.pid)
        if rss is not None or threads is not None:
            sample = {"elapsed_seconds": time.monotonic() - started}
            if rss is not None:
                sample["rss_kib"] = rss
            if threads is not None:
                sample["threads"] = threads
            samples.append(sample)

    stdout, stderr = process.communicate()
    return {
        "command": command,
        "returncode": 124 if timed_out else process.returncode,
        "timed_out": timed_out,
        "elapsed_seconds": time.monotonic() - started,
        "stdout": stdout,
        "stderr": stderr,
        "rss_samples": samples,
    }


def parse_json_line(stdout):
    candidates = [line.strip() for line in stdout.splitlines() if line.strip()]
    for line in reversed(candidates):
        try:
            value = json.loads(line)
            if isinstance(value, dict):
                return value
        except json.JSONDecodeError:
            continue
    return None


def flatten_metric_values(value, key):
    found = []
    if isinstance(value, dict):
        for candidate_key, candidate_value in value.items():
            if candidate_key == key and isinstance(candidate_value, (int, float)) and not isinstance(candidate_value, bool):
                found.append(candidate_value)
            found.extend(flatten_metric_values(candidate_value, key))
    elif isinstance(value, list):
        for item in value:
            found.extend(flatten_metric_values(item, key))
    return found


def flatten_text_values(value, key):
    found = []
    if isinstance(value, dict):
        for candidate_key, candidate_value in value.items():
            if candidate_key == key and isinstance(candidate_value, str):
                found.append(candidate_value)
            found.extend(flatten_text_values(candidate_value, key))
    elif isinstance(value, list):
        for item in value:
            found.extend(flatten_text_values(item, key))
    return found


def summarize_rss(samples, max_growth_kib):
    rss_samples = [sample for sample in samples if "rss_kib" in sample]
    values = [sample["rss_kib"] for sample in rss_samples]
    if not values:
        return {
            "available": False,
            "starting_rss_kib": None,
            "peak_rss_kib": None,
            "ending_rss_kib": None,
            "growth_kib": None,
            "trend_slope_kib_per_minute": None,
            "unbounded_growth_detected": False,
            "guardrail_kib": max_growth_kib,
            "samples": 0,
        }

    starting = values[0]
    peak = max(values)
    ending = values[-1]
    trend_samples = rss_samples[max(0, len(rss_samples) // 4):]
    slope = 0.0
    if len(trend_samples) >= 2:
        xs = [sample["elapsed_seconds"] for sample in trend_samples]
        ys = [sample["rss_kib"] for sample in trend_samples]
        x_mean = sum(xs) / len(xs)
        y_mean = sum(ys) / len(ys)
        denominator = sum((x - x_mean) ** 2 for x in xs)
        if denominator > 0.0:
            slope_per_second = sum((x - x_mean) * (y - y_mean) for x, y in zip(xs, ys)) / denominator
            slope = slope_per_second * 60.0

    growth = ending - starting
    unbounded = max_growth_kib > 0 and growth > max_growth_kib and slope > 0.0
    return {
        "available": True,
        "starting_rss_kib": starting,
        "peak_rss_kib": peak,
        "ending_rss_kib": ending,
        "growth_kib": growth,
        "trend_slope_kib_per_minute": slope,
        "unbounded_growth_detected": unbounded,
        "guardrail_kib": max_growth_kib,
        "samples": len(rss_samples),
    }


def summarize_threads(samples):
    values = [sample["threads"] for sample in samples if "threads" in sample]
    if not values:
        return {
            "available": False,
            "starting": None,
            "peak": None,
            "ending": None,
            "leak_indicator": False,
            "samples": 0,
        }
    starting = values[0]
    ending = values[-1]
    return {
        "available": True,
        "starting": starting,
        "peak": max(values),
        "ending": ending,
        "leak_indicator": ending > starting,
        "samples": len(values),
    }


def merge_process_samples(command_results):
    samples = []
    elapsed_offset = 0.0
    for result in command_results:
        for sample in result["rss_samples"]:
            merged = dict(sample)
            merged["elapsed_seconds"] = elapsed_offset + sample["elapsed_seconds"]
            samples.append(merged)
        elapsed_offset += result["elapsed_seconds"]
    return samples


def merge_rss(command_results, max_growth_kib):
    return summarize_rss(merge_process_samples(command_results), max_growth_kib)


def executable(build_dir, name):
    path = Path(build_dir) / name
    if not path.exists():
        raise FileNotFoundError(f"required qualification binary is missing: {path}")
    return str(path)


def scenario_commands(args):
    scenario_binary = executable(args.build_dir, "sentum_runtime_qualification_scenarios")
    if args.scenario == "runtime-restart":
        return [
            [executable(args.build_dir, "sentum_notification_delivery_evidence_repository_tests")],
            [executable(args.build_dir, "sentum_governed_incident_lifecycle_runtime_tests")],
        ]
    if args.scenario == "persistence-write-failure":
        return [[executable(args.build_dir, "sentum_notification_delivery_evidence_repository_tests")]]
    return [[
        scenario_binary,
        "--scenario", args.scenario,
        "--duration-seconds", str(args.duration_seconds),
        "--seed", str(args.seed),
    ]]


def aggregate_scenario_metrics(parsed_results):
    queue_depths = flatten_metric_values(parsed_results, "queue_depth")
    queue_high_water = flatten_metric_values(parsed_results, "queue_high_water")
    queue_saturation = flatten_metric_values(parsed_results, "queue_saturation_events")
    queue_drops = flatten_metric_values(parsed_results, "queue_drop_count")
    reconnects = flatten_metric_values(parsed_results, "reconnect_count")
    restarts = flatten_metric_values(parsed_results, "restart_count")
    lifecycle_failures = flatten_metric_values(parsed_results, "lifecycle_failures")
    kill_switch = flatten_metric_values(parsed_results, "kill_switch_transitions")
    throughputs = flatten_metric_values(parsed_results, "event_throughput_per_second")
    outcomes = flatten_text_values(parsed_results, "reconciliation_outcome")

    return {
        "queue": {
            "ending_depth": max(queue_depths) if queue_depths else 0,
            "high_water": max(queue_high_water) if queue_high_water else 0,
            "saturation_events": sum(queue_saturation) if queue_saturation else 0,
            "drop_count": sum(queue_drops) if queue_drops else 0,
        },
        "reconnect_count": sum(reconnects) if reconnects else 0,
        "restart_count": sum(restarts) if restarts else 0,
        "lifecycle_failures": sum(lifecycle_failures) if lifecycle_failures else 0,
        "kill_switch_transitions": sum(kill_switch) if kill_switch else 0,
        "event_throughput_per_second": max(throughputs) if throughputs else None,
        "reconciliation_outcome": outcomes[-1] if len(set(outcomes)) == 1 and outcomes else (
            outcomes if outcomes else "not_reported"
        ),
    }


def write_summary(path, report):
    rss = report["memory"]
    queue = report["metrics"]["queue"]
    lines = [
        "# Runtime qualification",
        "",
        f"Status: **{report['status']}**",
        f"Commit: `{report['git_sha']}`",
        f"Scenario: `{report['scenario']}`",
        f"Seed: `{report['seed']}`",
        f"Duration: {report['actual_duration_seconds']:.3f}s",
        "",
        "| Evidence | Value |",
        "| --- | ---: |",
        f"| RSS start | {rss['starting_rss_kib'] if rss['starting_rss_kib'] is not None else 'unavailable'} KiB |",
        f"| RSS peak | {rss['peak_rss_kib'] if rss['peak_rss_kib'] is not None else 'unavailable'} KiB |",
        f"| RSS end | {rss['ending_rss_kib'] if rss['ending_rss_kib'] is not None else 'unavailable'} KiB |",
        f"| Queue high-water | {queue['high_water']} |",
        f"| Queue saturation events | {queue['saturation_events']} |",
        f"| Queue drops | {queue['drop_count']} |",
        f"| Reconnects | {report['metrics']['reconnect_count']} |",
        f"| Restarts | {report['metrics']['restart_count']} |",
        f"| Lifecycle failures | {report['metrics']['lifecycle_failures']} |",
        f"| Kill-switch transitions | {report['metrics']['kill_switch_transitions']} |",
        "",
        f"Evidence complete: **{str(report['evidence_complete']).lower()}**",
    ]
    if report["failures"]:
        lines += ["", "## Failures", ""]
        lines.extend(f"- {failure}" for failure in report["failures"])
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="Sentum runtime qualification runner")
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--scenario", choices=SCENARIOS, required=True)
    parser.add_argument("--duration-seconds", type=float, default=2.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--output", default="log/runtime_qualification.json")
    parser.add_argument("--summary", default="log/runtime_qualification.md")
    parser.add_argument("--timeout-seconds", type=float, default=120.0)
    parser.add_argument("--sample-interval-seconds", type=float, default=0.10)
    parser.add_argument("--max-rss-growth-kib", type=int, default=0)
    args = parser.parse_args()

    if args.duration_seconds <= 0:
        raise SystemExit("--duration-seconds must be > 0")
    if args.timeout_seconds <= 0:
        raise SystemExit("--timeout-seconds must be > 0")
    if args.sample_interval_seconds <= 0:
        raise SystemExit("--sample-interval-seconds must be > 0")
    if args.max_rss_growth_kib < 0:
        raise SystemExit("--max-rss-growth-kib must be >= 0")

    output_path = Path(args.output)
    summary_path = Path(args.summary)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.parent.mkdir(parents=True, exist_ok=True)

    started_utc = utc_now()
    overall_started = time.monotonic()
    failures = []
    command_specs = []
    command_results = []
    parsed_results = []

    try:
        command_specs = scenario_commands(args)
        remaining_timeout = args.timeout_seconds
        for command in command_specs:
            result = monitor_command(
                command,
                max(0.1, remaining_timeout),
                args.sample_interval_seconds,
            )
            parsed = parse_json_line(result["stdout"])
            result["parsed_result"] = parsed
            command_results.append(result)
            parsed_results.append(parsed if parsed is not None else {})
            if result["timed_out"]:
                failures.append("qualification command timed out")
            if result["returncode"] != 0:
                detail = result["stderr"].strip() or result["stdout"].strip() or f"exit code {result['returncode']}"
                failures.append(detail[-4000:])
            if args.scenario not in ("runtime-restart", "persistence-write-failure"):
                if parsed is None:
                    failures.append("qualification binary did not produce machine-readable scenario evidence")
                elif not parsed.get("pass", False):
                    failures.append(parsed.get("failure", "scenario reported failure"))
            remaining_timeout = args.timeout_seconds - (time.monotonic() - overall_started)
            if remaining_timeout <= 0:
                break
    except (FileNotFoundError, OSError, ValueError) as error:
        failures.append(str(error))

    actual_duration = time.monotonic() - overall_started
    process_samples = merge_process_samples(command_results) if command_results else []
    rss = summarize_rss(process_samples, args.max_rss_growth_kib)
    threads = summarize_threads(process_samples)
    if rss["unbounded_growth_detected"]:
        failures.append(
            f"RSS growth guardrail exceeded: growth={rss['growth_kib']} KiB "
            f"guardrail={args.max_rss_growth_kib} KiB"
        )

    metrics = aggregate_scenario_metrics(parsed_results)
    timed_out = any(command.get("timed_out", False) for command in command_results)
    command_failures = any(command.get("returncode", 1) != 0 for command in command_results)
    git_sha = resolve_git_sha()
    scenario_evidence_present = bool(command_results) and all(
        command.get("parsed_result") is not None
        for command in command_results
        if args.scenario not in ("runtime-restart", "persistence-write-failure")
    )
    if args.scenario in ("runtime-restart", "persistence-write-failure"):
        scenario_evidence_present = bool(command_results) and not command_failures

    evidence_complete = (
        git_sha != "unknown"
        and bool(command_results)
        and scenario_evidence_present
        and not timed_out
        and not command_failures
    )

    if not evidence_complete and not failures:
        failures.append("qualification evidence is incomplete")

    status = "PASS" if evidence_complete and not failures else "FAIL"
    report = {
        "schema_version": 1,
        "git_sha": git_sha,
        "workflow_run_id": os.environ.get("GITHUB_RUN_ID", "local"),
        "scenario": args.scenario,
        "started_at_utc": started_utc,
        "ended_at_utc": utc_now(),
        "requested_duration_seconds": args.duration_seconds,
        "actual_duration_seconds": actual_duration,
        "seed": args.seed,
        "status": status,
        "pass": status == "PASS",
        "evidence_complete": evidence_complete,
        "memory": rss,
        "threads": threads,
        "metrics": metrics,
        "timeout": {
            "limit_seconds": args.timeout_seconds,
            "timed_out": timed_out,
        },
        "failures": failures,
        "scenario_evidence": parsed_results,
        "commands": [
            {
                "command": command["command"],
                "returncode": command["returncode"],
                "timed_out": command["timed_out"],
                "elapsed_seconds": command["elapsed_seconds"],
                "stderr": command["stderr"][-4000:],
            }
            for command in command_results
        ],
    }

    output_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_summary(summary_path, report)

    if status != "PASS":
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print(
        f"runtime qualification passed: scenario={args.scenario} "
        f"duration={actual_duration:.3f}s seed={args.seed}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
