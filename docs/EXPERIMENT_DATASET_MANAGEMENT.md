# Sentum Experiment & Dataset Management

Phase 13 adds a reproducible experiment layer above the existing single-asset and portfolio research engines.

## Dataset catalog

Historical files are referenced through stable dataset IDs instead of being hard-coded into every experiment.

Example:

```json
{
  "datasets": [
    {
      "id": "btc-usdt-2025",
      "symbol": "BTCUSDT",
      "path": "data/btcusdt-2025.csv",
      "interval": "1s",
      "start_ms": 1735689600000,
      "end_ms": 1767225599000,
      "tags": ["spot", "crypto", "2025"]
    }
  ]
}
```

Catalog IDs are unique. An experiment can select the complete file or an explicit `[from_ms, to_ms]` time range. The selected range is materialized into the experiment directory as a canonical CSV before research begins.

## Reproducible experiment runs

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Run:

```bash
cp config/datasets.example.json config/datasets.json
cp config/experiment.example.json config/experiment.json
./build/sentum_experiment config/experiment.json
```

Portfolio experiments use `config/portfolio-experiment.example.json` and the same executable.

## Experiment directory

Each execution receives a unique run ID and directory:

```text
log/experiments/<run-id>/
  manifest.json
  experiment.json
  dataset-catalog.json
  risk.json
  research-config.json          # single-asset research
  portfolio-config.json         # portfolio research
  datasets/
    <dataset-id>.csv
  research.json
  trials.csv
  portfolio-research.json
```

Only files relevant to the selected experiment kind are present.

## Provenance

`manifest.json` records:

- run ID
- experiment name and kind
- lifecycle status
- start and finish timestamps
- Git commit embedded at build time
- SHA-256 of the experiment specification
- SHA-256 of `risk.json`
- every selected dataset ID, symbol and time range
- SHA-256 of every materialized dataset slice
- output directory and generated artifacts

The experiment also copies its input specification, dataset catalog, risk configuration and research/portfolio configuration into the run directory. Artifact hashes are stored in the SQLite registry.

This makes the reproducibility identity approximately:

```text
Git commit
+ experiment spec
+ risk configuration
+ strategy/research configuration
+ exact materialized dataset bytes
```

## SQLite experiment registry

The default registry is:

```text
log/experiments.sqlite3
```

Tables:

```text
research_runs
research_datasets
research_artifacts
```

`research_runs` contains one row per execution. `research_datasets` maps each run to its immutable materialized data slices and hashes. `research_artifacts` stores the SHA-256 of generated/captured run artifacts.

The registry uses SQLite WAL mode and is independent of the runtime trading database.

## Time and asset splits

A dataset is selected by ID and can be sliced without modifying the source file:

```json
{
  "id": "btc-usdt-2025",
  "from_ms": 1735689600000,
  "to_ms": 1743465599000
}
```

Portfolio experiments can use several catalog entries with the same explicit time interval:

```json
"datasets": [
  {"id": "btc-usdt-2025", "from_ms": 1735689600000, "to_ms": 1751327999000, "weight": 1.0},
  {"id": "eth-usdt-2025", "from_ms": 1735689600000, "to_ms": 1751327999000, "weight": 1.0},
  {"id": "sol-usdt-2025", "from_ms": 1735689600000, "to_ms": 1751327999000, "weight": 0.75}
]
```

This gives research runs explicit asset and period boundaries instead of relying on implicit filenames.

## Failure semantics

A run is inserted into the registry with status `started` before the research engine executes. If research throws, the manifest and registry are updated to `failed`. Completed runs are marked `completed` with a finish timestamp.

This means failed experiments remain auditable rather than disappearing from history.

## Re-running an experiment

Run IDs deliberately contain the start timestamp, so rerunning the same specification creates a new run rather than overwriting history. The hashes allow two runs to be compared and determine whether code, configuration or data changed.

For strict result reproducibility, compare:

1. `git_commit`
2. experiment/config/risk hashes
3. materialized dataset SHA-256 values
4. generated research artifact hashes

A future dashboard phase can use `experiments.sqlite3` to compare runs visually without changing this storage model.
