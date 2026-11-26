# OrderBook — a low-latency limit order book and matching engine in C++20

A price-time-priority matching engine with limit, market, IOC and FOK orders, plus cancel and modify. It has a zero-allocation hot path and is checked against a slow reference engine by differential fuzzing. It replays real Binance market data through a lock-free threaded pipeline, and an Avellaneda–Stoikov market maker runs on top.

```
                       (recorded Binance L2 + trades, or synthetic)
  .obmd ──► orderbook_convert ──► .obcmd
                                    │
                                    ▼
  gateway thread ──► SPSC ring ──► matching engine ──► SPSC ring ──► market-data publisher
  (paced replay)    (lock-free)    (single thread,    (lock-free)    (trades, BBO, acks,
                                    owns the book)                   latency histogram)

  orderbook_backtest:  .obmd ──► L2Converter ──► engine ◄──► market maker (simulated latency)
```

## Repo layout

| Path | What |
|---|---|
| `engine/include/orderbook/order_book.hpp` | The engine: flat price-level arrays, intrusive FIFO lists, pool, hash index, bitmap |
| `engine/include/orderbook/{object_pool,flat_hash_map,level_bitmap}.hpp` | The building blocks, each benchmarked against its naive alternative |
| `reference/include/orderbook/reference_book.hpp` | Slow, obviously correct engine (`std::map` + `std::list`), the fuzzing oracle |
| `common/include/orderbook/spsc_ring.hpp` | Lock-free SPSC ring, cache-line padded, cached indices |
| `common/include/orderbook/latency_histogram.hpp` | HdrHistogram-style log-linear histogram (in-repo, ~100 lines) |
| `tests/unit_tests.cpp` | 21 scenario tests, one per rule |
| `tests/fuzz_diff.cpp` | Differential fuzzer: engine vs reference, every op, every trade, full-book snapshots |
| `bench/bench_engine.cpp` | Google Benchmark: engine vs reference, FlatHashMap vs `unordered_map`, pool vs `new` |
| `replay/recorder.py` | Binance depth + aggTrade recorder → `.obmd` |
| `replay/l2_converter.hpp` | Turns aggregated levels into synthetic individual orders (see caveat below) |
| `replay/{gen,convert,replay,pipeline}_main.cpp` | Synthetic data generator, converter, single-thread replay, threaded pipeline |
| `strategy/market_maker.hpp`, `strategy/backtest_main.cpp` | Inventory-skewed quoter and its deterministic backtester |
| `scripts/` | `run_all.sh`, `perf.sh`, `analyze_backtest.py` |

## Build and run

```bash
./scripts/run_all.sh                      # build, test, fuzz, generate, convert, replay, pipeline, backtest, bench

# or step by step
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # -DORDERBOOK_BENCH=OFF to skip fetching Google Benchmark
cmake --build build -j
./build/unit_tests
./build/fuzz_diff 200 100000              # seeds, ops per seed
ctest --test-dir build

# real data (needs network access to Binance)
pip install websockets
python3 replay/recorder.py --symbol BTCUSDT --hours 24 --out data/btcusdt.obmd
./build/orderbook_convert  data/btcusdt.obmd data/btcusdt.obcmd
./build/orderbook_replay   data/btcusdt.obcmd 2097152 2          # pin to core 2
./build/orderbook_pipeline data/btcusdt.obcmd --cores 2,4,6 --rate 500000
./build/orderbook_backtest data/btcusdt.obmd --latency-us 300 --csv data/bt.csv
python3 scripts/analyze_backtest.py data/bt.csv
```

Sanitizer build for the tests: `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DORDERBOOK_SANITIZE=ON`.

## 1. Order book core: design choices

| Choice | Instead of | Why |
|---|---|---|
| Flat array of levels indexed by `price - minPrice` | `std::map<Price, Level>` | O(1) level lookup, no tree rebalancing, no pointer chasing, contiguous memory. The cost is a fixed price window, which is fine for one instrument whose price stays inside the window. |
| Intrusive doubly linked list per level, 32-bit indices | `std::list` / `std::deque` | O(1) cancel from the middle of the queue; `Order` is 32 bytes (2 per cache line); no per-node allocation |
| `ObjectPool<Order>` preallocated | `new`/`delete` | The hot path never calls the allocator: no locks inside malloc, no fragmentation, predictable latency |
| `FlatHashMap` (open addressing, linear probing, backward-shift delete) | `std::unordered_map` | No node allocation; a lookup is usually one cache line; deletion leaves no tombstones, so probe lengths don't degrade |
| `LevelBitmap` per side | Linear scan for the next level | When the best level empties, the next one is found 64 levels per `clz`/`ctz` instruction |
| Integer ticks and lots | `double` | Exact arithmetic and equality; floats never touch matching |

## 2. Matching semantics

Price-time priority: the best price matches first, and at the same price the earlier order matches first. Trades print at the maker's price.

| Command | Behaviour |
|---|---|
| Limit | Match whatever crosses, rest the remainder |
| Market | Match at any price; cancel the remainder |
| IOC | Match up to the limit price; cancel the remainder |
| FOK | Pre-check available liquidity within the limit; if short, reject with **no** trades |
| Cancel | Remove the order (O(1) via the hash index) |
| Modify | Same price and qty reduced or unchanged → in place, **keeps queue priority**. A price change or qty increase → cancel and re-enter, which loses priority and may match immediately |

Rejects: `BadQty`, `DuplicateId`, `BadPrice` (outside the window), `CapacityExhausted` (pool full; conservative, checked before matching), `FokUnfillable`, `UnknownId`. Not implemented, and worth saying so: self-trade prevention, iceberg orders, stop orders, and market-order price protection.

## 3. Correctness

* **Differential fuzzing** (`fuzz_diff`). Random streams of limit, market, IOC, FOK, cancel and modify orders, about 3% of them deliberately invalid, run through both engines. After every operation the `Result` and the full trade sequence must be identical. Every 500 operations the entire book must be identical: every level, and every order in queue order. Engine invariants are checked too: level total equals the sum of its orders, the index count matches, and the book is never crossed. Three book shapes rotate:
  * dense: constant matching
  * edges: 130 levels, deliberately not a multiple of 64, to hit bitmap word boundaries and window edges
  * sparse: 65,536 levels, long bitmap scans
* **The fuzzer was mutation-tested.** I planted two bugs and checked that it caught both. One let a quantity increase keep priority; the other was an off-by-one in the bitmap scan that skips a level.
* **Determinism.** Single-thread replay, a second replay pass, and the 3-thread pipeline all produce the same order-sensitive trade digest.
* **Unit tests and sanitizers.** The unit tests and fuzzer pass under ASan and UBSan. The SPSC ring and pipeline pass under TSan.

## 4. Market-data replay — and its honest caveat

> **Binance's public depth feed publishes aggregated quantity per price level, not individual orders.** Nobody outside the exchange knows the real queue. `L2Converter` *invents* orders that reproduce the published levels.

* A level increase becomes one new limit order for the difference, joining the back of the queue.
* A level decrease removes quantity from the **back** of the queue: whole orders are cancelled, then the last partial order is modified down. This assumes cancels are LIFO.
* A trade (from the `aggTrade` stream) becomes an IOC on the aggressor side. It consumes the front of the queue, like the real trade did, so the next depth update has little left to reconcile.
* Reconciliation is always against the engine's actual state, so the replayed book cannot drift from the feed.
* What this measures: engine speed under a **realistic mix and rate** of adds, cancels and trades. What it cannot claim: a true reconstruction of queue positions.
* Depth arrives as 100 ms diffs, so several real orders get merged into one synthetic order. The real message rate is higher than what we replay.

The recorder follows Binance's documented snapshot plus diff-stream sync procedure. On any sequence gap it starts a fresh file segment rather than guessing.

## 5. Market-making strategy

`strategy/market_maker.hpp` is a simplified Avellaneda–Stoikov quoter:

```
reservation  r      = mid − q·γ·σ²·T
spread       δ_tot  = γ·σ²·T + (2/γ)·ln(1 + γ/k)
quotes       bid = ⌊r − δ_tot/2⌋,  ask = ⌈r + δ_tot/2⌉   (post-only, inventory-capped)
```

σ is estimated online with an EWMA. T is a constant horizon. Orders reach the engine after a configurable one-way latency, and the quoter is throttled. Fees are charged per fill, and P&L is reported **gross and net of fees**. It also reports the fill rate, max inventory, and the **1-second markout** of fills: the average mid move against you after a fill, which measures adverse selection.

Backtest limits:

* Fills come from real trades consuming the synthetic queue, so queue position is approximate.
* A simulated fill doesn't move the market, so impact is ignored.
* Latency is a constant, with no jitter.

## 6. Results

Measured on an **AMD Ryzen 7 170 (8 cores / 16 threads), WSL2**, not core-isolated. Treat these as relative comparisons; absolute numbers vary run to run.

**Google Benchmark** (`bench_engine`)

| Benchmark | Engine / flat | Naive | Speed-up |
|---|---|---|---|
| Mixed flow, 200k ops (55% limit, 30% cancel, 7% modify, 8% IOC) | 15.5 M ops/s | 1.25 M ops/s (`std::map` reference) | ~12× |
| Add + cancel on a 50k-order book | 68.6 ns per pair | 1,194 ns | ~17× |
| Order index churn, **random** keys | 60.9 ns per op triple | 135 ns (`unordered_map`) | ~2.2× |
| Order index churn, **sequential** keys | 61.5 ns | 49.5 ns (`unordered_map`) | **~0.8× (slower)** |
| Pool vs heap, alloc + free | ~0.7 ns per op | ~19.8 ns | ~28× |

**One result went against my expectation.** With sequential order IDs, `std::unordered_map` beats my flat map by about 20% (49.5 ns vs 61.5 ns). libstdc++ hashes integers with the identity function, so sequential keys land in sequential buckets and nodes come out of the allocator in order: near-perfect locality. My map scrambles keys with splitmix64 across an 8 MB table. With random keys, `unordered_map` loses 2.2×. Exchange order IDs *are* mostly sequential, so the honest conclusion is that the flat map's advantage is predictable latency (no allocation, no tail spikes) more than raw speed. An identity-hash variant is the next experiment.

**Replay of 2.17 M synthetic commands** (`orderbook_replay`)

* Throughput: about 17 M commands/s (59 ns per command on average)
* Per-command latency: p50 66 ns, p99 415 ns, p99.9 647 ns. Each timing includes about 21 ns of `steady_clock` overhead.
* The p99.99 and max values are page faults and container noise.

**Threaded pipeline:** 4.7 M commands/s unpaced on 2 vCPUs. Its latency numbers there are meaningless, because three busy-spinning threads share two cores. Measure on a machine with at least 3 free physical cores, using `--cores` and `--rate`.

**Market maker on synthetic data** (1 hour simulated, 200 µs latency, 1 bp maker fee)

* Gross P&L: +0.45
* Fees: 101.16
* **Net P&L: −100.7**
* Fill rate: 3.8%
* 1 s markout: +3 ticks per lot

Spread capture roughly breaks even, and the fees wipe it out. Correlation analysis shows P&L moves with inventory × mid change, so it is mostly inventory risk. That is the expected result for a naive maker paying fees. It is also synthetic data, so it says nothing about real BTCUSDT.

## License

MIT
