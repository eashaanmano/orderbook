#!/usr/bin/env bash
# Build, test, and run the full chain on synthetic data. No network needed
# except the first cmake run (fetches Google Benchmark; pass -DORDERBOOK_BENCH=OFF to skip).
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

echo "== unit tests";            ./build/unit_tests | tail -1
echo "== differential fuzz";     ./build/fuzz_diff 60 50000
mkdir -p data
echo "== generate synthetic L2"; ./build/orderbook_gen data/synth.obmd 2000000
echo "== convert to orders";     ./build/orderbook_convert data/synth.obmd data/synth.obcmd
echo "== single-thread replay";  ./build/orderbook_replay data/synth.obcmd
echo "== threaded pipeline";     ./build/orderbook_pipeline data/synth.obcmd
echo "== market maker backtest"; ./build/orderbook_backtest data/synth.obmd --csv data/backtest.csv
if [[ -x build/bench_engine ]]; then
  echo "== benchmarks";          ./build/bench_engine --benchmark_min_time=0.3s
fi
