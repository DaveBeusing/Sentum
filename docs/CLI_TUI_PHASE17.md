# Phase 17 - Unified CLI and TUI

Phase 17 replaces the legacy stateful `UiConsole` with one command-line entry point and one terminal view backed by the same `DashboardState` used by the web dashboard.

## Unified commands

Preferred syntax:

```bash
./client paper
./client testnet BTCUSDT
./client replay data/events.csv BTCUSDT
./client research config/research.example.json
./client dashboard
./client version
./client help
```

Existing flag-style invocations remain compatible:

```bash
./client --paper
./client --testnet BTCUSDT
./client --replay data/events.csv BTCUSDT
./client --research config/research.example.json
./client --dashboard
```

Runtime options can be placed after the command:

```bash
./client paper --no-tui
./client testnet BTCUSDT --dashboard-port 18080
./client dashboard --dashboard-port 18080
```

`SENTUM_DASHBOARD_PORT` remains supported when no explicit `--dashboard-port` is supplied.

## TUI behavior

Paper and Testnet modes start the terminal UI when stdout is attached to a terminal. Redirected output and CI therefore do not emit ANSI redraw sequences. `--no-tui` explicitly disables it.

The TUI shows:

- runtime mode and health
- collector/scanner/trader state
- current symbol and scanner leader
- market event throughput
- persistence queue depth and drop rate
- database size
- trade count, W/L, win rate and P/L
- active position snapshot
- parser, event-dispatch, strategy-decision and SQLite p50/p95/p99 latency

The terminal view does not maintain a separate model. It snapshots `DashboardState`, so terminal and web dashboard cannot drift due to independent setter paths.

## Runtime ownership

Before Phase 17, `ExecutionEngine` owned `UiConsole` and mirrored state into both `UiConsole` fields and `DashboardState`.

After Phase 17:

```text
Collector / Scanner / Trader / Runtime metrics
                    |
                    v
              DashboardState
               /          \
              v            v
       TerminalUi      DashboardServer
```

`ExecutionEngine` is now UI-agnostic. It publishes runtime state only.

## Operational behavior

`Ctrl+C` and `SIGTERM` remain the shutdown mechanism for long-running modes. The TUI restores the cursor before exit. The web dashboard continues to run in Paper/Testnet whether or not TUI output is enabled.

## CI acceptance

CI verifies:

- new `help` and `version` commands
- legacy `--help` and `--version` forms
- invalid commands fail with a useful error
- new `research` syntax
- legacy `--research` compatibility
- dashboard startup with `--dashboard-port`
- all existing Phase-16 benchmarks and TSan build coverage
