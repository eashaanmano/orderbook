// orderbook_replay: single-threaded replay of a .obcmd stream through the engine.
//
// Pass 1 measures throughput (no per-op timing in the loop).
// Pass 2 measures per-command latency into a log-linear histogram.
// Both passes print a trade digest: identical digests = deterministic engine.
//
// Usage: orderbook_replay in.obcmd [maxOrders=2097152] [core=-1]

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "orderbook/apply.hpp"
#include "orderbook/digest.hpp"
#include "orderbook/latency_histogram.hpp"
#include "orderbook/sys.hpp"

using namespace ob;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s in.obcmd [maxOrders=2097152] [core=-1]\n", argv[0]);
        return 2;
    }
    const std::uint32_t maxOrders = argc > 2 ? static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)) : (1u << 21);
    const int           core      = argc > 3 ? std::atoi(argv[3]) : -1;
    pinThisThread(core);

    const CmdFile f = loadCmds(argv[1]);
    const EngineConfig cfg{f.header.minPrice, f.header.numLevels, maxOrders};
    std::printf("%zu commands, window %u levels from %lld\n", f.cmds.size(), cfg.numLevels, (long long)cfg.minPrice);

    // ---- pass 1: throughput
    {
        auto        book = std::make_unique<OrderBook>(cfg);
        TradeDigest dig;
        const auto  t0 = nowNs();
        for (const Command& c : f.cmds) apply(*book, c, [&](const Trade& t) { dig.add(t); });
        const auto  dt = nowNs() - t0;
        std::printf("throughput: %.2f M cmds/s  (%.1f ns/cmd avg)  trades=%llu digest=%016llx\n",
                    f.cmds.size() / (dt / 1e3), double(dt) / f.cmds.size(), (unsigned long long)dig.count(),
                    (unsigned long long)dig.value());
    }

    // ---- pass 2: latency per command
    {
        // Clock overhead: two back-to-back reads.
        LatencyHistogram overhead;
        for (int i = 0; i < 100000; ++i) {
            const auto a = nowNs();
            const auto b = nowNs();
            overhead.record(b - a);
        }

        auto             book = std::make_unique<OrderBook>(cfg);
        TradeDigest      dig;
        LatencyHistogram all, hNew, hCancel, hModify;
        for (const Command& c : f.cmds) {
            const auto a = nowNs();
            apply(*book, c, [&](const Trade& t) { dig.add(t); });
            const auto d = nowNs() - a;
            all.record(d);
            (c.type == CmdType::New ? hNew : c.type == CmdType::Cancel ? hCancel : hModify).record(d);
        }
        overhead.print("clock overhead");
        all.print("all commands");
        hNew.print("  new");
        hCancel.print("  cancel");
        hModify.print("  modify");
        std::printf("pass 2 digest=%016llx (must equal pass 1)\n", (unsigned long long)dig.value());
        std::printf("resting orders at end: %llu\n", (unsigned long long)book->orderCount());
    }
    return 0;
}
