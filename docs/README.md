# Sentum documentation

This directory contains the current functional documentation for Sentum. Documents are organized by subsystem rather than implementation history.

## Runtime and operation

- [Trading console](TRADING_CONSOLE.md) — interactive Paper/Testnet terminal views and Paper controls.
- [Web dashboard](DASHBOARD.md) — local read-only browser dashboard, bind configuration and API.
- [Runtime performance](PERFORMANCE.md) — market-data hot path, persistence architecture, telemetry and benchmarks.
- [Execution safety](LIVE_TRADING_SAFETY.md) — supported execution boundaries and operational safety expectations.

## Research

- [Quant research](RESEARCH.md) — deterministic parameter research and output artifacts.
- [Research validation and robustness](RESEARCH_ROBUSTNESS.md) — walk-forward validation, holdout, bootstrap, Monte Carlo and selection controls.
- [Strategy and portfolio research](STRATEGY_PORTFOLIO_RESEARCH.md) — strategy framework, ensembles, multi-timeframe and portfolio risk.
- [Experiment and dataset management](EXPERIMENT_DATASET_MANAGEMENT.md) — dataset catalogs, immutable slices, hashes and experiment registry.
- [Research dashboard](ADVANCED_RESEARCH_DASHBOARD.md) — experiment comparison and research visualization in the web UI.

## Model lifecycle and Testnet

- [Shadow trading and model promotion](SHADOW_TRADING_MODEL_PROMOTION.md) — research-to-Testnet model lifecycle and promotion gates.
- [Binance Spot Testnet runtime](TESTNET_RUNTIME.md) — exchange-confirmed order state and Testnet operation.
- [Account reconciliation and recovery](ACCOUNT_RECONCILIATION.md) — startup reconciliation, recovery and audit state.

For project overview, build instructions and common commands, start with the repository [README](../README.md).
