#!/usr/bin/env bash
# Profile the replay with Linux perf. Frame pointers are kept by CMakeLists, so -g works.
#   ./scripts/perf.sh data/synth.obcmd
# Hardware counters (cache misses, branch misses) say WHY a hot spot is hot:
#   perf stat -e cycles,instructions,cache-misses,branch-misses ./build/orderbook_replay data/synth.obcmd
set -euo pipefail
CMDS=${1:-data/synth.obcmd}
perf record -g -o perf.data ./build/orderbook_replay "$CMDS"
perf report -i perf.data --no-children --percent-limit 2 --stdio | head -80
