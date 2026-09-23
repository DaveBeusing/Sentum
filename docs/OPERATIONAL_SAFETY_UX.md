# Operational safety, error UX and shutdown contract

operational safety UX defines how Sentum presents operational safety, degraded conditions and shutdown progress to an operator. Presentation never replaces Risk, Execution, RuntimeControl or lifecycle authority.

## Operational states

`OperationalSafetyPolicy.hpp` derives one presentation state from the existing runtime snapshot:

- `READY` — runtime healthy, market data connected, persistence not critical;
- `STARTING` — runtime is still starting;
- `DEGRADED` — market data disconnected or persistence pressure is critical/saturated;
- `HALTED` — kill switch active or runtime health is outside the supported healthy/startup/shutdown states;
- `STOPPING` — ordered shutdown is in progress;
- `STOPPED` — runtime shutdown completed.

The model is deliberately small. It gives the operator one primary state rather than several competing warnings.

## Precedence

Safety precedence is deterministic:

1. completed shutdown;
2. shutdown in progress;
3. kill switch / unsafe runtime health;
4. startup;
5. market-data degradation;
6. persistence degradation;
7. ready.

This prevents a lower-severity connectivity or persistence warning from masking a halt or shutdown state.

## Shutdown presentation

The runtime already publishes `health=stopping/stopped`, `shutdown_step`, `shutdown_total_steps` and `shutdown_detail`. operational safety UX consumes those existing fields and does not introduce a second lifecycle state machine.

When shutdown is active, operator presentation should include:

- `STOPPING` state;
- current shutdown detail;
- step `N/Total` when available;
- normalized progress for presentation;
- guidance not to start new work while shutdown drains components.

`STOPPED` is a terminal presentation state and reports complete shutdown progress.

## Fail-closed UX

A kill switch or unsupported runtime health maps to `HALTED`. The presentation must not imply that entries are safe merely because market data or persistence appear healthy. Recovery guidance may point to diagnostic workspaces, but the UI must never clear a kill switch, resume trading, alter risk or synthesize execution truth.

## Degraded operation

`DEGRADED` is reserved for conditions that require operator attention but are distinct from a hard runtime halt. The first slice covers:

- market-data disconnect;
- critical/saturated persistence pressure.

The policy can be extended only when a new condition has an authoritative runtime signal and a clear operator interpretation.

## Evidence

`sentum_operational_safety_policy_tests` verifies:

- healthy -> READY;
- disconnect / persistence pressure -> DEGRADED;
- kill switch precedence -> HALTED;
- startup -> STARTING;
- shutdown step/progress -> STOPPING;
- completed shutdown -> STOPPED terminal state.

The test is part of Release and sanitizer regression coverage.

## Next operational safety integration

The next slice should surface the operational state and shutdown progress in the terminal's always-visible safety area and align the existing web dashboard with the same state vocabulary. That presentation wiring must remain downstream of runtime truth and preserve terminal frame-pacing's zero-write behavior for unchanged terminal frames.
