# Model source provenance

Model registration imports research evidence only from a completed managed, single-asset research experiment. The experiment registry is the trust anchor for the recorded run, input files, materialized dataset and output artifacts.

## Registration checks

`validate_source_experiment` reads the run, dataset and artifact rows in one read-only SQLite transaction. It requires the version-2 manifest to match the registry's run identity, status, timestamps, source commit, input hashes and output directory. Missing registries are never created by validation.

The manifest and registry must declare the same complete artifact set. Registration verifies SHA-256 for every declared artifact, the immutable experiment and risk inputs, and the single materialized dataset. The required artifacts are `experiment.json`, `dataset-catalog.json`, `risk.json`, `research-config.json`, `research.json`, `trials.csv` and `research-visualization.json`. Recorded relative paths are resolved from the working directory used for the experiment; absolute paths retain their recorded location. A missing, unreadable, altered or incompletely recorded artifact rejects registration. Legacy manifests require a newly generated managed experiment.

The candidate symbol must match both the registered dataset and the research report. Its complete supported strategy definition must identify the selected eligible Momentum candidate: `type`, `parameters.lookback` and `parameters.entry_threshold`. Additional strategy fields are rejected until the managed research producer explicitly supports and records them.

Every effective `RiskConfig` field must match the immutable research risk configuration after applying the selected grid's `stop_loss_percent`, `take_profit_percent` and `slippage_percent`. Fees, capital, position sizing, trailing policies, exchange filters and time limits therefore cannot silently differ from the researched candidate. A selected grid value may legitimately differ from the experiment's base risk file; the candidate must use the selected value. Equivalent numeric formatting in the candidate risk JSON is accepted, while the archived source files remain bound to their exact bytes.

## Evidence binding

The report and risk inputs are parsed from the same bytes that were checked. The report's generation time must fall within the recorded source run. The returned research evidence includes those run boundaries and the definition hash computed from the checked candidate risk snapshot. The model registry then atomically registers the immutable model revision and its initial research evidence. If the candidate risk changes between validation and registration, the revision comparison rejects it.

Registration validates provenance and candidate identity. It does not independently recompute every metric or qualify profitability; `tools/verify_research.py` remains the independent check for split boundaries, holdout trades, metrics and deterministic reproduction. Source files and the registry must be protected by the same access controls as the other governed local evidence: SHA-256 detects disagreement with recorded identities, not a coordinated rewrite of both data and its trusted registry.

## Regression coverage

`model_source_validation` runs a genuine managed experiment with nonzero holdout trades and selected risk values that differ from the base configuration. It then checks altered reports, datasets and artifacts; missing or inconsistent manifest/registry records; changed source, symbol and strategy; every effective risk field; and malformed or mismatched selected-candidate evidence. The fixture uses isolated local files and does not connect to an exchange.
