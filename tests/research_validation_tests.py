#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import copy
import csv
import importlib.util
import json
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "verify_research.py"
SPEC = importlib.util.spec_from_file_location("sentum_verify_research", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot load verify_research.py")
verify_research = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = verify_research
SPEC.loader.exec_module(verify_research)


def codes(report: dict) -> set[str]:
    return {str(item.get("code")) for item in report.get("violations", [])}


class ResearchValidationFixture:
    def __init__(self, root: Path, name: str):
        self.root = root
        self.run_dir = root / name
        self.run_dir.mkdir(parents=True)
        (self.run_dir / "datasets").mkdir()
        self.registry = root / f"{name}-registry.sqlite3"
        self._write_files()
        self._write_registry()

    def _write_files(self) -> None:
        base_ms = 1_710_000_000_000
        dataset = self.run_dir / "datasets" / "dataset.csv"
        with dataset.open("w", encoding="utf-8", newline="") as handle:
            handle.write("timestamp_ms,price,volume\n")
            for index in range(100):
                handle.write(f"{base_ms + index * 1000},{100.0 + index * 0.01:.8f},1.0\n")

        experiment = {
            "name": "canonical-validation-fixture",
            "kind": "research",
            "registry_path": str(self.registry),
        }
        risk = {"fixture": "risk"}
        research_config = {
            "dataset": str(dataset),
            "symbol": "BTCUSDT",
            "objective": "sharpe",
            "train_fraction": 0.60,
            "holdout_fraction": 0.20,
            "walk_forward_folds": 2,
            "purge_events": 2,
            "embargo_events": 1,
            "min_validation_trades": 0,
            "max_trials": 1,
            "leaderboard_size": 1,
            "random_seed": 7,
            "grid": {"lookback": [5], "entry_threshold": [0.001]},
        }
        catalog = {
            "datasets": [
                {
                    "id": "canonical-dataset",
                    "symbol": "BTCUSDT",
                    "path": "source.csv",
                    "interval": "1s",
                    "start_ms": base_ms,
                    "end_ms": base_ms + 99_000,
                }
            ]
        }

        for name, value in (
            ("experiment.json", experiment),
            ("risk.json", risk),
            ("research-config.json", research_config),
            ("dataset-catalog.json", catalog),
        ):
            (self.run_dir / name).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

        contract = verify_research.expected_validation_contract(100, research_config)
        parameters = {
            "lookback": 5,
            "entry_threshold": 0.001,
            "stop_loss_percent": 0.01,
            "take_profit_percent": 0.03,
            "slippage_percent": 0.0005,
        }
        trades = [
            {
                "symbol": "BTCUSDT",
                "entry_time_ms": base_ms + 81_000,
                "exit_time_ms": base_ms + 84_000,
                "entry_price": 100.81,
                "exit_price": 101.10,
                "quantity": 10.0,
                "gross_profit": 11.5,
                "net_profit": 10.0,
                "fee_entry": 0.75,
                "fee_exit": 0.75,
                "close_reason": "take_profit",
            },
            {
                "symbol": "BTCUSDT",
                "entry_time_ms": base_ms + 86_000,
                "exit_time_ms": base_ms + 90_000,
                "entry_price": 100.86,
                "exit_price": 100.70,
                "quantity": 10.0,
                "gross_profit": -3.5,
                "net_profit": -5.0,
                "fee_entry": 0.75,
                "fee_exit": 0.75,
                "close_reason": "stop_loss",
            },
        ]
        metrics = verify_research.calculate_metrics(trades)
        leaderboard = [
            {
                "trial_id": 1,
                "parameters": parameters,
                "train": copy.deepcopy(metrics),
                "validation": copy.deepcopy(metrics),
                "train_score": 1.6,
                "validation_score": 1.5,
                "overfit_gap": 0.1,
                "parameter_stability_score": 1.0,
                "deflated_sharpe": 1.4,
                "eligible": True,
            }
        ]
        research = {
            "dataset": str(dataset),
            "symbol": "BTCUSDT",
            "objective": "sharpe",
            "generated_at_ms": base_ms + 200_000,
            "events": 100,
            "research_events": contract["research_end_index"],
            "holdout_events": 100 - contract["holdout_begin_index"],
            "folds": len(contract["fold_boundaries"]),
            "trials": 1,
            "holdout_evaluated": True,
            "validation_contract": contract,
            "leaderboard": leaderboard,
            "selected_parameters": parameters,
            "final_holdout": metrics,
            "final_holdout_score": float(metrics["sharpe"]),
            "bootstrap_net_profit": {"lower": 0.0, "median": 5.0, "upper": 10.0},
            "monte_carlo": {
                "samples": 10,
                "net_profit": {"lower": -5.0, "median": 5.0, "upper": 20.0},
                "max_drawdown": {"lower": 0.0, "median": 5.0, "upper": 10.0},
                "probability_of_loss": 0.2,
            },
            "holdout_regimes": [],
        }
        (self.run_dir / "research.json").write_text(json.dumps(research, indent=2) + "\n", encoding="utf-8")

        with (self.run_dir / "trials.csv").open("w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(
                [
                    "trial_id",
                    "lookback",
                    "entry_threshold",
                    "stop_loss_percent",
                    "take_profit_percent",
                    "slippage_percent",
                    "eligible",
                    "train_score",
                    "validation_score",
                    "overfit_gap",
                    "parameter_stability_score",
                    "deflated_sharpe",
                    "train_trades",
                    "validation_trades",
                    "train_net_profit",
                    "validation_net_profit",
                    "validation_max_drawdown",
                    "validation_sharpe",
                    "validation_sortino",
                ]
            )
            writer.writerow(
                [
                    1,
                    5,
                    0.001,
                    0.01,
                    0.03,
                    0.0005,
                    1,
                    1.6,
                    1.5,
                    0.1,
                    1.0,
                    1.4,
                    2,
                    2,
                    5.0,
                    5.0,
                    metrics["max_drawdown"],
                    metrics["sharpe"],
                    metrics["sortino"],
                ]
            )

        visualization = {
            "schema_version": 2,
            "symbol": "BTCUSDT",
            "objective": "sharpe",
            "holdout_evaluated": True,
            "equity_curve": [
                {"ts": base_ms + 80_000, "equity": 0.0},
                {"ts": base_ms + 84_000, "equity": 10.0},
                {"ts": base_ms + 90_000, "equity": 5.0},
            ],
            "drawdown_curve": [
                {"ts": base_ms + 84_000, "drawdown": 0.0},
                {"ts": base_ms + 90_000, "drawdown": 5.0},
            ],
            "trade_records": trades,
            "trades": len(trades),
            "net_profit": metrics["net_profit"],
        }
        (self.run_dir / "research-visualization.json").write_text(
            json.dumps(visualization, indent=2) + "\n", encoding="utf-8"
        )

        self.artifacts = [
            "experiment.json",
            "dataset-catalog.json",
            "risk.json",
            "research.json",
            "trials.csv",
            "research-visualization.json",
            "research-config.json",
        ]
        self.dataset_record = {
            "dataset_id": "canonical-dataset",
            "symbol": "BTCUSDT",
            "source_path": "source.csv",
            "materialized_path": "datasets/dataset.csv",
            "sha256": verify_research.sha256_file(dataset),
            "from_ms": base_ms,
            "to_ms": base_ms + 99_000,
        }
        self._write_manifest()

    def _write_manifest(self) -> None:
        experiment_path = self.run_dir / "experiment.json"
        risk_path = self.run_dir / "risk.json"
        manifest = {
            "manifest_version": 2,
            "run_id": self.run_dir.name,
            "name": "canonical-validation-fixture",
            "kind": "research",
            "status": "completed",
            "started_at_ms": 1_710_000_000_000,
            "finished_at_ms": 1_710_000_200_000,
            "git_commit": "0123456789abcdef",
            "config_sha256": verify_research.sha256_file(experiment_path),
            "risk_sha256": verify_research.sha256_file(risk_path),
            "output_directory": str(self.run_dir),
            "datasets": [self.dataset_record],
            "artifacts": self.artifacts,
            "artifact_sha256": {
                artifact: verify_research.sha256_file(self.run_dir / artifact)
                for artifact in self.artifacts
                if (self.run_dir / artifact).exists()
            },
        }
        (self.run_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    def _write_registry(self) -> None:
        manifest = json.loads((self.run_dir / "manifest.json").read_text(encoding="utf-8"))
        with sqlite3.connect(self.registry) as connection:
            connection.executescript(
                """
                CREATE TABLE research_runs(
                    run_id TEXT PRIMARY KEY,name TEXT NOT NULL,kind TEXT NOT NULL,status TEXT NOT NULL,
                    started_at_ms INTEGER NOT NULL,finished_at_ms INTEGER NOT NULL DEFAULT 0,
                    git_commit TEXT,config_sha256 TEXT,risk_sha256 TEXT,output_directory TEXT
                );
                CREATE TABLE research_datasets(
                    run_id TEXT NOT NULL,dataset_id TEXT NOT NULL,symbol TEXT NOT NULL,source_path TEXT NOT NULL,
                    materialized_path TEXT NOT NULL,sha256 TEXT NOT NULL,from_ms INTEGER,to_ms INTEGER,
                    PRIMARY KEY(run_id,dataset_id)
                );
                CREATE TABLE research_artifacts(
                    run_id TEXT NOT NULL,path TEXT NOT NULL,sha256 TEXT NOT NULL,PRIMARY KEY(run_id,path)
                );
                """
            )
            connection.execute(
                "INSERT INTO research_runs VALUES(?,?,?,?,?,?,?,?,?,?)",
                (
                    manifest["run_id"],
                    manifest["name"],
                    manifest["kind"],
                    manifest["status"],
                    manifest["started_at_ms"],
                    manifest["finished_at_ms"],
                    manifest["git_commit"],
                    manifest["config_sha256"],
                    manifest["risk_sha256"],
                    manifest["output_directory"],
                ),
            )
            dataset = manifest["datasets"][0]
            connection.execute(
                "INSERT INTO research_datasets VALUES(?,?,?,?,?,?,?,?)",
                (
                    manifest["run_id"],
                    dataset["dataset_id"],
                    dataset["symbol"],
                    dataset["source_path"],
                    dataset["materialized_path"],
                    dataset["sha256"],
                    dataset["from_ms"],
                    dataset["to_ms"],
                ),
            )
            for artifact, digest in manifest["artifact_sha256"].items():
                connection.execute(
                    "INSERT INTO research_artifacts VALUES(?,?,?)",
                    (manifest["run_id"], artifact, digest),
                )

    def refresh_integrity(self) -> None:
        self._write_manifest()
        manifest = json.loads((self.run_dir / "manifest.json").read_text(encoding="utf-8"))
        with sqlite3.connect(self.registry) as connection:
            connection.execute(
                "UPDATE research_runs SET name=?,kind=?,status=?,started_at_ms=?,finished_at_ms=?,git_commit=?,config_sha256=?,risk_sha256=?,output_directory=? WHERE run_id=?",
                (
                    manifest["name"],
                    manifest["kind"],
                    manifest["status"],
                    manifest["started_at_ms"],
                    manifest["finished_at_ms"],
                    manifest["git_commit"],
                    manifest["config_sha256"],
                    manifest["risk_sha256"],
                    manifest["output_directory"],
                    manifest["run_id"],
                ),
            )
            connection.execute("DELETE FROM research_artifacts WHERE run_id=?", (manifest["run_id"],))
            for artifact, digest in manifest["artifact_sha256"].items():
                connection.execute(
                    "INSERT INTO research_artifacts VALUES(?,?,?)",
                    (manifest["run_id"], artifact, digest),
                )

    def validate(self, reference: "ResearchValidationFixture | None" = None) -> dict:
        return verify_research.validate_run(
            self.run_dir,
            self.registry,
            reference.run_dir if reference is not None else None,
        )


class IndependentResearchValidationTests(unittest.TestCase):
    def test_valid_experiment_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = ResearchValidationFixture(Path(directory), "valid")
            report = fixture.validate()
            self.assertEqual(report["status"], "PASS", report["violations"])
            self.assertEqual(report["provenance"]["status"], "PASS")
            self.assertEqual(report["split_validation"]["status"], "PASS")
            self.assertEqual(report["metric_validation"]["status"], "PASS")

    def test_dataset_hash_mismatch_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = ResearchValidationFixture(Path(directory), "dataset-tamper")
            with (fixture.run_dir / "datasets" / "dataset.csv").open("a", encoding="utf-8") as handle:
                handle.write("1710000100000,200.0,1.0\n")
            report = fixture.validate()
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("DATASET_HASH_MISMATCH", codes(report))

    def test_metric_mismatch_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = ResearchValidationFixture(Path(directory), "metric-tamper")
            path = fixture.run_dir / "research.json"
            research = json.loads(path.read_text(encoding="utf-8"))
            research["final_holdout"]["net_profit"] += 123.0
            path.write_text(json.dumps(research, indent=2) + "\n", encoding="utf-8")
            report = fixture.validate()
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("METRIC_MISMATCH", codes(report))

    def test_overlapping_fold_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = ResearchValidationFixture(Path(directory), "fold-overlap")
            path = fixture.run_dir / "research.json"
            research = json.loads(path.read_text(encoding="utf-8"))
            fold = research["validation_contract"]["fold_boundaries"][0]
            fold["validation_begin"] = fold["train_end_exclusive"] - 1
            path.write_text(json.dumps(research, indent=2) + "\n", encoding="utf-8")
            report = fixture.validate()
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("FOLD_OVERLAP", codes(report))

    def test_missing_purge_embargo_metadata_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = ResearchValidationFixture(Path(directory), "missing-boundary-metadata")
            path = fixture.run_dir / "research.json"
            research = json.loads(path.read_text(encoding="utf-8"))
            research["validation_contract"].pop("purge_events")
            research["validation_contract"].pop("embargo_events")
            path.write_text(json.dumps(research, indent=2) + "\n", encoding="utf-8")
            report = fixture.validate()
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("SPLIT_METADATA_MISSING", codes(report))

    def test_invalid_holdout_boundary_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = ResearchValidationFixture(Path(directory), "holdout-boundary")
            path = fixture.run_dir / "research.json"
            research = json.loads(path.read_text(encoding="utf-8"))
            research["validation_contract"]["holdout_begin_index"] -= 1
            path.write_text(json.dumps(research, indent=2) + "\n", encoding="utf-8")
            report = fixture.validate()
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("HOLDOUT_BOUNDARY", codes(report))

    def test_malformed_manifest_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = ResearchValidationFixture(Path(directory), "malformed-manifest")
            (fixture.run_dir / "manifest.json").write_text("{broken-json", encoding="utf-8")
            report = fixture.validate()
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("MANIFEST_INVALID", codes(report))

    def test_missing_artifact_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = ResearchValidationFixture(Path(directory), "missing-artifact")
            (fixture.run_dir / "research-visualization.json").unlink()
            report = fixture.validate()
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("ARTIFACT_MISSING", codes(report))

    def test_modified_manifest_is_detected_against_registry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = ResearchValidationFixture(Path(directory), "manifest-tamper")
            path = fixture.run_dir / "manifest.json"
            manifest = json.loads(path.read_text(encoding="utf-8"))
            manifest["git_commit"] = "tampered"
            path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            report = fixture.validate()
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("MANIFEST_REGISTRY_MISMATCH", codes(report))

    def test_holdout_trade_before_boundary_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = ResearchValidationFixture(Path(directory), "holdout-trade-tamper")
            path = fixture.run_dir / "research-visualization.json"
            visualization = json.loads(path.read_text(encoding="utf-8"))
            visualization["trade_records"][0]["entry_time_ms"] = 1_710_000_070_000
            path.write_text(json.dumps(visualization, indent=2) + "\n", encoding="utf-8")
            report = fixture.validate()
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("HOLDOUT_LEAKAGE", codes(report))

    def test_identical_deterministic_projection_passes_reproduction(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = ResearchValidationFixture(root, "reference")
            rerun = ResearchValidationFixture(root, "rerun")
            report = rerun.validate(reference)
            self.assertEqual(report["status"], "PASS", report["violations"])
            self.assertEqual(report["reproduction"]["status"], "PASS")

    def test_reproduction_mismatch_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = ResearchValidationFixture(root, "reference")
            rerun = ResearchValidationFixture(root, "rerun")
            path = rerun.run_dir / "research-visualization.json"
            visualization = json.loads(path.read_text(encoding="utf-8"))
            visualization["equity_curve"][1]["equity"] += 0.25
            path.write_text(json.dumps(visualization, indent=2) + "\n", encoding="utf-8")
            rerun.refresh_integrity()
            report = rerun.validate(reference)
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("REPRODUCTION_MISMATCH", codes(report))


if __name__ == "__main__":
    unittest.main()
