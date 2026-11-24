// orderbook_backtest: run the market maker against recorded (or synthetic) market data.
//
// Event loop, all single-threaded and deterministic:
//   for each market record (in arrival order):
//     1. deliver strategy commands whose (send time + latency) <= now
//          New:    post-only check, then into the engine
//          Cancel: into the engine (may fail if already filled)
//     2. convert the record into synthetic orders and apply them; synthetic IOCs
//        (from real trades) are what fill the strategy's resting quotes
//     3. show the strategy the external BBO; it may queue new commands
//
// Realism limits (put these in the README, they matter):
//   * our quotes join the BACK of the synthetic queue at their price, but the
//     synthetic queue's own ordering is invented (see l2_converter.hpp)
//   * a real trade that fills us in the sim also filled us INSTEAD of someone else
//     in reality, without moving the market; market impact is ignored
//   * latency is a constant one-way delay; no jitter, no exchange-side queueing
//
// Usage: orderbook_backtest in.obmd [--latency-us 200] [--gamma 0.01] [--k 0.3]
//          [--horizon 5] [--qty 1000] [--max-inv 10000] [--maker-bps 1] [--csv out.csv]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "../replay/l2_converter.hpp"
#include "market_maker.hpp"
#include "orderbook/formats.hpp"

using namespace ob;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s in.obmd [--latency-us N] [--gamma G] [--k K] [--horizon S] [--qty LOTS]\n"
                     "          [--max-inv LOTS] [--maker-bps B] [--csv out.csv]\n",
                     argv[0]);
        return 2;
    }
    MMParams      p;
    std::uint64_t latencyNs = 200'000;
    std::string   csvPath;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        const char* v = argv[i + 1];
        if (a == "--latency-us") latencyNs = static_cast<std::uint64_t>(std::atof(v) * 1000);
        else if (a == "--gamma") p.gamma = std::atof(v);
        else if (a == "--k") p.k = std::atof(v);
        else if (a == "--horizon") p.horizonSec = std::atof(v);
        else if (a == "--qty") p.quoteLots = std::atoll(v);
        else if (a == "--max-inv") p.maxInventoryLots = std::atoll(v);
        else if (a == "--maker-bps") p.makerFeeBps = std::atof(v);
        else if (a == "--csv") csvPath = v;
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }

    MdFile md = loadMd(argv[1]);
    if (md.records.empty()) return 1;
    std::vector<Price> early;
    for (std::size_t i = 0; i < md.records.size() && early.size() < 10000; ++i)
        if (md.records[i].qty > 0) early.push_back(md.records[i].price);
    std::nth_element(early.begin(), early.begin() + early.size() / 2, early.end());
    const std::uint32_t window = 1u << 20;
    EngineConfig        cfg{early[early.size() / 2] - window / 2, window, 1u << 21};

    auto        book = std::make_unique<OrderBook>(cfg);
    L2Converter conv(*book);
    MarketMaker mm(p, md.header.tickSize, md.header.lotSize);
    static_assert(MarketMaker::kIdBit == L2Converter::kStrategyBit);

    std::deque<std::pair<std::uint64_t, Command>> inFlight;  // (deliver at, cmd)
    std::vector<Command> outbox;
    outbox.reserve(8);

    std::FILE* csv = csvPath.empty() ? nullptr : std::fopen(csvPath.c_str(), "w");
    if (csv) std::fprintf(csv, "t_s,mid,sigma_ticks,bid_px,ask_px,inventory_lots,cash,fees,mtm_pnl\n");

    std::uint64_t now = 0;
    auto onTrade = [&](const Trade& t) {
        if (!L2Converter::isSynthetic(t.maker)) mm.onFill(now, t.maker, t.price, t.qty, true);
        if (!L2Converter::isSynthetic(t.taker)) mm.onFill(now, t.taker, t.price, t.qty, false);
    };

    // Best price on a side ignoring our own quotes.
    auto externalBest = [&](Side s) -> std::optional<Price> {
        std::optional<Price> out;
        book->forEachLevelWhile(s, [&](Price px, Qty total) {
            if (total - mm.ownQtyAt(s, px) > 0) { out = px; return false; }
            return true;
        });
        return out;
    };

    auto deliver = [&](const Command& c) {
        if (c.type == CmdType::New) {
            const auto bb = book->bestBid(), ba = book->bestAsk();
            const bool crosses = c.side == Side::Buy ? (ba && *ba <= c.price) : (bb && *bb >= c.price);
            if (crosses) { mm.onNewAck(c.id, false); return; }  // post-only reject
            const Result r = apply(*book, c, onTrade);
            mm.onNewAck(c.id, r.status == Status::Resting);
        } else {
            apply(*book, c, onTrade);
            mm.onCancelAck(c.id);
        }
    };

    std::uint64_t nextCsv = 0;
    const std::uint64_t t0 = md.records.front().ts;
    for (const MdRecord& r : md.records) {
        now = r.ts;
        while (!inFlight.empty() && inFlight.front().first <= now) {
            deliver(inFlight.front().second);
            inFlight.pop_front();
        }
        conv.onRecord(r, [&](const Command& c) { apply(*book, c, onTrade); });

        outbox.clear();
        mm.onMarket(now, externalBest(Side::Buy), externalBest(Side::Sell), outbox);
        for (const Command& c : outbox) inFlight.emplace_back(now + latencyNs, c);

        if (csv && now >= nextCsv) {
            std::fprintf(csv, "%.3f,%.2f,%.3f,%lld,%lld,%lld,%.6f,%.6f,%.6f\n", (now - t0) / 1e9, mm.mid(), mm.sigma(),
                         (long long)(mm.bid().st == MarketMaker::St::Live ? mm.bid().px : 0),
                         (long long)(mm.ask().st == MarketMaker::St::Live ? mm.ask().px : 0),
                         (long long)mm.inventory(), mm.cash(), mm.fees(), mm.markToMarket());
            nextCsv = now + 1'000'000'000;
        }
    }
    if (csv) std::fclose(csv);

    const auto& s = mm.stats();
    const double hours = (md.records.back().ts - t0) / 3.6e12;
    std::printf("data: %.16s, %zu records, %.2f h, latency %.0f us\n", md.header.symbol, md.records.size(), hours,
                latencyNs / 1e3);
    std::printf("params: gamma=%g k=%g T=%gs qty=%lld lots max_inv=%lld lots maker=%gbps\n", p.gamma, p.k,
                p.horizonSec, (long long)p.quoteLots, (long long)p.maxInventoryLots, p.makerFeeBps);
    std::printf("orders: sent=%llu cancels=%llu post_only_rejects=%llu filled(any)=%llu fill_rate=%.2f%%\n",
                (unsigned long long)s.ordersSent, (unsigned long long)s.cancelsSent,
                (unsigned long long)s.postOnlyRejects, (unsigned long long)s.ordersFilled,
                s.ordersSent ? 100.0 * s.ordersFilled / s.ordersSent : 0.0);
    std::printf("fills:  %llu fills, %lld lots (%.4f units), max |inventory| %lld lots, final %lld lots\n",
                (unsigned long long)s.fills, (long long)s.lotsFilled, s.lotsFilled * md.header.lotSize,
                (long long)s.maxAbsInventory, (long long)mm.inventory());
    std::printf("markout @1s: %.3f ticks/lot (negative = adverse selection)\n",
                s.markoutQty ? s.markoutTicksXQty / s.markoutQty : 0.0);
    const double gross = mm.markToMarket() + mm.fees();
    std::printf("P&L (quote ccy): gross=%.4f fees=%.4f net=%.4f\n", gross, mm.fees(), mm.markToMarket());
    if (csv) std::printf("time series written to %s\n", csvPath.c_str());
    return 0;
}
