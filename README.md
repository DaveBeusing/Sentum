# Sentum

**Deterministic market replay, auditable paper trading, and exchange-confirmed order execution in C++17.**

Sentum is an experimental Binance Spot trading system focused on predictable runtime behavior, traceable trading decisions, bounded market-data processing, and fail-closed execution safety.

> **Project status:** active development. Paper trading and deterministic replay are integrated. A Binance Spot Testnet execution layer exists, but it is not yet wired into the normal strategy runtime or validated end to end against a real Testnet account.

## Capabilities

- Binance market-data collection over TLS-secured WebSockets
- Bounded, non-blocking persistence queue between market data and SQLite
- Dedicated batched SQLite writer using WAL and prepared UPSERT statements
- In-memory candle ring buffers for scanner and strategy decisions
- Modular `IStrategy` interface with a momentum strategy implementation
- Central `RiskManager` with position sizing and exchange filters
- Auditable paper fills including spread, slippage, fees, stops, take profit, cooldown, and maximum holding time
- Deterministic CSV replay through the same strategy, risk, fill, and exit path used by paper mode
- Backtest metrics including profit, drawdown, profit factor, win rate, expectancy, Sharpe, Sortino, fees, and slippage sensitivity
- Binance Spot Testnet order-management infrastructure with reconciliation, User Data Stream handling, and a kill switch
- Exchange-confirmed position ledger: REST acknowledgements and local order intent never count as executed positions
- Idempotent startup/shutdown handling and optional ThreadSanitizer builds

## Architecture

```text
Binance Market WebSocket
          |
          v
   Collector / Parser
      |           |
      |           +--> bounded persistence queue --> SQLite writer --> WAL database
      |
      +--> in-memory candle buffers --> scanner / strategy
                                      |
                                      v
                                 RiskManager
                                      |
                         +------------+-------------+
                         |                          |
                         v                          v
                  Paper execution             Replay execution
                         |                          |
                         +------------+-------------+
                                      |
                                      v
                              completed trades
                                      |
                                      v
                              backtest metrics

Phase 5 execution infrastructure:
Strategy intent --> OrderManager --> Binance Spot Testnet REST
                         ^                    |
                         |                    v
                  reconciliation <-- User Data Stream
                         |
                         v
              ConfirmedPositionLedger
```

The Phase 5 components are currently infrastructure only. The normal runtime created by `main()` still starts `ExecutionEngine`; it does not yet route strategy signals through `LiveOrderSession`.

## Execution and state model

Sentum treats the exchange as the authority for live order state.

Supported order states:

```text
pending
acknowledged
partially filled
filled
cancelling
cancelled
rejected
```

A successful REST placement response moves an order only to `acknowledged`. It does **not** create or change a local position. The confirmed position ledger accepts a fill only when Binance reports:

- state `filled`,
- a non-zero exchange order ID, and
- a positive executed quantity.

Partial fills remain order state. A cancellation remains `cancelling` until the User Data Stream or reconciliation confirms the terminal exchange status.

## Requirements

Sentum currently targets Linux and requires:

- C++17 compiler
- CMake 3.10 or newer
- libcurl
- OpenSSL
- Boost.System
- WebSocket++
- SQLite3
- POSIX threads
- nlohmann/json headers

On Debian or Ubuntu:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake git \
  libcurl4-openssl-dev libssl-dev \
  libboost-system-dev libasio-dev \
  libwebsocketpp-dev \
  sqlite3 libsqlite3-dev \
  nlohmann-json3-dev
```

## Build

```bash
git clone https://github.com/DaveBeusing/Sentum.git
cd Sentum

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The normal build copies the stripped executable to the repository root as `./client`.

### ThreadSanitizer build

```bash
cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSENTUM_ENABLE_TSAN=ON
cmake --build build-tsan --parallel
```

ThreadSanitizer requires GCC or Clang. The TSAN executable remains in `build-tsan/client`.

## Configuration

Runtime configuration is stored under `config/`.

### `config/config.json`

```json
{
  "quoteAsset": "USDC",
  "minCumulativeReturn": 0.0,
  "databasePath": "log/sentum.sqlite3",
  "paperTrading": true
}
```

Keep `paperTrading` enabled unless a separately reviewed and validated live integration is in place.

### `config/risk.json`

This file controls capital limits, risk per trade, stops, take profit, fees, simulated spread and slippage, leverage, exchange quantity filters, cooldown, maximum holding time, market-data age, and trailing exits.

The values committed to the repository are development defaults, not investment recommendations. Review every value before running the application.

### Legacy market-data credentials

The normal runtime may require `config/secrets.json` for its Binance connections:

```json
{
  "api_key": "your_binance_api_key",
  "api_secret": "your_binance_api_secret"
}
```

Do not commit real credentials. Use an API key with only the minimum required permissions and no withdrawal permission.

## Run paper mode

From the repository root:

```bash
./client
```

The process handles `SIGINT` and `SIGTERM`, actively closes its WebSocket connections, drains the persistence queue during controlled shutdown, and preserves the SQLite database between restarts.

## Deterministic replay

Replay accepts CSV records in the following format:

```text
timestamp_ms,price,volume
1710000000000,68250.10,0.25
1710000001000,68260.20,0.12
```

The volume column is optional.

Run a replay with:

```bash
./client --replay data/btcusdt.csv btcusdt
```

The replay clock advances to each event timestamp. Sentum prints:

- trade count
- net profit
- maximum drawdown
- profit factor
- win rate
- expectancy
- Sharpe ratio
- Sortino ratio
- fee share
- slippage sensitivity using a deterministic 2x-slippage comparison

Replay currently loads `config/risk.json` and uses an in-memory SQLite database.

## Binance Spot Testnet safety

Phase 5 is hard-wired to Binance Spot Testnet. Production REST and WebSocket endpoints are intentionally absent from the execution client.

Required environment variables:

```bash
export SENTUM_ENABLE_SPOT_TESTNET=I_UNDERSTAND_TESTNET_ONLY
export SENTUM_BINANCE_TESTNET_API_KEY='...'
export SENTUM_BINANCE_TESTNET_API_SECRET='...'
```

Use a Testnet API key that allows Spot trading only. Never enable withdrawals. Sentum neither needs nor implements withdrawal endpoints.

`LiveOrderSession::start()` reconciles open exchange orders before accepting any new order. If reconciliation fails, startup fails closed.

The kill switch:

1. rejects new orders,
2. attempts to cancel pending, acknowledged, and partially filled orders,
3. waits for Binance confirmation before treating cancellation as final,
4. activates when listen-key keepalive fails, and
5. activates during orderly shutdown.

See [`docs/LIVE_TRADING_SAFETY.md`](docs/LIVE_TRADING_SAFETY.md) for the complete safety model.

> Setting the Testnet environment variables does not currently enable live execution from `main()`. The Phase 5 session still needs explicit integration with `TradeEngine`/`ExecutionEngine`.

## Development phases

| Phase | Scope | Status |
|---|---|---|
| 1 | Runtime stability, safe shutdown, serial strategy queue, database retention, TSAN build | Implemented; runtime and CI verification still required |
| 2 | Bounded market-data pipeline, single SQLite writer, WAL, batch persistence, in-memory scanning | Implemented; production-load soak test still required |
| 3 | Strategy interface, risk approval, position sizing, paper execution, auditable trade history | Implemented; live paper soak test still required |
| 4 | Deterministic replay, shared event pipeline, backtest metrics, slippage sensitivity | Implemented; reproducibility and independent metric validation still required |
| 5 | Testnet OrderManager, User Data Stream, reconciliation, kill switch, confirmed position ledger | Infrastructure implemented; end-to-end integration and Testnet validation outstanding |

## Known limitations

- Phase 5 is not connected to the normal strategy execution path.
- No production Binance endpoints are implemented.
- No real-money operation has been validated or approved.
- Release and ThreadSanitizer builds have not been independently confirmed in this documentation update.
- No 24-hour paper-trading soak test is documented as completed.
- No real Binance Spot Testnet integration test is documented as completed.
- User Data Stream interruption, reconnect, and backfill behavior still require deliberate fault testing.
- Replay metrics have not yet been benchmarked against an independent reference implementation.

## Next milestones

1. Integrate `LiveOrderSession` behind an explicit Testnet-only runtime mode.
2. Add automated unit tests for order-state transitions and confirmed-position accounting.
3. Add mocked REST/User Data Stream integration tests.
4. Make release and ThreadSanitizer CI green and retain the results.
5. Complete paper-trading and Testnet soak tests with fault injection.
6. Add account and balance reconciliation plus operational observability.
7. Consider production execution only as a separate reviewed change with a strict capital cap and dedicated runbook.

## Repository layout

```text
config/                  Runtime and risk configuration
src/sentum/api/          Binance REST and WebSocket clients
src/sentum/backtest/     Historical event loading and metrics
src/sentum/core/         Runtime orchestration
src/sentum/database/     SQLite persistence
src/sentum/time/         System and replay clocks
src/sentum/trader/       Trade engine, strategy, risk, and execution logic
src/sentum/trader/order/ Phase 5 order state and position confirmation
docs/                    Operational and safety documentation
```

## Contributing

Pull requests are welcome. For substantial changes, open an issue first to discuss the design, safety impact, and validation plan. Changes involving exchange execution should include tests, failure behavior, credential handling, and state-reconciliation details.

## Disclaimer

Sentum is experimental software for research and development. Nothing in this repository is legal, tax, investment, or financial advice. Trading can result in substantial loss. Read [`DISCLAIMER.md`](DISCLAIMER.md) before using the software.

Do not use Sentum with real capital unless you have independently reviewed the code, validated the strategy and risk model, tested failure modes, secured the operating environment, and accepted full responsibility for the outcome.

## License

Copyright © 2025 Dave Beusing

Licensed under the MIT License. See [`LICENSE`](LICENSE) for details.
