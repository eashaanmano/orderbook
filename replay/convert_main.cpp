// orderbook_convert: .obmd (L2 levels + trades) -> .obcmd (individual engine commands).
//
// Runs the L2Converter against a live OrderBook, so every decision is made
// against the true engine state, and writes out the commands it emitted.
// Because the engine is deterministic, replaying the .obcmd into a fresh engine
// reproduces exactly the same book and trades.
//
// Usage: orderbook_convert in.obmd out.obcmd [windowLevels=1048576] [maxOrders=2097152]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "l2_converter.hpp"
#include "orderbook/formats.hpp"

using namespace ob;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s in.obmd out.obcmd [windowLevels=1048576] [maxOrders=2097152]\n", argv[0]);
        return 2;
    }
    const std::uint32_t window    = argc > 3 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)) : (1u << 20);
    const std::uint32_t maxOrders = argc > 4 ? static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10)) : (1u << 21);

    MdFile md = loadMd(argv[1]);
    if (md.records.empty()) {
        std::fprintf(stderr, "no records\n");
        return 1;
    }

    // Centre the price window on the median price of the first 10k records.
    std::vector<Price> early;
    for (std::size_t i = 0; i < md.records.size() && early.size() < 10000; ++i)
        if (md.records[i].qty > 0) early.push_back(md.records[i].price);
    std::nth_element(early.begin(), early.begin() + early.size() / 2, early.end());
    const Price centre = early[early.size() / 2];

    EngineConfig cfg{centre - static_cast<Price>(window / 2), window, maxOrders};
    OrderBook    book(cfg);
    L2Converter  conv(book);

    std::vector<Command> cmds;
    cmds.reserve(md.records.size() * 2);
    std::uint64_t trades = 0, rejects = 0, capacityRejects = 0;
    for (const MdRecord& r : md.records) {
        conv.onRecord(r, [&](const Command& c) {
            const Result res = apply(book, c, [&](const Trade&) { ++trades; });
            if (res.status == Status::Rejected) {
                ++rejects;
                if (res.reject == Reject::CapacityExhausted) ++capacityRejects;
            }
            cmds.push_back(c);
        });
    }

    CmdFileHeader h{};
    h.tickSize  = md.header.tickSize;
    h.lotSize   = md.header.lotSize;
    h.minPrice  = cfg.minPrice;
    h.numLevels = cfg.numLevels;
    saveCmds(argv[2], h, cmds);

    const auto& s = conv.stats();
    std::printf("symbol %.16s  tick %g  lot %g\n", md.header.symbol, md.header.tickSize, md.header.lotSize);
    std::printf("window: [%lld, %lld) ticks (%u levels per side)\n", (long long)cfg.minPrice,
                (long long)(cfg.minPrice + cfg.numLevels), cfg.numLevels);
    std::printf("input:  %zu records (%llu level updates, %llu trades, %llu outside window)\n", md.records.size(),
                (unsigned long long)s.levelUpdates, (unsigned long long)s.trades, (unsigned long long)s.outOfWindow);
    std::printf("output: %zu commands (adds %llu, cancels %llu, reduces %llu, iocs %llu)\n", cmds.size(),
                (unsigned long long)s.adds, (unsigned long long)s.cancels, (unsigned long long)s.reduces,
                (unsigned long long)s.iocs);
    std::printf("engine: %llu trades, %llu rejects (%llu capacity), %llu orders resting at end\n",
                (unsigned long long)trades, (unsigned long long)rejects, (unsigned long long)capacityRejects,
                (unsigned long long)book.orderCount());
    if (capacityRejects) std::printf("WARNING: raise maxOrders, the book diverged from the feed\n");
    return 0;
}
