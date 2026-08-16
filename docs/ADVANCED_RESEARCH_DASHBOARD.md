# Research dashboard

Sentum's web dashboard includes a research workbench backed by the experiment registry and immutable run artifacts.

## Start

```bash
./sentum dashboard
```

The research views are read-only and share the dashboard's configured host and port.

## Research workspace

The dashboard provides:

- experiment history from `log/experiments.sqlite3`
- primary-run selection and comparison with another run
- validation and final-holdout metrics
- deterministic holdout equity and drawdown curves
- parameter heatmaps
- parameter-stability and overfit-gap information
- multiple-testing-adjusted Sharpe information
- bootstrap confidence intervals
- Monte-Carlo net-profit/drawdown intervals and probability of loss
- market-regime performance
- Git/config/risk/dataset provenance
- single-asset and portfolio research results

## Visualization artifacts

Single-asset experiment runs can include `research-visualization.json`. Sentum generates this by replaying the already-selected holdout parameters through the normal deterministic clock, strategy, risk and simulated-execution stack. The resulting equity/drawdown series therefore comes from actual simulated completed trades rather than being reconstructed from summary metrics.

Older experiment runs that do not contain a visualization artifact remain readable; chart panels simply have no detailed curve data for those runs.

## Research APIs

```text
GET /api/experiments?limit=200
GET /api/experiment?run_id=<run-id>
GET /api/experiment/trials?run_id=<run-id>&limit=10000
```

Run IDs are resolved through the experiment registry. Browser input is not treated as a filesystem path.

The experiment-detail response can combine registry metadata, dataset hashes, artifact hashes, research output, visualization data and portfolio research output.

## Parameter landscape

The current heatmap uses `lookback` and `entry_threshold` as its primary axes and displays the best validation score for trials sharing those values. Other risk and execution dimensions remain available in the complete trial dataset.

## Security boundary

Research visualization does not provide execution control. The dashboard cannot place or cancel orders, change strategy/risk configuration, promote models, reset the kill switch or access API credentials.
