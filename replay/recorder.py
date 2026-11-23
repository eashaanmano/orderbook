#!/usr/bin/env python3
"""
Record Binance spot market data (L2 depth diffs + aggregated trades) into a
binary .obmd file that orderbook_convert / orderbook_backtest can read.

Why Python: recording is I/O bound and off the hot path; the engine never sees
this code. Prices/quantities are converted to integer ticks/lots with Decimal,
so no float error enters the file.

Book sync follows Binance's documented procedure for a local order book:
  1. open the diff stream and buffer events
  2. GET /api/v3/depth snapshot -> lastUpdateId
  3. drop events with u <= lastUpdateId
  4. first applied event must satisfy U <= lastUpdateId + 1 <= u
  5. afterwards every event must have U == previous u + 1, otherwise there is a
     gap: the current file segment is closed and a fresh snapshot starts a new one

Within one depth event, level DECREASES are written before INCREASES, so the
replayed book never crosses transiently because of record order.

Usage
  pip install websockets
  python3 replay/recorder.py --symbol BTCUSDT --hours 24 --out data/btcusdt.obmd
  # endpoints are configurable, e.g. the market-data-only mirrors:
  python3 replay/recorder.py --rest https://data-api.binance.vision \
                             --ws wss://data-stream.binance.vision ...
  # offline format check (no network):
  python3 replay/recorder.py --selftest --out /tmp/selftest.obmd
"""
import argparse
import asyncio
import json
import os
import signal
import struct
import sys
import time
import urllib.request
from decimal import Decimal

HEADER = struct.Struct("<8sdd16s")  # == MdFileHeader in common/include/orderbook/formats.hpp
RECORD = struct.Struct("<QqqBB6x")  # == MdRecord
LEVEL, TRADE = 0, 1
BUY, SELL = 0, 1


class GapError(Exception):
    pass


def http_json(url: str):
    with urllib.request.urlopen(url, timeout=15) as r:
        return json.load(r)


def fetch_filters(rest: str, symbol: str):
    info = http_json(f"{rest}/api/v3/exchangeInfo?symbol={symbol}")
    flt = {f["filterType"]: f for f in info["symbols"][0]["filters"]}
    return Decimal(flt["PRICE_FILTER"]["tickSize"]), Decimal(flt["LOT_SIZE"]["stepSize"])


class Writer:
    def __init__(self, path: str, tick: Decimal, lot: Decimal, symbol: str):
        self.path, self.tick, self.lot = path, tick, lot
        self.f = open(path, "wb")
        self.f.write(HEADER.pack(b"OBMD0001", float(tick), float(lot), symbol.encode()[:15]))
        self.count = 0

    def ticks(self, px: str) -> int:
        return int(Decimal(px) / self.tick)

    def lots(self, qty: str) -> int:
        return int(Decimal(qty) / self.lot)

    def level(self, ts_ns: int, side: int, px_ticks: int, qty_lots: int):
        self.f.write(RECORD.pack(ts_ns, px_ticks, qty_lots, LEVEL, side))
        self.count += 1

    def trade(self, ts_ns: int, aggressor: int, px_ticks: int, qty_lots: int):
        self.f.write(RECORD.pack(ts_ns, px_ticks, qty_lots, TRADE, aggressor))
        self.count += 1

    def close(self):
        self.f.close()


class LocalBook:
    """Mirror of the book, only used to order decreases before increases."""

    def __init__(self):
        self.levels = {BUY: {}, SELL: {}}

    def apply_event(self, w: Writer, ts_ns: int, bids, asks):
        dec, inc = [], []
        for side, rows in ((BUY, bids), (SELL, asks)):
            book = self.levels[side]
            for px_s, qty_s in rows:
                px, q = w.ticks(px_s), w.lots(qty_s)
                old = book.get(px, 0)
                if q == old:
                    continue
                (dec if q < old else inc).append((side, px, q))
                if q == 0:
                    book.pop(px, None)
                else:
                    book[px] = q
        for side, px, q in dec + inc:
            w.level(ts_ns, side, px, q)


def segment_path(base: str, n: int) -> str:
    if n == 0:
        return base
    root, ext = os.path.splitext(base)
    return f"{root}.{n}{ext}"


async def record_segment(args, tick, lot, path, deadline) -> bool:
    """Record one gap-free segment. Returns True when the deadline was reached."""
    import websockets  # imported lazily so --selftest works without it

    sym = args.symbol.lower()
    url = f"{args.ws}/stream?streams={sym}@depth@100ms/{sym}@aggTrade"
    w = Writer(path, tick, lot, args.symbol.upper())
    book = LocalBook()
    queue: asyncio.Queue = asyncio.Queue()
    try:
        async with websockets.connect(url, max_size=2**24, ping_interval=20) as ws:

            async def reader():
                async for raw in ws:
                    await queue.put(json.loads(raw))

            rtask = asyncio.create_task(reader())
            await asyncio.sleep(1.0)  # let some diffs buffer before the snapshot
            snap = await asyncio.to_thread(
                http_json, f"{args.rest}/api/v3/depth?symbol={args.symbol.upper()}&limit=5000")
            last_id = snap["lastUpdateId"]
            book.apply_event(w, time.time_ns(), snap["bids"], snap["asks"])
            print(f"[{path}] snapshot lastUpdateId={last_id}: "
                  f"{len(snap['bids'])} bids, {len(snap['asks'])} asks", flush=True)

            synced, prev_u, last_report = False, None, time.time()
            while time.time() < deadline:
                if rtask.done():
                    rtask.result()  # re-raise connection errors
                try:
                    msg = await asyncio.wait_for(queue.get(), timeout=5)
                except asyncio.TimeoutError:
                    continue
                stream, d = msg.get("stream", ""), msg.get("data", {})
                if stream.endswith("@aggTrade"):
                    if synced:
                        aggressor = SELL if d["m"] else BUY  # m: buyer is maker -> seller aggressed
                        w.trade(d["T"] * 1_000_000, aggressor, w.ticks(d["p"]), w.lots(d["q"]))
                    continue
                U, u = d["U"], d["u"]
                if not synced:
                    if u <= last_id:
                        continue
                    if not (U <= last_id + 1 <= u):
                        raise GapError(f"first event U={U} u={u} does not cover {last_id + 1}")
                    synced = True
                elif U != prev_u + 1:
                    raise GapError(f"sequence gap: expected U={prev_u + 1}, got {U}")
                prev_u = u
                book.apply_event(w, d["E"] * 1_000_000, d["b"], d["a"])
                if time.time() - last_report > 60:
                    print(f"[{path}] {w.count} records", flush=True)
                    w.f.flush()
                    last_report = time.time()
            rtask.cancel()
            return True
    finally:
        w.close()
        print(f"[{path}] closed with {w.count} records", flush=True)


async def main_async(args):
    tick, lot = fetch_filters(args.rest, args.symbol.upper())
    print(f"{args.symbol.upper()}: tick={tick} lot={lot}", flush=True)
    deadline = time.time() + args.hours * 3600
    seg = 0
    while time.time() < deadline:
        try:
            if await record_segment(args, tick, lot, segment_path(args.out, seg), deadline):
                break
        except GapError as e:
            print(f"gap: {e}; starting a new segment", flush=True)
        except Exception as e:  # network hiccup: back off, start a new segment
            print(f"error: {e!r}; reconnecting in 5 s", flush=True)
            await asyncio.sleep(5)
        seg += 1


def selftest(path: str):
    """Write a tiny canned session to check the format against the C++ reader."""
    w = Writer(path, Decimal("0.01"), Decimal("0.00001"), "SELFTEST")
    book = LocalBook()
    t = 1_700_000_000_000_000_000
    book.apply_event(w, t, [["100.00", "1.0"], ["99.99", "2.0"]], [["100.01", "1.5"], ["100.02", "3.0"]])
    w.trade(t + 1, BUY, w.ticks("100.01"), w.lots("0.5"))
    book.apply_event(w, t + 2, [["100.00", "0"]], [["100.01", "1.0"]])
    w.close()
    print(f"selftest: wrote {w.count} records to {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symbol", default="BTCUSDT")
    ap.add_argument("--hours", type=float, default=24.0)
    ap.add_argument("--out", default="data/btcusdt.obmd")
    ap.add_argument("--rest", default="https://api.binance.com")
    ap.add_argument("--ws", default="wss://stream.binance.com:9443")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        selftest(args.out)
        return
    signal.signal(signal.SIGINT, lambda *_: sys.exit(0))
    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
