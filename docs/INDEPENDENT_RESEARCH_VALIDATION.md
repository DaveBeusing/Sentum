# Independent research validation

Sentum provides an independent verification path for persisted managed single-asset research experiments. The verifier checks experiment evidence without importing the C++ research ranking or scoring implementation that produced the result.

A `PASS` is an evidence result only. It does not claim that historical performance predicts future profitability, that a model is suitable for live trading, or that model promotion should occur automatically.

## Run

Validate a completed managed experiment:

```bash
python3 tools/verify_research.py \
  log/experiments/<run-id> \
  --registry log/experiments.sqlite3
```

By default the verifier writes `log/experiments/<run-id>/research-validation.json`. The report is a sidecar evidence artifact and does not overwrite the original experiment artifacts.

To compare deterministic outputs with an independently generated reference run:

```bash
python3 tools/verify_research.py \
  log/experiments/<rerun-id> \
  --registry log/experiments.sqlite3 \
  --reference-run log/experiments/<reference-run-id>
```

## Required evidence

Independent validation currently targets managed single-asset research experiments and expects:

- manifest version 2;
- experiment, risk and research configuration;
- materialized dataset slice and recorded SHA-256;
- experiment registry metadata;
- `research.json`;
- `trials.csv`;
- `research-visualization.json` schema version 2 with persisted final-holdout trade records.

The managed experiment manifest records SHA-256 values for immutable inputs and generated artifacts. The experiment registry retains an independent persisted copy of dataset and artifact hashes.

## Validation checks

### Provenance and integrity

The verifier checks configuration hashes, materialized dataset identity, dataset provenance, generated artifact hashes, registry consistency, source revision and missing or malformed evidence.

### Walk-forward and holdout boundaries

The research result persists the concrete validation contract used by the run. The verifier independently recomputes expected boundaries from the materialized event count and persisted research configuration.

Checks include chronological dataset ordering, train/validation ordering, absence of overlap, purge and embargo boundaries, and final holdout separation. Trial-search artifacts are checked for holdout-labelled columns, leaderboard entries are reconciled with `trials.csv`, and the selected parameter set must match the first eligible validation-ranked candidate.

### Independent metric recomputation

The final holdout visualization persists neutral trade evidence including entry/exit timestamps, prices, quantity, net profit and fees. `tools/verify_research.py` independently recalculates trade count, net profit, max drawdown, profit factor, win rate, expectancy, Sharpe, Sortino and fee share.

The Python verifier follows Sentum's metric conventions but does not call the production research metric calculator. Holdout trade entry timestamps are checked against the independently recomputed holdout boundary.

### Deterministic reproduction

When `--reference-run` is supplied, the verifier compares deterministic research outputs while excluding non-deterministic lifecycle metadata such as generation timestamps and run IDs. The comparison covers the validation contract, leaderboard, selected parameters, final holdout and robustness outputs, `trials.csv`, holdout visualization/trade evidence and dataset hashes.

Without a reference run, reproduction is reported as `NOT_CHECKED`; this does not make the other validation sections fail.

## Report contract

`research-validation.json` is versioned and contains the validation kind and schema version, run/code identity, overall `PASS` or `FAIL`, section status, independent metric comparison, optional reproduction status, explicit machine-readable violation codes and limitations.

A verifier failure is fail-closed for the validation claim: the report remains `FAIL` until persisted evidence is internally consistent.

## CI

Core CI executes the verifier regression suite and runs a compact canonical managed experiment twice. The first run validates provenance, boundaries and metrics; the second is also compared against the first for deterministic reproduction. The fixture is generated locally and does not depend on large external datasets.

## Model promotion boundary

The canonical Independent Research Validation report produced in Core CI is a release-blocking input to repository Release Readiness. Existing model-promotion policy is intentionally unchanged.

A verifier `PASS` is not interpreted as profitability, target-environment production acceptance, execution readiness or permission to promote a model. Making this evidence a model-promotion gate would require a separate explicit policy change.

## Readiness relationship

See [Readiness Evidence Contract](READINESS_EVIDENCE.md) for how canonical research validation participates in repository release evidence without becoming execution authority.
