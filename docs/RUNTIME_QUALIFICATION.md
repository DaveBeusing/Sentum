# Runtime Qualification and Fault Injection

## Purpose

Sentum runtime qualification extends the existing operational-acceptance evidence model with bounded Paper soak runs, deterministic failure injection and commit-bound machine-readable evidence.

The qualification layer is intentionally separate from production trading. It never requires live Binance credentials, never routes production-money orders and never changes Strategy, Risk or execution semantics to make a scenario pass.

## Validation layers

Runtime reliability is validated at three different cadences:

1. normal regression and sanitizer tests verify focused contracts;
2. Core CI runs a short Paper soak plus the deterministic fault suite on every pull request and push to `master`;
3. the dedicated `Sentum Runtime Qualification` workflow runs a longer Paper soak and every supported fault scenario on a schedule or by manual dispatch.

The longer workflow is evidence for operational qualification. It is not currently consumed by Release Readiness or the Production Operations Gate.

## Build

Configure a Release build with tests:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel --target \
  sentum_runtime_qualification_scenarios \
  sentum_notification_delivery_evidence_repository_tests \
  sentum_governed_incident_lifecycle_runtime_tests
```

The short registered CTest fault suite can be executed with:

```bash
ctest --test-dir build -R runtime_qualification_faults --output-on-failure
```

## Qualification runner

`tools/ci/runtime_qualification.py` is the canonical evidence runner.

Example Paper soak:

```bash
python3 tools/ci/runtime_qualification.py \
  --build-dir build \
  --scenario paper-soak \
  --duration-seconds 900 \
  --seed 1 \
  --timeout-seconds 1020 \
  --max-rss-growth-kib 131072 \
  --output log/runtime-qualification/paper-soak.json \
  --summary log/runtime-qualification/paper-soak.md
```

Example deterministic fault qualification:

```bash
python3 tools/ci/runtime_qualification.py \
  --build-dir build \
  --scenario all-faults \
  --duration-seconds 1 \
  --seed 1 \
  --timeout-seconds 120 \
  --max-rss-growth-kib 65536 \
  --output log/runtime-qualification/all-faults.json \
  --summary log/runtime-qualification/all-faults.md
```

The seed is recorded in every report and controls deterministic Paper input. The same seed and scenario selection can therefore be reused when reproducing a qualification failure.

## Supported scenarios

| Scenario | Contract exercised |
| --- | --- |
| `paper-soak` | Real `TradeEngine` Strategy/Risk/simulated-execution path driven by deterministic seeded market input for a bounded duration. |
| `market-data-reconnect` | Collector lifecycle and persistence continuity across a controlled producer disconnect/restart boundary without network dependency. |
| `user-stream-interruption` | `LiveOrderSession` User Data Stream health loss, fail-closed kill switch and stream rebuild through an injected stream boundary. |
| `listen-key-keepalive` | Keepalive failure, submission blocking, stream rebuild and required explicit recovery. |
| `partial-fill` | `OrderManager` exchange-event path; partial execution remains non-final until an exchange-confirmed `FILLED` event. |
| `unresolved-order-restart` | Startup reconciliation with an unresolved exchange order latches the kill switch and blocks new submissions. |
| `balance-mismatch` | `AccountReconciler` detects authoritative account/local quantity divergence and emits unsuccessful reconciliation evidence. |
| `persistence-pressure` | Real bounded Collector queue saturation, drop accounting, pressure evidence and later drain. |
| `persistence-write-failure` | Existing durable-delivery persistence failure/restart regression is executed as qualification evidence and must fail closed. |
| `dashboard-recovery` | Dashboard partial initialization failure on an occupied port followed by bounded successful recovery and shutdown. |
| `runtime-restart` | Existing notification-delivery and governed-incident runtime restart regressions verify persisted authoritative state reconstruction. |
| `kill-switch-recovery` | Automatic recovery remains blocked until the explicit reconciled-resume confirmation is supplied. |
| `all-faults` | Runs the hermetic C++ fault scenarios as one deterministic contract suite. |

The mock Testnet boundaries provide exchange responses and User Data Stream lifecycle events only. They do not bypass the production `OrderManager`, reconciliation or kill-switch state machines and they never manufacture a local confirmed fill outside the exchange-event contract.

## User Data Stream recovery

`LiveOrderSession` now treats both of these conditions as runtime faults:

- an unhealthy/stopped User Data Stream;
- a listen-key keepalive failure.

Either condition immediately marks the session not ready and activates the kill switch before attempting to rebuild the stream.

A successful automatic rebuild does not clear the latched kill switch. New submissions remain blocked until explicit reconciled resume succeeds. Reconciliation with unresolved open orders keeps the kill switch active and the resume request is rejected.

## Memory evidence

On Linux, the runner samples the child process `VmRSS` from `/proc/<pid>/status` throughout execution and records:

- starting RSS;
- peak RSS;
- ending RSS;
- total RSS growth;
- a trend slope calculated from the post-startup portion of the samples;
- the configured qualification growth guardrail;
- whether an unbounded-growth condition was detected.

The RSS growth values used by CI are qualification guardrails, not production memory SLAs. They exist to make large or continuously positive memory regressions fail reproducibly while longer evidence is accumulated.

Core CI currently uses a 65,536 KiB growth guardrail. Extended qualification uses 131,072 KiB for the longer Paper soak.

When RSS is unavailable on the host, the report retains the memory fields with explicit unavailable values rather than inventing measurements.

## Machine-readable evidence

Every runner report contains:

- schema version;
- Git commit SHA;
- workflow run identifier when available;
- scenario;
- UTC start and end timestamps;
- requested and actual duration;
- deterministic seed;
- PASS/FAIL status;
- evidence-completeness state;
- starting/peak/ending RSS and trend evidence;
- queue depth/high-water/saturation/drop evidence;
- reconnect and restart counts;
- lifecycle failure count;
- reconciliation outcome;
- kill-switch transition count;
- event throughput when produced by the scenario;
- timeout state;
- failure details;
- scenario-native evidence;
- exact executed commands and return codes.

The Git SHA is taken from `GITHUB_SHA` in Actions and otherwise from `git rev-parse HEAD`. A report with an unknown commit identity cannot produce PASS.

## PASS / FAIL rules

A qualification report can produce PASS only when all required evidence is present and every required invariant holds.

FAIL is produced when any of the following occurs:

- a required qualification binary is missing;
- a child process times out;
- a child process exits non-zero;
- a scenario returns `pass=false`;
- expected machine-readable scenario output is absent;
- commit identity cannot be established;
- the configured RSS-growth guardrail is exceeded with a positive growth trend;
- any scenario assertion detects unsafe or inconsistent state.

Incomplete evidence is never converted to PASS.

## Queue and persistence evidence

The persistence-pressure scenario drives the real bounded Collector queue until saturation is reached. Saturation must remain visible as a rejected enqueue and an explicit saturation metric; data is never silently overwritten.

After pressure injection, the queue must drain to zero under the normal persistence writer. Queue high-water, saturation events and drop count are retained in qualification evidence.

## Execution-truth and recovery invariants

Qualification must preserve these runtime invariants:

- REST placement acknowledgement is not a confirmed fill;
- partial fill is not treated as a complete confirmed position;
- Testnet execution truth remains downstream of exchange-confirmed events and reconciliation;
- ambiguous restart state stays blocked;
- stream failure disables submissions;
- unresolved open orders block kill-switch clearing;
- balance mismatch produces failed reconciliation evidence;
- automatic stream recovery never authorizes trading by itself;
- explicit reconciled resume is required before a latched kill switch can be cleared.

No qualification test may weaken these invariants to obtain a passing result.

## CI responsibilities

### Core CI

Core CI:

- builds the qualification target;
- registers and executes the deterministic CTest fault suite;
- runs a two-second Paper qualification smoke;
- runs the combined fault qualification;
- publishes JSON and Markdown evidence for 14 days;
- builds/runs the qualification fault target under the existing sanitizer validation graph.

The Core CI scope remains bounded and does not contain long-duration soak tests.

### Extended qualification

`.github/workflows/runtime-qualification.yml`:

- is available through `workflow_dispatch`;
- runs weekly through the configured schedule;
- defaults to a 900-second Paper soak;
- accepts an explicit deterministic seed and Paper duration for manual runs;
- runs every fault/recovery scenario separately after the soak;
- uploads all JSON and Markdown evidence for 30 days.

A scheduled qualification failure is visible as a failed dedicated workflow. It does not block unrelated pull-request feedback and is not currently an input to Release Readiness.

## Sanitizers

The deterministic fault executable participates in the existing sanitizer build graph. Concurrency-sensitive runtime paths therefore remain eligible for ASan, UBSan and TSan coverage without requiring external exchange availability.

Long-duration soak timing remains in the Release qualification workflow rather than sanitizer jobs.

## Reproduction and triage

To reproduce a failure:

1. check out the exact `git_sha` recorded in the failed report;
2. configure the Release test build;
3. execute the recorded scenario with the recorded seed and duration;
4. use the exact command recorded in the report when the failure is command-specific;
5. compare RSS, queue, reconnect, reconciliation and kill-switch evidence with the failed artifact;
6. run the focused sanitizer suite when the failure is concurrency or memory related.

Do not replace a failed qualification run with a different seed or shorter duration when determining whether the original failure has been fixed.

## Authority boundary

Runtime qualification does not:

- route production Binance orders;
- require production or Testnet credentials;
- clear kill switches automatically;
- resume entries automatically;
- synthesize exchange-confirmed fills;
- change Risk or Strategy decisions;
- reinterpret an ambiguous exchange state as healthy;
- modify the Production Operations Gate.

The qualification harness observes and asserts the existing runtime authority model; it does not become execution authority.
