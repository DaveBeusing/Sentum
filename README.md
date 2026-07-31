# Sentum

**Deterministic market replay, auditable paper trading, and exchange-confirmed Binance Spot Testnet order infrastructure in C++17.**

Sentum is an experimental trading-system project focused on predictable lifecycle behavior, bounded market-data processing, reproducible strategy execution, traceable risk decisions, and fail-closed exchange state handling.

> **Current status:** Phases 1–5 are merged into `master`. Paper trading and deterministic replay are integrated into the normal runtime. The Binance Spot Testnet order infrastructure exists as a separate execution layer, but it is not yet connected end to end to the normal `ExecutionEngine` strategy path.

## Capabilities

- Binance market-data collection over TLS WebSockets
- bounded, non-blocking queue between market-data callbacks and SQLite
- one batched SQLite writer using WAL and reusable prepared UPSERT statements
- in-memory candle ring buffers for scanner decisions
- modular `IStrategy` interface and momentum strategy
- central `RiskManager` with strict configuration validation, position sizing, and exchange-style filters
- auditable paper fills with spread, slippage, fees, cooldown, stale-data checks, stops, take profit, and maximum holding time
- persistent completed-trade history in SQLite
- deterministic CSV replay through the same strategy, risk, fill, and exit code used by paper mode
- backtest metrics: Net Profit, Max Drawdown, Profit Factor, Win Rate, Expectancy, Sharpe, Sortino, fee share, and slippage sensitivity
- Binance Spot Testnet order state machine, startup reconciliation, User Data Stream handling, kill switch, and confirmed-position ledger
- controlled SIGINT/SIGTERM shutdown and optional ThreadSanitizer builds

## Runtime modes

### Paper trading

```bash
./client
```

The normal entry point starts `ExecutionEngine`, the collector, scanner, UI, and paper `TradeEngine`.

### Deterministic replay

```bash
./client --replay data/btcusdt.csv btcusdt
```

CSV format:

```text
timestamp_ms,price,volume
1710000000000,68250.10,0.25
1710000001000,68260.20,0.12
```

The volume column is optional. Events are stably sorted by timestamp and processed with `ReplayClock`.

### Binance Spot Testnet execution

The Phase 5 components are available under `src/sentum/trader/order/`, but `main()` does not currently expose a Testnet execution mode and `ExecutionEngine` does not yet route strategy signals through `LiveOrderSession`.

The implemented safety gate requires:

```bash
export SENTUM_ENABLE_SPOT_TESTNET=I_UNDERSTAND_TESTNET_ONLY
export SENTUM_BINANCE_TESTNET_API_KEY='...'
export SENTUM_BINANCE_TESTNET_API_SECRET='...'
```

The execution client is hard-wired to Binance Spot Testnet. Production execution and withdrawal endpoints are intentionally absent.

## Architecture

```text
Binance market WebSocket
        |
        v
Collector / parser
   |             |
   |             +--> bounded queue --> single SQLite writer --> WAL database
   |
   +--> in-memory ring buffers --> scanner --> TradeEngine
                                             |
                                             v
                                        IStrategy
                                             |
                                             v
                                        RiskManager
                                             |
                              +--------------+--------------+
                              |                             |
                              v                             v
                         paper mode                     replay mode
                              |                             |
                              +--------------+--------------+
                                             |
                                             v
                                   persistent trade history

Separate Phase 5 Testnet infrastructure:
Order intent --> OrderManager --> Binance Spot Testnet REST
                     ^                     |
                     |                     v
              reconciliation <----- User Data Stream
                     |
                     v
          ConfirmedPositionLedger
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

A successful REST placement response advances an order only to `acknowledged`. It does not create a local executed position. The confirmed-position ledger accepts a fill only when the exchange reports:

- state `filled`,
- a non-zero exchange order ID, and
- a positive executed quantity.

Cancellation remains `cancelling` until confirmed by User Data Stream or reconciliation.

## Requirements

- Linux
- C++17 compiler
- CMake 3.10+
- libcurl
- OpenSSL
- Boost.System
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

## Build

```bash
git clone https://github.com/DaveBeusing/Sentum.git
cd Sentum
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The standard build copies the stripped executable to `./client`.

### ThreadSanitizer

```bash
cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSENTUM_ENABLE_TSAN=ON
cmake --build build-tsan --parallel
```

## Configuration

Required files:

```text
config/config.json
config/risk.json
config/secrets.json
```

`config/config.json` controls quote asset, scanner threshold, database setting, and paper mode. The current normal runtime must use:

```json
"paperTrading": true
```

`config/risk.json` contains capital limits, risk per trade, stops, take profit, fees, spread, slippage, leverage, quantity filters, cooldown, maximum holding time, maximum data age, and trailing settings. Missing or invalid required values fail startup.

Do not commit real API credentials. Use keys with the minimum required permissions and no withdrawal rights.

## Persistence and observability

- market candles and trade history use SQLite
- the collector writes completed candles through a bounded queue
- the persistence batch size is 256 records
- the queue capacity is 8,192 records
- the documented drop-rate limit is 0.1%
- queue depth, accepted events, dropped events, and drop rate are logged periodically
- scanner decisions use the in-memory ring buffer rather than SQLite

## Development status

| Phase | Scope | Status |
|---|---|---|
| 1 | startup/shutdown stability, serialized strategy work, database retention, TSAN option | Merged; full soak validation still required |
| 2 | bounded persistence path, WAL, batching, prepared statements, in-memory scanner | Merged; production-load drop-rate validation still required |
| 3 | strategy interface, risk approval, sizing, paper fills, persistent trade audit | Merged; strategy and parameter validation still required |
| 4 | deterministic replay, shared event pipeline, metrics, slippage sensitivity | Merged; independent metric validation still required |
| 5 | Testnet OrderManager, User Data Stream, reconciliation, kill switch, confirmed ledger | Merged as infrastructure; runtime integration and real Testnet validation outstanding |

## Known limitations

- Phase 5 is not connected to the normal strategy execution path.
- `main()` supports paper mode and replay only.
- no production Binance execution endpoints are implemented.
- no real-money operation has been validated or approved.
- no documented 24-hour paper-trading soak test has been completed.
- no documented end-to-end Binance Spot Testnet integration test has been completed.
- User Data Stream reconnect and missed-event recovery still require deliberate fault testing.
- replay metrics have not yet been compared against an independent reference implementation.
- CI and ThreadSanitizer results should be reviewed before treating `master` as release-ready.

## Next milestones

1. Integrate `LiveOrderSession` behind an explicit Testnet-only runtime mode.
2. Connect confirmed exchange fills to the strategy portfolio state without allowing REST acknowledgements to mutate positions.
3. Add unit tests for every order-state transition and reconciliation case.
4. Add mocked REST/User Data Stream integration tests and reconnect recovery.
5. Complete release, TSAN, replay reproducibility, paper soak, and Testnet soak validation.
6. Add operational metrics, account reconciliation, and a documented incident runbook.
7. Consider production execution only in a separately reviewed change with a strict capital cap.

## Repository layout

```text
config/                         runtime and risk configuration
docs/                           safety and operational documentation
src/main.cpp                    paper/replay command-line entry point
src/sentum/api/                 Binance REST and WebSocket clients
src/sentum/backtest/            historical-event reader and metrics
src/sentum/collector/           market-data ingestion and persistence queue
src/sentum/core/                runtime orchestration
src/sentum/market/              unified market events and in-memory buffers
src/sentum/scanner/             in-memory symbol scanner
src/sentum/time/                system and replay clocks
src/sentum/trader/              strategy, risk, paper execution, history, and orders
src/sentum/utils/Database.*     SQLite candle persistence
```

See [`docs/LIVE_TRADING_SAFETY.md`](docs/LIVE_TRADING_SAFETY.md) for the Testnet safety model.

## Disclaimer

Sentum is experimental research and development software. It is not legal, tax, investment, or financial advice. Trading can result in substantial loss. Do not use Sentum with real capital unless you have independently reviewed the code, validated the strategy and risk model, tested failure modes, secured the operating environment, and accepted full responsibility for the outcome.

Read [`DISCLAIMER.md`](DISCLAIMER.md) before use.

## License

Copyright © 2025 Dave Beusing

Licensed under the MIT License. See [`LICENSE`](LICENSE).
