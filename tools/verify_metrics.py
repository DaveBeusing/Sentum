#!/usr/bin/env python3
"""Independently recalculate Sentum replay metrics from the SQLite trade history.

Usage:
  python3 tools/verify_metrics.py log/klines.sqlite3 log/replay_metrics.json
"""

from __future__ import annotations

import json
import math
import sqlite3
import statistics
import sys
from pathlib import Path


def calculate(rows: list[tuple[float, float, float]]) -> dict[str, float]:
    # rows: net_profit, fees, entry_notional approximation
    profits = [r[0] for r in rows]
    fees = sum(r[1] for r in rows)
    net = sum(profits)
    wins = [p for p in profits if p >= 0]
    losses = [p for p in profits if p < 0]
    gross_wins = sum(wins)
    gross_losses = abs(sum(losses))

    equity = 0.0
    peak = 0.0
    max_drawdown = 0.0
    for profit in profits:
        equity += profit
        peak = max(peak, equity)
        max_drawdown = max(max_drawdown, peak - equity)

    returns = [p / n for p, _, n in rows if n > 0]
    mean = statistics.fmean(returns) if returns else 0.0
    std = statistics.pstdev(returns) if len(returns) > 1 else 0.0
    downside = [min(0.0, value) for value in returns]
    downside_dev = math.sqrt(sum(v * v for v in downside) / len(downside)) if downside else 0.0
    scale = math.sqrt(len(returns)) if returns else 0.0

    return {
        "trades": float(len(rows)),
        "net_profit": net,
        "max_drawdown": max_drawdown,
        "profit_factor": gross_wins / gross_losses if gross_losses > 0 else math.inf,
        "win_rate": (len(wins) / len(rows) * 100.0) if rows else 0.0,
        "expectancy": net / len(rows) if rows else 0.0,
        "sharpe": mean / std * scale if std > 0 else 0.0,
        "sortino": mean / downside_dev * scale if downside_dev > 0 else 0.0,
        "fee_share": fees / (abs(net) + fees) if abs(net) + fees > 0 else 0.0,
    }


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    database, report_path = map(Path, sys.argv[1:])
    report = json.loads(report_path.read_text())
    with sqlite3.connect(database) as connection:
        rows = connection.execute(
            "SELECT net_profit, fees, ABS(entry_price * quantity) FROM trades ORDER BY id"
        ).fetchall()
    reference = calculate([(float(a), float(b), float(c)) for a, b, c in rows])
    tolerance = 1e-6
    failed = False
    for key, expected in reference.items():
        actual = float(report[key])
        equal = (math.isinf(expected) and math.isinf(actual)) or math.isclose(actual, expected, rel_tol=tolerance, abs_tol=tolerance)
        print(f"{key}: sentum={actual:.10g} reference={expected:.10g} {'OK' if equal else 'MISMATCH'}")
        failed |= not equal
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
