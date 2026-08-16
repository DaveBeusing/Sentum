# Sentum

**Deterministic quant research, interactive Paper trading, model validation and Binance Spot Testnet execution infrastructure in C++17.**

Sentum is an experimental trading-system platform built around one shared strategy/risk/execution stack. Historical research, replay, shadow trading, interactive Paper trading and Testnet execution reuse the same core abstractions so that strategy behavior is not silently reimplemented for each environment.

> Sentum supports simulated execution and Binance Spot Testnet. Production Binance order routing and withdrawal functionality are intentionally outside the supported runtime.

## What Sentum provides

### Trading runtime

- live Binance public market-data collection over TLS WebSockets
- event-driven market scanner with fixed-capacity per-symbol ring buffers
- configurable momentum, trend, mean-reversion, breakout and multi-timeframe strategies
- weighted strategy ensembles
- centralized risk approval and position sizing
- simulated fills with fees, spread and slippage
- persistent Paper account independent from the real Binance balance
- stop loss, take profit, trailing stop, cooldown and maximum-holding controls
- auditable completed trades and exit reasons
- interactive terminal trading console
- read-only web dashboard

### Quant research

- deterministic CSV replay with `ReplayClock`
- parameter-grid research using the normal trading stack
- expanding walk-forward validation
- purging and embargo around validation boundaries
- final untouched holdout evaluation
- combined out-of-sample metrics
- parameter-stability and overfit-gap analysis
- bootstrap confidence intervals and Monte-Carlo trade resampling
- market-regime analysis
- parallel trial execution
- portfolio/correlation-aware research
- versioned datasets, experiment manifests and SHA-256 provenance
- research comparison and visualization in the web dashboard

### Model lifecycle

```text
research -> shadow -> paper -> testnet
```

Candidates can be registered from completed research experiments, validated against live data in shadow mode, promoted into Paper and finally into Binance Spot Testnet. Promotion is linear, policy-gated, audited and requires explicit operator confirmation. There is no automatic production-money promotion stage.

### Testnet execution

- Binance Spot Testnet only
- exchange-confirmed order-state machine
- partial-fill handling
- Binance User Data Stream
- startup and recovery reconciliation
- dynamic exchange filters and precision rules
- persistent order/runtime/reconciliation audit history
- fail-closed kill switch

## Build

### Requirements

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

Build a Release binary:

```bash
git clone https://github.com/DaveBeusing/Sentum.git
cd Sentum
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The main runtime executable is:

```text
./sentum
```

## Unified CLI

```bash
./sentum help
./sentum version
```

Primary runtime commands:

```bash
./sentum paper
./sentum testnet BTCUSDT
./sentum replay data/btcusdt.csv BTCUSDT
./sentum research config/research.json
./sentum dashboard
```

Legacy flag forms such as `--paper`, `--testnet`, `--replay`, `--research` and `--dashboard` remain accepted.

## Interactive Paper trading

Start:

```bash
./sentum paper
```

Paper mode uses live Binance market data and simulated execution only. It starts the scanner, trading engine, terminal trading console and web dashboard.

The terminal console provides seven views:

```text
1 Market     2 Scanner     3 Orders     4 Trades
5 Strategy   6 Models      7 System
```

Interactive Paper controls:

```text
S  cycle strategy presets
A  automatic scanner symbol selection
M  manual symbol entry
P  pause/resume new entries
C  close the current Paper position through simulated execution
```

Strategy or symbol changes are deferred while a position is open. Pausing new entries does not stop management of an existing position.

See [Trading console](docs/TRADING_CONSOLE.md).

## Configuration

Runtime configuration lives in:

```text
config/config.json
config/risk.json
config/secrets.json
```

Example runtime settings:

```json
{
  "quoteAsset": "USDC",
  "databasePath": "log/klines.sqlite3",
  "paperTrading": true,
  "dashboardHost": "127.0.0.1",
  "dashboardPort": 8080,
  "strategy": {
    "type": "momentum",
    "parameters": {
      "lookback": 20,
      "entry_threshold": 0.001
    }
  },
  "paper": {
    "initialBalance": 10000.0,
    "statePath": "log/paper_account.json",
    "autoSymbol": true,
    "symbol": "",
    "modelDefinition": ""
  }
}
```

`config/risk.json` controls capital limits, position risk, stop/target rules, fees, spread, slippage, cooldown, holding duration and stale-data limits.

Do not commit real API credentials.

## Web dashboard

Paper and Testnet start the dashboard automatically. It can also run standalone:

```bash
./sentum dashboard
```

Host and port are configured in `config/config.json`. Keep `dashboardHost` on `127.0.0.1` for local-only access; binding to `0.0.0.0` exposes the service to reachable network interfaces.

The dashboard is read-only. It provides runtime health, trades/orders, research history, holdout visualization, parameter analysis, model lifecycle state and performance telemetry.

See [Web dashboard](docs/DASHBOARD.md) and [Research dashboard](docs/ADVANCED_RESEARCH_DASHBOARD.md).

## Quant research

Direct research:

```bash
cp config/research.example.json config/research.json
./sentum research config/research.json
```

Managed experiments provide dataset catalogs, immutable slices, SHA-256 provenance and a SQLite experiment registry:

```bash
./build/sentum_experiment --list-datasets config/datasets.json
./build/sentum_experiment config/experiment.json
```

Portfolio research:

```bash
cp config/portfolio-research.example.json config/portfolio-research.json
./build/sentum_portfolio_research config/portfolio-research.json
```

See [Quant research](docs/RESEARCH.md), [Research validation](docs/RESEARCH_ROBUSTNESS.md), [Strategy and portfolio research](docs/STRATEGY_PORTFOLIO_RESEARCH.md) and [Experiment management](docs/EXPERIMENT_DATASET_MANAGEMENT.md).

## Model validation and promotion

Model workflows use `sentum_model`:

```bash
./build/sentum_model register config/model.json
./build/sentum_model promote config/model.json shadow I_APPROVE_MODEL_PROMOTION
./build/sentum_model shadow config/model.json
```

Lifecycle state is persisted in `log/models.sqlite3`. A model loaded into Paper through `paper.modelDefinition` must already have reached the `paper` stage.

See [Shadow trading and model promotion](docs/SHADOW_TRADING_MODEL_PROMOTION.md).

## Binance Spot Testnet

```bash
export SENTUM_ENABLE_SPOT_TESTNET=I_UNDERSTAND_TESTNET_ONLY
export SENTUM_BINANCE_TESTNET_API_KEY='...'
export SENTUM_BINANCE_TESTNET_API_SECRET='...'
./sentum testnet BTCUSDT
```

The exchange remains authoritative for executed state. New submissions are disabled until reconciliation and the User Data Stream are healthy.

See [Testnet runtime](docs/TESTNET_RUNTIME.md), [Account reconciliation](docs/ACCOUNT_RECONCILIATION.md) and [Execution safety](docs/LIVE_TRADING_SAFETY.md).

## Architecture

```text
                         +-----------------------+
Binance market data ---> | Collector / parser    |
                         +-----------+-----------+
                                     |
                        +------------+-------------+
                        |                          |
                        v                          v
              MarketDataStore            bounded SPSC queue
                        |                          |
                        v                          v
                 MarketEventBus              SQLite WAL
                        |
                 +------+------+
                 |             |
                 v             v
              Scanner      Strategy
                                |
                                v
                           RiskManager
                                |
                                v
                        IExecutionVenue
                         /            \
                        /              \
            SimulatedExecution    Binance Testnet
         paper/replay/research/      OrderManager
                shadow                   |
                                        v
                                User Data Stream

Historical datasets -> deterministic replay -> research/portfolio experiments
                                                |
                                                v
                                 experiment/model registries
                                                |
                          +---------------------+------------------+
                          v                                        v
                   terminal console                         web dashboard
```

## Persistence

Important runtime and research artifacts include:

```text
log/klines.sqlite3
log/paper_account.json
log/status.json
log/replay.sqlite3
log/replay_metrics.json
log/research_latest.json
log/research_trials.csv
log/experiments.sqlite3
log/experiments/<run-id>/
log/models.sqlite3
log/shadow/
```

## Performance and validation

Sentum uses an allocation-light kline parser, interned symbol IDs, bounded persistence queues, SQLite WAL/batching, in-memory scanner buffers and runtime latency histograms.

Build benchmark targets with:

```bash
cmake -S . -B build-perf \
  -DCMAKE_BUILD_TYPE=Release \
  -DSENTUM_BUILD_BENCHMARKS=ON
cmake --build build-perf --parallel
```

See [Runtime performance](docs/PERFORMANCE.md).

ThreadSanitizer build:

```bash
cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSENTUM_ENABLE_TSAN=ON
cmake --build build-tsan --parallel 2
```

Long-running Paper soak tests, sanitizer builds, reconnect/reconciliation scenarios and independent research validation remain important before treating Sentum as production-grade software.

## Documentation

- [Trading console](docs/TRADING_CONSOLE.md)
- [Web dashboard](docs/DASHBOARD.md)
- [Research dashboard](docs/ADVANCED_RESEARCH_DASHBOARD.md)
- [Runtime performance](docs/PERFORMANCE.md)
- [Quant research](docs/RESEARCH.md)
- [Research validation and robustness](docs/RESEARCH_ROBUSTNESS.md)
- [Strategy and portfolio research](docs/STRATEGY_PORTFOLIO_RESEARCH.md)
- [Experiment and dataset management](docs/EXPERIMENT_DATASET_MANAGEMENT.md)
- [Shadow trading and model promotion](docs/SHADOW_TRADING_MODEL_PROMOTION.md)
- [Binance Spot Testnet runtime](docs/TESTNET_RUNTIME.md)
- [Account reconciliation and recovery](docs/ACCOUNT_RECONCILIATION.md)
- [Execution safety](docs/LIVE_TRADING_SAFETY.md)

## Disclaimer

Sentum is experimental software. Simulated, replayed, shadow, research or Testnet performance does not guarantee future results. Review [DISCLAIMER.md](DISCLAIMER.md) before using the project for trading-system experimentation.
