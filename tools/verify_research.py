#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT
"""Independently validate a persisted Sentum managed research experiment.

The verifier intentionally operates on persisted experiment evidence. It does not
import the C++ research ranking or scoring implementation.

Usage:
  python3 tools/verify_research.py log/experiments/<run-id> --registry log/experiments.sqlite3
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import sqlite3
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


REPORT_SCHEMA_VERSION = 1
METRIC_KEYS = (
    "trades",
    "net_profit",
    "max_drawdown",
    "profit_factor",
    "win_rate",
    "expectancy",
    "sharpe",
    "sortino",
    "fee_share",
    "slippage_sensitivity",
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(64 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sample_deviation(values: list[float], center: float) -> float:
    if len(values) <= 1:
        return 0.0
    return math.sqrt(sum((value - center) ** 2 for value in values) / (len(values) - 1))


def calculate_metrics(trades: list[dict[str, Any]]) -> dict[str, float | int]:
    profits = [float(trade["net_profit"]) for trade in trades]
    fees = sum(float(trade.get("fee_entry", 0.0)) + float(trade.get("fee_exit", 0.0)) for trade in trades)
    net = sum(profits)
    wins = [profit for profit in profits if profit > 0.0]
    losses = [profit for profit in profits if profit <= 0.0]
    gross_wins = sum(wins)
    gross_losses = abs(sum(losses))

    equity = 0.0
    peak = 0.0
    max_drawdown = 0.0
    for profit in profits:
        equity += profit
        peak = max(peak, equity)
        max_drawdown = max(max_drawdown, peak - equity)

    returns = [
        float(trade["net_profit"]) / max(1.0, float(trade["entry_price"]) * float(trade["quantity"]))
        for trade in trades
    ]
    mean = statistics.fmean(returns) if returns else 0.0
    deviation = sample_deviation(returns, mean)
    negative = [value for value in returns if value < 0.0]
    downside = sample_deviation(negative, 0.0)
    scale = math.sqrt(len(returns)) if returns else 0.0
    raw_profit_factor = gross_wins / gross_losses if gross_losses > 0.0 else math.inf

    return {
        "trades": len(trades),
        "net_profit": net,
        "max_drawdown": max_drawdown,
        "profit_factor": raw_profit_factor if math.isfinite(raw_profit_factor) else 0.0,
        "win_rate": len(wins) / len(trades) * 100.0 if trades else 0.0,
        "expectancy": net / len(trades) if trades else 0.0,
        "sharpe": mean / deviation * scale if deviation > 0.0 else 0.0,
        "sortino": mean / downside * scale if downside > 0.0 else 0.0,
        "fee_share": fees / (abs(net) + fees) if abs(net) + fees > 0.0 else 0.0,
        "slippage_sensitivity": 0.0,
    }


def expected_validation_contract(event_count: int, config: dict[str, Any]) -> dict[str, Any]:
    train_fraction = float(config.get("train_fraction", 0.60))
    holdout_fraction = float(config.get("holdout_fraction", 0.15))
    requested_folds = int(config.get("walk_forward_folds", 3))
    purge_events = int(config.get("purge_events", 0))
    embargo_events = int(config.get("embargo_events", 0))

    holdout_events = max(1, int(event_count * holdout_fraction))
    research_end = event_count - holdout_events
    if research_end <= 2:
        raise ValueError("dataset does not leave enough events for research")
    initial_train = min(max(int(event_count * train_fraction), 2), research_end - 1)
    remaining = research_end - initial_train
    folds = min(requested_folds, remaining)
    if folds <= 0:
        raise ValueError("dataset leaves no validation events")
    fold_width = max(1, remaining // folds)

    boundaries: list[dict[str, int]] = []
    for fold_index in range(folds):
        boundary = initial_train + fold_index * fold_width
        validation_end = research_end if fold_index + 1 == folds else min(
            research_end, initial_train + (fold_index + 1) * fold_width
        )
        train_end = boundary - purge_events if boundary > purge_events else 0
        validation_begin = min(validation_end, boundary + embargo_events)
        boundaries.append(
            {
                "fold_index": fold_index,
                "train_begin": 0,
                "train_end_exclusive": train_end,
                "validation_begin": validation_begin,
                "validation_end_exclusive": validation_end,
                "purged_events": boundary - train_end,
                "embargoed_events": validation_begin - boundary,
            }
        )

    return {
        "schema_version": 1,
        "initial_train_events": initial_train,
        "research_end_index": research_end,
        "holdout_begin_index": research_end,
        "holdout_end_index": event_count,
        "purge_events": purge_events,
        "embargo_events": embargo_events,
        "fold_boundaries": boundaries,
    }


def close_number(actual: Any, expected: Any) -> bool:
    try:
        a = float(actual)
        e = float(expected)
    except (TypeError, ValueError):
        return False
    if math.isinf(a) or math.isinf(e):
        return math.isinf(a) and math.isinf(e) and (a > 0) == (e > 0)
    return math.isclose(a, e, rel_tol=1e-6, abs_tol=1e-6)


def add_violation(report: dict[str, Any], code: str, message: str) -> None:
    report["violations"].append({"code": code, "message": message})


def read_json(path: Path, report: dict[str, Any], code: str) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        add_violation(report, code, f"missing artifact: {path}")
        return None
    except (OSError, json.JSONDecodeError) as error:
        add_violation(report, code, f"cannot read {path}: {error}")
        return None
    if not isinstance(value, dict):
        add_violation(report, code, f"artifact root must be an object: {path}")
        return None
    return value


def resolve_recorded_path(run_dir: Path, recorded: str) -> Path:
    raw = Path(recorded)
    candidates = [raw]
    if raw.is_absolute():
        candidates.extend((run_dir / raw.name, run_dir / "datasets" / raw.name))
    else:
        candidates.extend((run_dir / raw, run_dir / raw.name, run_dir / "datasets" / raw.name))
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[-1]


def load_dataset(path: Path, report: dict[str, Any]) -> list[dict[str, float | int]]:
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            if reader.fieldnames is None or "timestamp_ms" not in reader.fieldnames or "price" not in reader.fieldnames:
                raise ValueError("dataset requires timestamp_ms and price columns")
            events = [
                {
                    "timestamp_ms": int(row["timestamp_ms"]),
                    "price": float(row["price"]),
                    "volume": float(row.get("volume") or 0.0),
                }
                for row in reader
            ]
    except (OSError, ValueError, TypeError) as error:
        add_violation(report, "DATASET_INVALID", f"cannot parse dataset {path}: {error}")
        return []

    if not events:
        add_violation(report, "DATASET_EMPTY", f"dataset is empty: {path}")
        return []
    timestamps = [int(event["timestamp_ms"]) for event in events]
    if any(left > right for left, right in zip(timestamps, timestamps[1:])):
        add_violation(report, "DATASET_ORDER", "materialized dataset timestamps are not chronological")
    return events


def registry_snapshot(path: Path, run_id: str, report: dict[str, Any]) -> dict[str, Any] | None:
    try:
        connection = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    except sqlite3.Error as error:
        add_violation(report, "REGISTRY_MISSING", f"cannot open experiment registry {path}: {error}")
        return None
    try:
        run = connection.execute(
            "SELECT run_id,name,kind,status,started_at_ms,finished_at_ms,git_commit,config_sha256,risk_sha256,output_directory "
            "FROM research_runs WHERE run_id=?",
            (run_id,),
        ).fetchone()
        if run is None:
            add_violation(report, "REGISTRY_RUN_MISSING", f"run {run_id} is not present in experiment registry")
            return None
        datasets = connection.execute(
            "SELECT dataset_id,symbol,source_path,materialized_path,sha256,from_ms,to_ms "
            "FROM research_datasets WHERE run_id=? ORDER BY dataset_id",
            (run_id,),
        ).fetchall()
        artifacts = connection.execute(
            "SELECT path,sha256 FROM research_artifacts WHERE run_id=? ORDER BY path",
            (run_id,),
        ).fetchall()
    except sqlite3.Error as error:
        add_violation(report, "REGISTRY_INVALID", f"cannot query experiment registry: {error}")
        return None
    finally:
        connection.close()

    keys = (
        "run_id",
        "name",
        "kind",
        "status",
        "started_at_ms",
        "finished_at_ms",
        "git_commit",
        "config_sha256",
        "risk_sha256",
        "output_directory",
    )
    return {
        "run": dict(zip(keys, run)),
        "datasets": [
            dict(
                zip(
                    ("dataset_id", "symbol", "source_path", "materialized_path", "sha256", "from_ms", "to_ms"),
                    row,
                )
            )
            for row in datasets
        ],
        "artifacts": {row[0]: row[1] for row in artifacts},
    }


def section_status(report: dict[str, Any], section: str, before: int, payload: dict[str, Any]) -> None:
    payload["status"] = "PASS" if len(report["violations"]) == before else "FAIL"
    report[section] = payload


def validate_manifest_and_provenance(
    run_dir: Path,
    manifest: dict[str, Any],
    registry_path: Path,
    report: dict[str, Any],
) -> tuple[list[dict[str, Any]], dict[str, Any] | None]:
    before = len(report["violations"])
    if manifest.get("manifest_version") != 2:
        add_violation(report, "MANIFEST_VERSION", "manifest_version 2 is required for independent artifact integrity checks")
    if manifest.get("kind") != "research":
        add_violation(report, "UNSUPPORTED_KIND", "independent verifier currently supports managed single-asset research experiments")
    if manifest.get("status") != "completed":
        add_violation(report, "RUN_INCOMPLETE", "experiment must be completed before validation")

    run_id = str(manifest.get("run_id", ""))
    snapshot = registry_snapshot(registry_path, run_id, report)
    if snapshot is not None:
        for key in (
            "run_id",
            "name",
            "kind",
            "status",
            "started_at_ms",
            "finished_at_ms",
            "git_commit",
            "config_sha256",
            "risk_sha256",
        ):
            if snapshot["run"].get(key) != manifest.get(key):
                add_violation(report, "MANIFEST_REGISTRY_MISMATCH", f"manifest field {key} disagrees with registry")

    fixed_inputs = {
        "config_sha256": run_dir / "experiment.json",
        "risk_sha256": run_dir / "risk.json",
    }
    input_results: dict[str, Any] = {}
    for field, path in fixed_inputs.items():
        if not path.exists():
            add_violation(report, "INPUT_MISSING", f"missing immutable input {path.name}")
            continue
        actual = sha256_file(path)
        expected = manifest.get(field)
        input_results[field] = {"expected": expected, "actual": actual}
        if not isinstance(expected, str) or expected != actual:
            add_violation(report, "INPUT_HASH_MISMATCH", f"{path.name} hash does not match manifest {field}")

    manifest_datasets = manifest.get("datasets")
    datasets: list[dict[str, Any]] = manifest_datasets if isinstance(manifest_datasets, list) else []
    if not datasets:
        add_violation(report, "PROVENANCE_MISSING", "manifest contains no dataset provenance")
    registry_datasets = {
        row["dataset_id"]: row for row in (snapshot["datasets"] if snapshot is not None else [])
    }
    dataset_results = []
    for dataset in datasets:
        dataset_id = str(dataset.get("dataset_id", ""))
        recorded_path = str(dataset.get("materialized_path", ""))
        expected_hash = dataset.get("sha256")
        if not dataset_id or not recorded_path or not isinstance(expected_hash, str) or not expected_hash:
            add_violation(report, "PROVENANCE_MISSING", "dataset provenance is incomplete")
            continue
        resolved = resolve_recorded_path(run_dir, recorded_path)
        if not resolved.exists():
            add_violation(report, "DATASET_MISSING", f"materialized dataset is missing: {recorded_path}")
            continue
        actual_hash = sha256_file(resolved)
        dataset_results.append(
            {"dataset_id": dataset_id, "path": str(resolved), "expected_sha256": expected_hash, "actual_sha256": actual_hash}
        )
        if actual_hash != expected_hash:
            add_violation(report, "DATASET_HASH_MISMATCH", f"dataset hash mismatch for {dataset_id}")
        registry_row = registry_datasets.get(dataset_id)
        if snapshot is not None:
            if registry_row is None:
                add_violation(report, "REGISTRY_DATASET_MISSING", f"dataset {dataset_id} is missing from registry")
            else:
                for key in ("symbol", "source_path", "materialized_path", "sha256", "from_ms", "to_ms"):
                    if registry_row.get(key) != dataset.get(key):
                        add_violation(report, "DATASET_REGISTRY_MISMATCH", f"dataset {dataset_id} field {key} disagrees with registry")

    artifacts = manifest.get("artifacts")
    artifact_paths: list[str] = artifacts if isinstance(artifacts, list) else []
    artifact_hashes = manifest.get("artifact_sha256")
    if not isinstance(artifact_hashes, dict):
        add_violation(report, "PROVENANCE_MISSING", "manifest artifact_sha256 map is missing")
        artifact_hashes = {}
    artifact_results = []
    for recorded in artifact_paths:
        if not isinstance(recorded, str):
            add_violation(report, "MANIFEST_INVALID", "manifest artifact path is not a string")
            continue
        resolved = resolve_recorded_path(run_dir, recorded)
        if not resolved.exists():
            add_violation(report, "ARTIFACT_MISSING", f"artifact is missing: {recorded}")
            continue
        actual_hash = sha256_file(resolved)
        expected_hash = artifact_hashes.get(recorded)
        artifact_results.append({"path": recorded, "expected_sha256": expected_hash, "actual_sha256": actual_hash})
        if not isinstance(expected_hash, str) or expected_hash != actual_hash:
            add_violation(report, "ARTIFACT_HASH_MISMATCH", f"artifact hash mismatch for {recorded}")
        if snapshot is not None:
            registry_hash = snapshot["artifacts"].get(recorded)
            if registry_hash is None:
                add_violation(report, "REGISTRY_ARTIFACT_MISSING", f"artifact is missing from registry: {recorded}")
            elif registry_hash != actual_hash:
                add_violation(report, "ARTIFACT_REGISTRY_MISMATCH", f"artifact hash disagrees with registry: {recorded}")

    section_status(
        report,
        "provenance",
        before,
        {"inputs": input_results, "datasets": dataset_results, "artifacts": artifact_results},
    )
    return datasets, snapshot


def validate_splits(
    events: list[dict[str, float | int]],
    research_config: dict[str, Any],
    research: dict[str, Any],
    report: dict[str, Any],
) -> dict[str, Any] | None:
    before = len(report["violations"])
    try:
        expected = expected_validation_contract(len(events), research_config)
    except (TypeError, ValueError) as error:
        add_violation(report, "SPLIT_CONFIG_INVALID", str(error))
        section_status(report, "split_validation", before, {})
        return None

    reported_contract = research.get("validation_contract")
    if not isinstance(reported_contract, dict):
        add_violation(report, "SPLIT_METADATA_MISSING", "research artifact has no validation_contract")
    elif reported_contract != expected:
        add_violation(report, "SPLIT_METADATA_MISMATCH", "persisted validation contract does not match independently recomputed boundaries")

    expected_scalars = {
        "events": len(events),
        "research_events": expected["research_end_index"],
        "holdout_events": len(events) - expected["holdout_begin_index"],
        "folds": len(expected["fold_boundaries"]),
    }
    for key, value in expected_scalars.items():
        if research.get(key) != value:
            add_violation(report, "SPLIT_SUMMARY_MISMATCH", f"research field {key} does not match independently recomputed split")

    for fold in expected["fold_boundaries"]:
        train_begin = fold["train_begin"]
        train_end = fold["train_end_exclusive"]
        validation_begin = fold["validation_begin"]
        validation_end = fold["validation_end_exclusive"]
        if not (0 <= train_begin <= train_end <= validation_begin <= validation_end <= expected["research_end_index"]):
            add_violation(report, "FOLD_OVERLAP", f"fold {fold['fold_index']} has overlapping or invalid boundaries")
    if expected["research_end_index"] != expected["holdout_begin_index"]:
        add_violation(report, "HOLDOUT_BOUNDARY", "research and holdout regions do not meet at one exclusive boundary")
    if expected["holdout_begin_index"] >= expected["holdout_end_index"]:
        add_violation(report, "HOLDOUT_BOUNDARY", "holdout region is empty or reversed")

    section_status(
        report,
        "split_validation",
        before,
        {
            "event_count": len(events),
            "expected_contract": expected,
            "reported_contract": reported_contract,
        },
    )
    return expected


def parse_trials(path: Path, report: dict[str, Any]) -> tuple[list[str], dict[int, dict[str, str]]]:
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            headers = reader.fieldnames or []
            rows = {int(row["trial_id"]): row for row in reader}
        return headers, rows
    except (OSError, KeyError, TypeError, ValueError) as error:
        add_violation(report, "TRIALS_INVALID", f"cannot parse trials.csv: {error}")
        return [], {}


def validate_selection_and_leakage(
    research: dict[str, Any],
    trials_path: Path,
    report: dict[str, Any],
) -> None:
    before = len(report["violations"])
    headers, rows = parse_trials(trials_path, report)
    if any("holdout" in header.lower() for header in headers):
        add_violation(report, "HOLDOUT_LEAKAGE_METADATA", "trial-search artifact contains holdout-labelled columns")

    leaderboard = research.get("leaderboard")
    if not isinstance(leaderboard, list):
        add_violation(report, "LEADERBOARD_INVALID", "research leaderboard is missing")
        leaderboard = []

    for item in leaderboard:
        if not isinstance(item, dict):
            add_violation(report, "LEADERBOARD_INVALID", "leaderboard entry is not an object")
            continue
        trial_id = item.get("trial_id")
        if not isinstance(trial_id, int) or trial_id not in rows:
            add_violation(report, "LEADERBOARD_TRIAL_MISSING", f"leaderboard trial {trial_id!r} is absent from trials.csv")
            continue
        row = rows[trial_id]
        if bool(item.get("eligible")) != (row.get("eligible") == "1"):
            add_violation(report, "LEADERBOARD_MISMATCH", f"trial {trial_id} eligibility differs from trials.csv")
        if not close_number(item.get("validation_score"), row.get("validation_score")):
            add_violation(report, "LEADERBOARD_MISMATCH", f"trial {trial_id} validation score differs from trials.csv")
        parameters = item.get("parameters") if isinstance(item.get("parameters"), dict) else {}
        expected_parameters = {
            "lookback": int(row["lookback"]),
            "entry_threshold": float(row["entry_threshold"]),
            "stop_loss_percent": float(row["stop_loss_percent"]),
            "take_profit_percent": float(row["take_profit_percent"]),
            "slippage_percent": float(row["slippage_percent"]),
        }
        for key, expected in expected_parameters.items():
            actual = parameters.get(key)
            if key == "lookback":
                equal = actual == expected
            else:
                equal = close_number(actual, expected)
            if not equal:
                add_violation(report, "LEADERBOARD_MISMATCH", f"trial {trial_id} parameter {key} differs from trials.csv")

    selected = next((entry for entry in leaderboard if isinstance(entry, dict) and entry.get("eligible") is True), None)
    holdout_evaluated = research.get("holdout_evaluated") is True
    if selected is None and holdout_evaluated:
        add_violation(report, "HOLDOUT_SELECTION_INVALID", "holdout was evaluated without an eligible validation-selected candidate")
    if selected is not None:
        selected_parameters = research.get("selected_parameters")
        if selected_parameters != selected.get("parameters"):
            add_violation(report, "HOLDOUT_SELECTION_INVALID", "selected_parameters do not match the first eligible validation-ranked candidate")
        if not holdout_evaluated:
            add_violation(report, "HOLDOUT_SELECTION_INVALID", "eligible selected candidate exists but holdout is marked unevaluated")

    section_status(
        report,
        "leakage_checks",
        before,
        {
            "trial_rows": len(rows),
            "leaderboard_rows": len(leaderboard),
            "holdout_evaluated": holdout_evaluated,
            "selected_trial_id": selected.get("trial_id") if selected else None,
        },
    )


def validate_metrics(
    events: list[dict[str, float | int]],
    expected_contract: dict[str, Any] | None,
    research: dict[str, Any],
    visualization: dict[str, Any],
    report: dict[str, Any],
) -> None:
    before = len(report["violations"])
    if visualization.get("schema_version") != 2:
        add_violation(report, "TRADE_EVIDENCE_VERSION", "research visualization schema_version 2 is required")
    trades = visualization.get("trade_records")
    if not isinstance(trades, list):
        add_violation(report, "TRADE_EVIDENCE_MISSING", "holdout trade_records are missing")
        trades = []

    recomputed: dict[str, float | int] = {}
    try:
        recomputed = calculate_metrics(trades)
    except (KeyError, TypeError, ValueError) as error:
        add_violation(report, "TRADE_EVIDENCE_INVALID", f"cannot recompute metrics from holdout trades: {error}")

    reported = research.get("final_holdout") if research.get("holdout_evaluated") is True else {}
    if research.get("holdout_evaluated") is True and not isinstance(reported, dict):
        add_violation(report, "METRIC_REPORT_MISSING", "final_holdout metrics are missing")
        reported = {}

    comparisons: dict[str, Any] = {}
    if recomputed and isinstance(reported, dict):
        for key in METRIC_KEYS:
            expected = recomputed.get(key)
            actual = reported.get(key)
            equal = (actual == expected) if key == "trades" else close_number(actual, expected)
            comparisons[key] = {"reported": actual, "recomputed": expected, "match": equal}
            if not equal:
                add_violation(report, "METRIC_MISMATCH", f"final holdout metric {key} does not match independent recomputation")

    if visualization.get("trades") != len(trades):
        add_violation(report, "TRADE_EVIDENCE_MISMATCH", "visualization trade count does not match trade_records")
    if recomputed and not close_number(visualization.get("net_profit"), recomputed.get("net_profit")):
        add_violation(report, "TRADE_EVIDENCE_MISMATCH", "visualization net_profit does not match trade_records")

    if expected_contract is not None and events:
        holdout_begin = int(expected_contract["holdout_begin_index"])
        holdout_start_ms = int(events[holdout_begin]["timestamp_ms"])
        dataset_end_ms = int(events[-1]["timestamp_ms"])
        for index, trade in enumerate(trades):
            try:
                entry_ms = int(trade["entry_time_ms"])
                exit_ms = int(trade["exit_time_ms"])
            except (KeyError, TypeError, ValueError):
                add_violation(report, "TRADE_EVIDENCE_INVALID", f"trade record {index} has invalid timestamps")
                continue
            if entry_ms < holdout_start_ms:
                add_violation(report, "HOLDOUT_LEAKAGE", f"trade record {index} entered before the final holdout boundary")
            if exit_ms < entry_ms or exit_ms > dataset_end_ms:
                add_violation(report, "TRADE_EVIDENCE_INVALID", f"trade record {index} exits outside the deterministic dataset window")

    section_status(
        report,
        "metric_validation",
        before,
        {"reported": reported, "recomputed": recomputed, "comparisons": comparisons},
    )


def deterministic_projection(run_dir: Path) -> dict[str, Any]:
    research = json.loads((run_dir / "research.json").read_text(encoding="utf-8"))
    visualization = json.loads((run_dir / "research-visualization.json").read_text(encoding="utf-8"))
    manifest = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
    research_keys = (
        "symbol",
        "objective",
        "events",
        "research_events",
        "holdout_events",
        "folds",
        "trials",
        "validation_contract",
        "holdout_evaluated",
        "leaderboard",
        "selected_parameters",
        "final_holdout",
        "final_holdout_score",
        "bootstrap_net_profit",
        "monte_carlo",
        "holdout_regimes",
    )
    visualization_keys = (
        "schema_version",
        "symbol",
        "objective",
        "holdout_evaluated",
        "equity_curve",
        "drawdown_curve",
        "trade_records",
        "trades",
        "net_profit",
    )
    return {
        "research": {key: research.get(key) for key in research_keys if key in research},
        "trials_csv": (run_dir / "trials.csv").read_text(encoding="utf-8"),
        "visualization": {key: visualization.get(key) for key in visualization_keys if key in visualization},
        "dataset_hashes": sorted(
            str(dataset.get("sha256", ""))
            for dataset in manifest.get("datasets", [])
            if isinstance(dataset, dict)
        ),
    }


def validate_reproduction(run_dir: Path, reference_run: Path | None, report: dict[str, Any]) -> None:
    before = len(report["violations"])
    if reference_run is None:
        report["reproduction"] = {"status": "NOT_CHECKED", "reference_run": None}
        return
    try:
        current = deterministic_projection(run_dir)
        reference = deterministic_projection(reference_run)
    except (OSError, json.JSONDecodeError, TypeError) as error:
        add_violation(report, "REPRODUCTION_EVIDENCE_INVALID", f"cannot compare reproduction evidence: {error}")
        section_status(report, "reproduction", before, {"reference_run": str(reference_run)})
        return
    if current != reference:
        add_violation(report, "REPRODUCTION_MISMATCH", "deterministic research outputs differ from the reference run")
    section_status(report, "reproduction", before, {"reference_run": str(reference_run)})


def infer_registry(run_dir: Path, experiment: dict[str, Any] | None) -> Path:
    if experiment is not None:
        configured = experiment.get("registry_path")
        if isinstance(configured, str) and configured:
            path = Path(configured)
            if path.exists():
                return path
            candidate = run_dir / path
            if candidate.exists():
                return candidate
    return Path("log/experiments.sqlite3")


def validate_run(
    run_dir: Path,
    registry_path: Path | None = None,
    reference_run: Path | None = None,
) -> dict[str, Any]:
    run_dir = run_dir.resolve()
    report: dict[str, Any] = {
        "schema_version": REPORT_SCHEMA_VERSION,
        "validation_kind": "sentum.research.independent",
        "validated_at_utc": datetime.now(timezone.utc).isoformat(),
        "run_directory": str(run_dir),
        "run_id": run_dir.name,
        "status": "FAIL",
        "violations": [],
        "limitations": [
            "PASS validates persisted provenance, split boundaries, trade-derived metrics and optional deterministic reproduction evidence.",
            "PASS does not imply future profitability, live-trading suitability or automatic model promotion.",
        ],
    }

    manifest = read_json(run_dir / "manifest.json", report, "MANIFEST_INVALID")
    experiment = read_json(run_dir / "experiment.json", report, "EXPERIMENT_SPEC_INVALID")
    research_config = read_json(run_dir / "research-config.json", report, "RESEARCH_CONFIG_INVALID")
    research = read_json(run_dir / "research.json", report, "RESEARCH_ARTIFACT_INVALID")
    visualization = read_json(run_dir / "research-visualization.json", report, "TRADE_EVIDENCE_INVALID")

    if manifest is None:
        report["status"] = "FAIL"
        return report
    report["run_id"] = str(manifest.get("run_id", run_dir.name))
    report["code_identity"] = {"git_commit": manifest.get("git_commit")}

    effective_registry = registry_path if registry_path is not None else infer_registry(run_dir, experiment)
    datasets, _ = validate_manifest_and_provenance(run_dir, manifest, effective_registry, report)

    events: list[dict[str, float | int]] = []
    if datasets:
        first = datasets[0]
        recorded_path = first.get("materialized_path")
        if isinstance(recorded_path, str) and recorded_path:
            events = load_dataset(resolve_recorded_path(run_dir, recorded_path), report)

    expected_contract = None
    if research_config is not None and research is not None and events:
        expected_contract = validate_splits(events, research_config, research, report)
        validate_selection_and_leakage(research, run_dir / "trials.csv", report)

    if research is not None and visualization is not None:
        validate_metrics(events, expected_contract, research, visualization, report)

    validate_reproduction(run_dir, reference_run.resolve() if reference_run is not None else None, report)
    report["status"] = "PASS" if not report["violations"] else "FAIL"
    return report


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_directory", type=Path)
    parser.add_argument("--registry", type=Path, default=None)
    parser.add_argument("--reference-run", type=Path, default=None)
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--no-write", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = validate_run(args.run_directory, args.registry, args.reference_run)
    output = args.output or args.run_directory / "research-validation.json"
    if not args.no_write:
        write_report(output, report)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
