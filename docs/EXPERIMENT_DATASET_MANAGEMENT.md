# Experiment and dataset management

Sentum records research runs as reproducible experiments rather than anonymous one-off backtests. Each run is tied to explicit dataset slices, configuration, risk assumptions, source revision and immutable result artifacts.

## Dataset catalog

Datasets are described in a catalog such as `config/datasets.example.json`. Entries define a stable dataset ID, symbol, source path, interval, available time range and optional tags. Experiments refer to dataset IDs and may select a bounded `from_ms` / `to_ms` range.

Before research begins, Sentum materializes the selected range into the experiment directory and hashes the exact bytes used for the run. Research therefore depends on a known immutable input instead of whatever a source CSV happens to contain later.

## Experiment runner

Use the experiment executable for managed runs:

```bash
./build/sentum_experiment --list-datasets config/datasets.json
./build/sentum_experiment config/experiment.json
```

Both single-asset and portfolio experiment specifications are supported. The runner delegates strategy evaluation to the normal research and portfolio-research components rather than implementing a separate backtester.

## Run directory

Each experiment receives a unique directory under:

```text
log/experiments/<run-id>/
```

Depending on the experiment type it can contain:

```text
manifest.json
experiment.json
dataset-catalog.json
risk.json
research-config.json
portfolio-config.json
datasets/
research.json
trials.csv
research-visualization.json
portfolio-research.json
```

## Provenance

The manifest records enough information to explain why two runs differ, including:

- embedded Git commit
- experiment/config hashes
- risk-config hash
- dataset ID, symbol and selected time range
- SHA-256 for each materialized dataset
- generated artifact paths and hashes
- start/finish timestamps and final status

A run is recorded as `started` before research begins and transitions to `completed` or `failed`. Failed experiments remain visible for audit/debugging.

## Experiment registry

`log/experiments.sqlite3` stores normalized run metadata in tables including:

- `research_runs`
- `research_datasets`
- `research_artifacts`

The web research dashboard uses this registry for history, comparisons and artifact lookup.

## Reproducibility

A research result should be treated as identified by the combination of source revision, experiment specification, risk assumptions and exact dataset hashes. Re-running with the same inputs is expected to produce deterministic trial output where the underlying research path is deterministic.
