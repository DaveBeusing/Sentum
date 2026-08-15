# Sentum

**Deterministic market replay, auditable paper trading, quantitative strategy research, Binance Spot Testnet execution infrastructure, and a local read-only trading dashboard in C++17.**

Sentum is an experimental trading-system and research project focused on predictable lifecycle behavior, bounded market-data processing, reproducible strategy execution, traceable risk decisions, exchange-confirmed state, observable runtime health, and out-of-sample strategy evaluation.

> **Current status:** Phases 1–18 are merged into `master`. Paper, replay, research, Binance Spot Testnet, dashboard, and interactive paper-control modes are exposed by the unified `sentum` executable. Production Binance execution and withdrawal endpoints remain intentionally absent.

## Capabilities

- Binance market-data collection over TLS WebSockets
- fixed-capacity in-memory per-symbol market buffers
- event-driven scanner using incremental market updates rather than timed polling
- bounded non-blocking persistence queue with one batched SQLite WAL writer
- reusable prepared UPSERT statements
- modular `IStrategy` interface and incremental indicator primitives
- central `RiskManager` with strict configuration validation and position sizing
- shared `IExecutionVenue` boundary for simulated and Testnet execution
- deterministic paper/replay fill modelling with spread and slippage
- persistent completed-trade and order-event audit history
- deterministic CSV replay with `ReplayClock`
- Net Profit, Max Drawdown, Profit Factor, Win Rate, Expectancy, Sharpe, Sortino, fee-share, and slippage-sensitivity metrics
- deterministic parameter-grid research using the normal strategy/risk/execution stack
- expanding walk-forward validation and out-of-sample ranking
- Train/Validation score and overfit-gap reporting
- complete CSV trial export plus compact JSON research leaderboard
- independent replay-metric verification tooling
- Binance Spot Testnet order state machine, reconciliation, User Data Stream, dynamic exchange filters, and kill switch
- account/balance reconciliation and controlled resume after reconciliation
- persistent runtime, reconciliation, and kill-switch operational events
- machine-readable runtime status
- local read-only Sentum web dashboard and research API
- interactive paper-trading TUI with strategy, symbol, pause/resume, and manual-close controls
- persistent simulated paper account independent from the real Binance balance
- Release/ThreadSanitizer CI, market-path microbenchmark, dashboard smoke test, and research smoke test

## Build

```bash
git clone https://github.com/DaveBeusing/Sentum.git
cd Sentum
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The main runtime binary is produced as:

```text
./sentum
```

The Release build also copies the stripped executable to the repository root as `sentum`.

### ThreadSanitizer

```bash
cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSENTUM_ENABLE_TSAN=ON
cmake --build build-tsan --parallel
```

### Market-path benchmark

```bash
cmake -S . -B build-perf \
  -DCMAKE_BUILD_TYPE=Release \
  -DSENTUM_BUILD_BENCHMARKS=ON
cmake --build build-perf --parallel
./build-perf/sentum_market_benchmark
```

## Unified CLI

```bash
./sentum help
./sentum version
```

Preferred command syntax:

```bash
./sentum paper
./sentum testnet BTCUSDT
./sentum replay data/btcusdt.csv BTCUSDT
./sentum research config/research.json
./sentum dashboard
```

The legacy flag forms (`--paper`, `--testnet`, `--replay`, `--research`, `--dashboard`) remain accepted by the unified CLI.

## Runtime modes

### Paper trading

```bash
./sentum paper
```

`./sentum` without arguments is equivalent to Paper mode. Paper starts `ExecutionEngine`, collector, event-driven scanner, terminal UI when attached to a TTY, paper `TradeEngine`, and the web dashboard.

Paper mode uses live market data and simulated execution only. Strategy and symbol changes, entry pause/resume, and manual simulated position closing are available from the TUI. See [`docs/PHASE18_INTERACTIVE_PAPER_TRADING.md`](docs/PHASE18_INTERACTIVE_PAPER_TRADING.md).

### Deterministic replay

```bash
./sentum replay data/btcusdt.csv BTCUSDT
```

CSV format:

```text
timestamp_ms,price,volume
1710000000000,68250.10,0.25
1710000001000,68260.20,0.12
```

The volume column is optional. Events are stably sorted and processed with `ReplayClock`.

### Quant research

```bash
cp config/research.example.json config/research.json
./sentum research config/research.json
```

Research mode performs a bounded Cartesian parameter search using deterministic replay and expanding walk-forward validation. Trial ranking uses validation rather than training metrics. Results are written to:

```text
log/research_latest.json
log/research_trials.csv
```

See [`docs/RESEARCH.md`](docs/RESEARCH.md).

### Binance Spot Testnet

```bash
export SENTUM_ENABLE_SPOT_TESTNET=I_UNDERSTAND_TESTNET_ONLY
export SENTUM_BINANCE_TESTNET_API_KEY='...'
export SENTUM_BINANCE_TESTNET_API_SECRET='...'
./sentum testnet BTCUSDT
```

The execution client is hard-wired to Binance Spot Testnet. Production and withdrawal endpoints are not implemented.

### Sentum Dashboard

Paper and Testnet modes start the dashboard automatically. It can also run independently against persisted data:

```bash
./sentum dashboard
```

Default URL:

```text
http://127.0.0.1:8080
```

Custom port:

```bash
./sentum dashboard --dashboard-port 8090
```

or:

```bash
export SENTUM_DASHBOARD_PORT=8090
```

The dashboard is intentionally read-only and loopback-only. See [`docs/DASHBOARD.md`](docs/DASHBOARD.md).

## Architecture

```text
Binance market WebSocket
        |
        v
Collector / parser
   |                  |
   |                  +--> bounded queue --> SQLite WAL writer
   |
   v
fixed MarketDataStore ring buffers
        |
        v
MarketEventBus
        |
        +--> event-driven SymbolScanner
        |
        +--> strategy/risk pipeline
                     |
                     v
                 IStrategy
                     |
                     v
                RiskManager
                     |
                     v
              IExecutionVenue
               /           \
              /             \
   SimulatedExecution   Binance Testnet
     paper / replay       OrderManager
       / research              |
              |                v
              |         User Data Stream
              |                |
              v                v
        trade history   exchange-confirmed state

Historical CSV + deterministic ReplayClock
                     |
                     v
          Quant Research Platform
         grid -> walk-forward folds
                     |
                     v
       JSON leaderboard + CSV trials

Runtime state + SQLite history + replay/research metrics
                     |
                     v
          local Sentum Dashboard
```

## Exchange order-state authority

Supported states:

```text
pending
acknowledged
partially filled
filled
cancelling
cancelled
rejected
```

A REST placement acknowledgement does not create an executed local position. Confirmed position state changes only from exchange-confirmed fills or reconciliation. Failed/ambiguous recovery keeps trading blocked.

## Requirements

- Linux
- C++17 compiler
- CMake 3.10+
- libcurl
- OpenSSL
- Boost.System / Boost.Beast headers
- WebSocket++
- SQLite3
- POSIX threads
- nlohmann/json

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake git \
  libcurl4-openssl-dev libssl-dev \
  libboost-system-dev libasio-dev \
  libwebsocketpp-dev sqlite3 libsqlite3-dev \
  nlohmann-json3-dev
```

## Configuration

Required runtime files:

```text
config/config.json
config/risk.json
config/secrets.json
```

Research configuration starts from:

```text
config/research.example.json
```

`config/risk.json` controls capital limits, risk per trade, stops, take profit, fees, spread, slippage, leverage, cooldown, holding duration, stale-data limits, and local risk constraints. Research grids can override stop-loss, take-profit, and slippage assumptions per trial. Binance Testnet quantity/notional rules are additionally loaded from exchange metadata.

Do not commit real API credentials. Testnet keys should use only the permissions required for Spot Testnet trading and should never have withdrawal rights.

## Persistence and observability

Sentum uses:

```text
log/klines.sqlite3         candles, trades, order and operational events
log/status.json            machine-readable Testnet runtime state
log/paper_account.json     persistent simulated Paper equity and realized P/L
log/replay.sqlite3         most recent deterministic replay trade history
log/replay_metrics.json    most recent replay metrics
log/research_latest.json   most recent research leaderboard
log/research_trials.csv    complete trial table for the latest research run
```

The dashboard reads SQLite through independent read-only connections, so browser requests do not enter the market-data or execution hot path. Research results are exposed as a read-only file-backed API.

## Development status

| Phase | Scope | Status |
|---|---|---|
| 1 | lifecycle stability, serialized strategy processing | Merged |
| 2 | bounded WAL persistence and in-memory scanner | Merged |
| 3 | strategy/risk modelling and auditable paper fills | Merged |
| 4 | deterministic replay and backtest metrics | Merged |
| 5 | exchange-confirmed Testnet order execution infrastructure | Merged |
| 6 | unified Testnet execution, recovery, persistence, observability | Merged |
| 7 | account reconciliation, dynamic filters, controlled resume | Merged |
| 8 | event-driven performance architecture and shared simulated venue | Merged |
| 9 | local Sentum Dashboard and read-only runtime API | Merged |
| 10 | Quant Research Platform, grid search, walk-forward validation | Merged |
| 11–16 | research robustness, experiments, model promotion, performance | Merged |
| 17 | unified CLI and terminal UI | Merged |
| 18 | interactive Paper trading and strategy control | Merged |

## Dashboard API

```text
GET /api/health
GET /api/status
GET /api/trades?limit=100
GET /api/orders?limit=100
GET /api/equity?limit=500
GET /api/replay
GET /api/research
```

The browser API is read-only. Order placement, cancellation, kill-switch reset, configuration writes, credential access, and production-trading activation are intentionally not exposed.

## Documentation

- [`docs/CLI_TUI_PHASE17.md`](docs/CLI_TUI_PHASE17.md) - unified CLI and terminal UI
- [`docs/PHASE18_INTERACTIVE_PAPER_TRADING.md`](docs/PHASE18_INTERACTIVE_PAPER_TRADING.md) - interactive Paper runtime
- [`docs/DASHBOARD.md`](docs/DASHBOARD.md) - local read-only dashboard
- [`docs/TESTNET_RUNTIME.md`](docs/TESTNET_RUNTIME.md) - Binance Spot Testnet runtime
- [`docs/RESEARCH.md`](docs/RESEARCH.md) - quantitative research workflow
- [`docs/RESEARCH_ROBUSTNESS.md`](docs/RESEARCH_ROBUSTNESS.md) - validation and robustness

## Known validation work

Before treating Sentum as release-ready, continue validating:

- long-running paper soak tests and queue-drop limits
- Release/TSan/ASan/UBSan builds after architecture changes
- deliberate User Data Stream interruption and recovery
- partial-fill and process-restart reconciliation
- real Binance Spot Testnet balance mismatch and controlled-resume scenarios
- independent replay metric reference checks over multiple datasets
- dashboard load and SQLite-WAL coexistence during long runtimes
- research datasets across different regimes and symbols
- parameter stability around leaderboard winners rather than only the single best point
- stronger research methods such as purged/embargoed folds and Monte Carlo resampling before production claims

Sentum remains experimental software and should not be assumed safe for production or real-money trading without dedicated validation.
