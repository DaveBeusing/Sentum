# Cross-Surface Semantic Parity & Drift Detection

## Purpose

AP-18 requires terminal and browser operations surfaces to present the same safety, governance and workflow semantics from the same runtime snapshot.

The browser must not reinterpret safety or governance state independently. The canonical web contract is `sentum.operations.v1` with schema version `1` and authority `READ_ONLY_PRESENTATION`.

## Canonical semantics

Parity covers:

- runtime label, message and recommended workspace;
- governance state;
- evidence state;
- maintenance, incident and recovery states;
- pending approval count;
- approval request classification and status;
- maintenance, incident and recovery workflow classification/state.

The terminal side is derived directly from the AP-17 policy helpers. The web side is reconstructed from the serialized `/api/operations` contract. The comparison therefore detects semantic differences introduced by serialization, contract changes or browser-facing projection drift.

## Fail-closed behavior

A web contract is invalid when any of the following changes unexpectedly:

- `schema_version`;
- `contract` identifier;
- `authority`.

Invalid contracts are treated as drift. The browser independently validates the same three fields before rendering operational state. A mismatch produces a visible `UNAVAILABLE` state with a contract-mismatch warning rather than rendering unknown semantics as healthy.

Missing operations-control-plane evidence remains `UNAVAILABLE / MISSING`. Stale evidence remains `STALE`, and stale approval requests remain `FORBIDDEN / BLOCKED - STALE EVIDENCE`.

## Regression gate

`cross_surface_semantic_parity_tests` exercises:

- healthy runtime parity;
- critical/kill-switch parity;
- missing governance parity;
- stale evidence parity;
- schema and authority drift detection;
- deliberate governance and approval-classification drift detection;
- stable contract identifier/version.

The test target is part of the normal CTest suite and is attached to the sanitizer dependency graph so ASan, UBSan and TSan build it before CTest execution.

Any semantic mismatch causes the regression executable to fail and therefore blocks Core CI.

## Authority boundary

Parity and drift detection are presentation validation only. They do not add browser or terminal authority to execute governed actions, approve requests, clear safety interlocks, resume trading, mutate Risk/Execution state or create execution truth.
