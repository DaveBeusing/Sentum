#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


def output_text(value):
	if isinstance(value, bytes):
		return value.decode("utf-8", errors="replace")
	return value or ""


def run_cycle(command, timeout_seconds):
	started = time.monotonic()
	try:
		with subprocess.Popen(
			command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
			text=True, encoding="utf-8", errors="replace", start_new_session=True,
		) as process:
			try:
				stdout, stderr = process.communicate(timeout=timeout_seconds)
				result = {
					"returncode": process.returncode,
					"timed_out": False,
					"stdout": stdout,
					"stderr": stderr,
				}
			except subprocess.TimeoutExpired as error:
				result = {
					"returncode": 124,
					"timed_out": True,
					"stdout": output_text(error.stdout),
					"stderr": output_text(error.stderr),
					"failure": f"acceptance command timed out after {timeout_seconds} seconds",
				}
			finally:
				# Also stop descendants that inherited the command's output pipes.
				try:
					os.killpg(process.pid, signal.SIGKILL)
				except ProcessLookupError:
					pass
				process.wait(timeout=2)
	except OSError as error:
		result = {
			"returncode": 127,
			"timed_out": False,
			"stdout": "",
			"stderr": str(error),
			"failure": "acceptance command could not be started",
		}
	result.update({"command": command, "elapsed_seconds": time.monotonic() - started})
	return result


def main():
	parser = argparse.ArgumentParser(description="Sentum operational acceptance runner")
	parser.add_argument("--build-dir", default="build")
	parser.add_argument("--cycles", type=int, default=5)
	parser.add_argument("--timeout-seconds", type=int, default=90)
	parser.add_argument("--report", default="log/operational_acceptance.json")
	parser.add_argument("--summary", default="log/operational_acceptance.md")
	args = parser.parse_args()

	if args.cycles < 1:
		raise SystemExit("--cycles must be >= 1")
	if args.timeout_seconds < 1:
		raise SystemExit("--timeout-seconds must be >= 1")

	build_dir = Path(args.build_dir)
	lifecycle = build_dir / "sentum_runtime_lifecycle_tests"
	observability = build_dir / "sentum_runtime_observability_tests"
	if not lifecycle.exists() or not observability.exists():
		raise SystemExit("required acceptance binaries are missing")

	report_path = Path(args.report)
	summary_path = Path(args.summary)
	report_path.parent.mkdir(parents=True, exist_ok=True)
	summary_path.parent.mkdir(parents=True, exist_ok=True)

	results = []
	passed = True
	for cycle in range(1, args.cycles + 1):
		cycle_result = {"cycle": cycle, "checks": []}
		for binary in (lifecycle, observability):
			check = run_cycle([str(binary)], args.timeout_seconds)
			cycle_result["checks"].append(check)
			if check["returncode"] != 0:
				passed = False
		results.append(cycle_result)
		if not passed:
			break

	payload = {
		"schema_version": 1,
		"generated_at": datetime.now(timezone.utc).isoformat(),
		"environment_class": "ci_rehearsal",
		"status": "PASS" if passed else "FAIL",
		"git_sha": os.environ.get("GITHUB_SHA", "unknown"),
		"workflow_run_id": os.environ.get("GITHUB_RUN_ID", "unknown"),
		"cycles_requested": args.cycles,
		"cycles_completed": len(results),
		"timeout_seconds": args.timeout_seconds,
		"results": results,
	}
	report_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")

	lines = [
		"# Operational acceptance",
		"",
		f"Status: **{payload['status']}**",
		f"Commit: `{payload['git_sha']}`",
		f"Cycles: {payload['cycles_completed']}/{payload['cycles_requested']}",
		"",
		"| Cycle | Check | Result | Elapsed (s) |",
		"| ---: | --- | --- | ---: |",
	]
	failure_lines = []
	for cycle in results:
		for check in cycle["checks"]:
			name = Path(check["command"][0]).name
			result = "PASS" if check["returncode"] == 0 else "FAIL"
			lines.append(f"| {cycle['cycle']} | {name} | {result} | {check['elapsed_seconds']:.3f} |")
			if check["returncode"] != 0:
				failure_lines += [
					"", f"## Failed check: cycle {cycle['cycle']}, {name}", "",
					f"Command: `{json.dumps(check['command'])}`",
					f"Return code: `{check['returncode']}`; timed out: `{str(check['timed_out']).lower()}`",
					f"Elapsed: {check['elapsed_seconds']:.3f}s",
					check.get("failure", "acceptance command returned a non-zero status"),
				]
				for stream in ("stdout", "stderr"):
					failure_lines += ["", f"### {stream}", ""]
					failure_lines.extend(f"    {line}" for line in check[stream].splitlines())
					if not check[stream]:
						failure_lines.append("(no output)")
	lines.extend(failure_lines)
	summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

	if not passed:
		for cycle in results:
			for check in cycle["checks"]:
				if check["returncode"] != 0:
					if "failure" in check:
						print(check["failure"], file=sys.stderr)
					print(check["stdout"], end="")
					print(check["stderr"], file=sys.stderr, end="")
		return 1

	print(f"operational acceptance passed: {payload['cycles_completed']} cycles")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
