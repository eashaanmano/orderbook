// orderbook_pipeline: the threaded architecture.
//
//   gateway thread --SPSC--> matching engine thread --SPSC--> market-data publisher thread
//
// * gateway:   reads commands from memory, stamps a send time, pushes into ring 1
//              (optionally paced to --rate msgs/s; unpaced = max-throughput test)
// * engine:    the ONLY thread that touches the OrderBook. Pops commands in order,
//              emits Trade events, a top-of-book (BBO) event when it changes, and an
//              Ack per command, into ring 2.
// * publisher: consumes market data, measures gateway->ack latency per command,
//              and digests the trade stream.
//
// The engine sees commands in exactly file order, so the trade digest equals the
// single-threaded orderbook_replay digest: threading added no nondeterminism.
//
// Unpaced latency numbers mostly measure queueing (the gateway outruns the engine
// and the ring fills); use --rate to measure latency under a realistic load.
//
// Usage: orderbook_pipeline in.obcmd [--rate N] [--cores g,e,p] [--max-orders N]

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "orderbook/apply.hpp"
#include "orderbook/digest.hpp"
#include "orderbook/latency_histogram.hpp"
#include "orderbook/spsc_ring.hpp"
#include "orderbook/sys.hpp"

using namespace ob;

namespace {

struct InMsg {
    Command       cmd;
    std::uint64_t sentNs;
    std::uint8_t  shutdown;
};

enum class MdType : std::uint8_t { Trade, Bbo, Ack, Shutdown };

struct MdMsg {
    std::uint64_t sentNs;   // gateway timestamp of the command that caused this
    OrderId       taker, maker;
    Price         price;    // trade px, or best bid for Bbo
    Qty           qty;      // trade qty
    Price         price2;   // best ask for Bbo
    MdType        type;
    Side          side;
    Status        status;
};

constexpr std::size_t kRing = 1 << 16;
using InRing = SpscRing<InMsg, kRing>;
using MdRing = SpscRing<MdMsg, kRing>;

template <class Ring, class T>
inline void pushSpin(Ring& r, const T& v) {
    while (!r.tryPush(v)) cpuRelax();
}

constexpr Price kNone = std::numeric_limits<Price>::min();

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s in.obcmd [--rate N] [--cores g,e,p] [--max-orders N]\n", argv[0]);
        return 2;
    }
    double        rate      = 0;
    int           cores[3]  = {-1, -1, -1};
    std::uint32_t maxOrders = 1u << 21;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (a == "--max-orders" && i + 1 < argc) maxOrders = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--cores" && i + 1 < argc) {
            auto v = parseIntList(argv[++i]);
            for (std::size_t k = 0; k < 3 && k < v.size(); ++k) cores[k] = v[k];
        }
    }
    if (std::thread::hardware_concurrency() < 3)
        std::printf("note: %u hardware threads; 3 busy-spinning threads will share cores, expect noisy latency\n",
                    std::thread::hardware_concurrency());

    const CmdFile f = loadCmds(argv[1]);
    const EngineConfig cfg{f.header.minPrice, f.header.numLevels, maxOrders};
    auto book   = std::make_unique<OrderBook>(cfg);
    auto inRing = std::make_unique<InRing>();
    auto mdRing = std::make_unique<MdRing>();

    std::atomic<int> ready{0};
    auto waitAllReady = [&] {
        ready.fetch_add(1);
        while (ready.load() < 3) cpuRelax();
    };

    // ------------------------------------------------------------ publisher
    TradeDigest      digest;
    LatencyHistogram latency;
    std::uint64_t    bboUpdates = 0, acks = 0, rejects = 0;
    std::thread publisher([&] {
        pinThisThread(cores[2]);
        waitAllReady();
        MdMsg m;
        for (;;) {
            if (!mdRing->tryPop(m)) {
                cpuRelax();
                continue;
            }
            if (m.type == MdType::Shutdown) break;
            switch (m.type) {
                case MdType::Trade: digest.add(Trade{m.taker, m.maker, m.side, m.price, m.qty}); break;
                case MdType::Bbo: ++bboUpdates; break;
                case MdType::Ack:
                    ++acks;
                    if (m.status == Status::Rejected) ++rejects;
                    latency.record(nowNs() - m.sentNs);
                    break;
                default: break;
            }
        }
    });

    // --------------------------------------------------------------- engine
    std::thread engine([&] {
        pinThisThread(cores[1]);
        waitAllReady();
        InMsg in;
        Price lastBid = kNone, lastAsk = kNone;
        for (;;) {
            if (!inRing->tryPop(in)) {
                cpuRelax();
                continue;
            }
            if (in.shutdown) {
                MdMsg s{};
                s.type = MdType::Shutdown;
                pushSpin(*mdRing, s);
                break;
            }
            const std::uint64_t sent = in.sentNs;
            const Result r = apply(*book, in.cmd, [&](const Trade& t) {
                MdMsg m{};
                m.type = MdType::Trade; m.sentNs = sent; m.taker = t.taker; m.maker = t.maker;
                m.price = t.price; m.qty = t.qty; m.side = t.takerSide;
                pushSpin(*mdRing, m);
            });
            const Price bid = book->bestBid().value_or(kNone);
            const Price ask = book->bestAsk().value_or(kNone);
            if (bid != lastBid || ask != lastAsk) {
                MdMsg m{};
                m.type = MdType::Bbo; m.sentNs = sent; m.price = bid; m.price2 = ask;
                pushSpin(*mdRing, m);
                lastBid = bid;
                lastAsk = ask;
            }
            MdMsg ack{};
            ack.type = MdType::Ack; ack.sentNs = sent; ack.status = r.status; ack.taker = in.cmd.id;
            pushSpin(*mdRing, ack);
        }
    });

    // -------------------------------------------------------------- gateway
    pinThisThread(cores[0]);
    waitAllReady();
    const std::uint64_t t0 = nowNs();
    for (std::size_t i = 0; i < f.cmds.size(); ++i) {
        if (rate > 0) {
            const auto due = t0 + static_cast<std::uint64_t>(i * 1e9 / rate);
            while (nowNs() < due) cpuRelax();
        }
        InMsg m{};
        m.cmd    = f.cmds[i];
        m.sentNs = nowNs();
        pushSpin(*inRing, m);
    }
    InMsg stop{};
    stop.shutdown = 1;
    pushSpin(*inRing, stop);
    engine.join();
    publisher.join();
    const std::uint64_t dt = nowNs() - t0;

    std::printf("pipeline: %zu cmds in %.3f s = %.2f M cmds/s%s\n", f.cmds.size(), dt / 1e9,
                f.cmds.size() / (dt / 1e3), rate > 0 ? " (paced)" : " (unpaced)");
    std::printf("acks=%llu rejects=%llu bbo_updates=%llu trades=%llu digest=%016llx\n", (unsigned long long)acks,
                (unsigned long long)rejects, (unsigned long long)bboUpdates, (unsigned long long)digest.count(),
                (unsigned long long)digest.value());
    latency.print("gateway->ack latency");
    return 0;
}
