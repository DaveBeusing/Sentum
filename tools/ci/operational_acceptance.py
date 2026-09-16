#!/usr/bin/env python3

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path


def run_cycle(command, timeout_seconds):
	started = time.monotonic()
	completed = subprocess.run(command, capture_output=True, text=True, timeout=timeout_seconds)
	elapsed = time.monotonic() - started
	return {
		"command": command,
		"returncode": completed.returncode,
		"elapsed_seconds": elapsed,
		"stdout": completed.stdout,
		"stderr": completed.stderr,
	}


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
			try:
				check = run_cycle([str(binary)], args.timeout_seconds)
			except subprocess.TimeoutExpired as error:
				check = {
					"command": [str(binary)],
					"returncode": 124,
					"elapsed_seconds": args.timeout_seconds,
					"stdout": error.stdout or "",
					"stderr": error.stderr or "timeout",
				}
			cycle_result["checks"].append(check)
			if check["returncode"] != 0:
				passed = False
		results.append(cycle_result)
		if not passed:
			break

	payload = {
		"status": "PASS" if passed else "FAIL",
		"cycles_requested": args.cycles,
		"cycles_completed": len(results),
		"timeout_seconds": args.timeout_seconds,
		"results": results,
	}
	report_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

	lines = [
		"# Operational acceptance",
		"",
		f"Status: **{payload['status']}**",
		f"Cycles: {payload['cycles_completed']}/{payload['cycles_requested']}",
		"",
		"| Cycle | Check | Result | Elapsed (s) |",
		"| ---: | --- | --- | ---: |",
	]
	for cycle in results:
		for check in cycle["checks"]:
			name = Path(check["command"][0]).name
			result = "PASS" if check["returncode"] == 0 else "FAIL"
			lines.append(f"| {cycle['cycle']} | {name} | {result} | {check['elapsed_seconds']:.3f} |")
	summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

	if not passed:
		for cycle in results:
			for check in cycle["checks"]:
				if check["returncode"] != 0:
					print(check["stdout"], end="")
					print(check["stderr"], file=sys.stderr, end="")
		return 1

	print(f"operational acceptance passed: {payload['cycles_completed']} cycles")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
