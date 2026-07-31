#!/usr/bin/env python3
"""Independently recalculate Sentum replay metrics from persisted replay trades.

Usage:
  python3 tools/verify_metrics.py log/replay.sqlite3 log/replay_metrics.json
"""

from __future__ import annotations

import json
import math
import sqlite3
import statistics
import sys
from pathlib import Path


def sample_deviation(values: list[float], center: float) -> float:
    return math.sqrt(sum((value - center) ** 2 for value in values) / (len(values) - 1)) if len(values) > 1 else 0.0


def calculate(rows: list[tuple[float, float, float]]) -> dict[str, float]:
    profits = [row[0] for row in rows]
    fees = sum(row[1] for row in rows)
    net = sum(profits)
    wins = [profit for profit in profits if profit > 0.0]
    losses = [profit for profit in profits if profit <= 0.0]
    gross_wins = sum(wins)
    gross_losses = abs(sum(losses))

    equity = peak = max_drawdown = 0.0
    for profit in profits:
        equity += profit
        peak = max(peak, equity)
        max_drawdown = max(max_drawdown, peak - equity)

    returns = [profit / max(1.0, notional) for profit, _, notional in rows]
    mean = statistics.fmean(returns) if returns else 0.0
    deviation = sample_deviation(returns, mean)
    negative = [value for value in returns if value < 0.0]
    downside = sample_deviation(negative, 0.0)
    scale = math.sqrt(len(returns)) if returns else 0.0

    return {
        "trades": float(len(rows)),
        "net_profit": net,
        "max_drawdown": max_drawdown,
        "profit_factor": gross_wins / gross_losses if gross_losses > 0.0 else math.inf,
        "win_rate": len(wins) / len(rows) * 100.0 if rows else 0.0,
        "expectancy": net / len(rows) if rows else 0.0,
        "sharpe": mean / deviation * scale if deviation > 0.0 else 0.0,
        "sortino": mean / downside * scale if downside > 0.0 else 0.0,
        "fee_share": fees / (abs(net) + fees) if abs(net) + fees > 0.0 else 0.0,
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
    failed = False
    for key, expected in reference.items():
        actual = float(report[key])
        equal = (math.isinf(expected) and math.isinf(actual)) or math.isclose(actual, expected, rel_tol=1e-6, abs_tol=1e-6)
        print(f"{key}: sentum={actual:.10g} reference={expected:.10g} {'OK' if equal else 'MISMATCH'}")
        failed |= not equal
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
