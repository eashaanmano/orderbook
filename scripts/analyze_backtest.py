#!/usr/bin/env python3
"""Plot the backtest time series: P&L (gross vs net), inventory, mid.

Usage: python3 scripts/analyze_backtest.py data/backtest.csv [out.png]
Needs: pip install pandas matplotlib
"""
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

path = sys.argv[1] if len(sys.argv) > 1 else "data/backtest.csv"
out = sys.argv[2] if len(sys.argv) > 2 else path.replace(".csv", ".png")
df = pd.read_csv(path)
df["gross_pnl"] = df["mtm_pnl"] + df["fees"]

fig, ax = plt.subplots(3, 1, figsize=(11, 9), sharex=True)
ax[0].plot(df.t_s, df.gross_pnl, label="gross (before fees)")
ax[0].plot(df.t_s, df.mtm_pnl, label="net")
ax[0].set_ylabel("P&L (quote ccy)")
ax[0].legend()
ax[1].plot(df.t_s, df.inventory_lots, color="tab:orange")
ax[1].axhline(0, color="grey", lw=0.5)
ax[1].set_ylabel("inventory (lots)")
ax[2].plot(df.t_s, df.mid, color="tab:green")
ax[2].set_ylabel("mid (ticks)")
ax[2].set_xlabel("time (s)")
fig.suptitle("Market maker backtest")
fig.tight_layout()
fig.savefig(out, dpi=120)

print(f"saved {out}")
print(f"final net P&L {df.mtm_pnl.iloc[-1]:.4f}, gross {df.gross_pnl.iloc[-1]:.4f}, fees {df.fees.iloc[-1]:.4f}")
print(f"inventory: mean {df.inventory_lots.mean():.0f}, std {df.inventory_lots.std():.0f}, "
      f"min {df.inventory_lots.min()}, max {df.inventory_lots.max()}")
corr = df.gross_pnl.diff().corr(df.mid.diff() * df.inventory_lots.shift())
print(f"corr(dPnL, inventory * dMid) = {corr:.2f}  (near 1 => P&L is mostly inventory risk, not spread capture)")
