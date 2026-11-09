// Differential fuzzer: the fast engine vs the reference engine.
//
// Random order streams (limit / market / IOC / FOK / cancel / modify, plus
// deliberately invalid input) go through both engines. After EVERY operation
// the Result and the emitted trade sequence must be identical; every
// `snapshotEvery` operations the full book (every level, every order in queue
// order) must be identical too, and the fast engine's internal invariants
// (level totals, uncrossed book) are checked.
//
// Three book shapes rotate by seed:
//   0: dense   - narrow price band, constant matching
//   1: edges   - tiny window (130 levels, not a multiple of 64) so bitmap word
//                boundaries and window edges are hit all the time
//   2: sparse  - 65536 levels, prices spread wide, long bitmap scans
//
// Usage: fuzz_diff [seeds=60] [opsPerSeed=50000] [firstSeed=1]
// On failure it prints the seed and op index; rerun with that seed to reproduce.

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "orderbook/order_book.hpp"
#include "orderbook/reference_book.hpp"

using namespace ob;

namespace {

struct Shape {
    const char*   name;
    Price         minPrice;
    std::uint32_t numLevels;
    Price         lo, hi;  // where most prices are drawn
    Qty           maxQty;
};

constexpr Shape kShapes[] = {
    {"dense", 0, 1024, 490, 534, 20},
    {"edges", 1000, 130, 1000, 1129, 12},
    {"sparse", 0, 65536, 0, 65535, 50},
};

using LevelSnap = std::tuple<int, Price, Qty, std::vector<std::pair<OrderId, Qty>>>;

template <class Book>
std::vector<LevelSnap> snapshot(const Book& b) {
    std::vector<LevelSnap> out;
    for (Side s : {Side::Buy, Side::Sell}) {
        b.forEachLevel(s, [&](Price px, Qty total) {
            std::vector<std::pair<OrderId, Qty>> q;
            b.forEachOrderAtLevel(s, px, [&](OrderId id, Qty qty) { q.emplace_back(id, qty); });
            out.emplace_back(static_cast<int>(s), px, total, std::move(q));
        });
    }
    return out;
}

bool checkInvariants(const OrderBook& b, std::string& why) {
    std::uint64_t orders = 0;
    for (Side s : {Side::Buy, Side::Sell}) {
        bool ok = true;
        b.forEachLevel(s, [&](Price px, Qty total) {
            Qty sum = 0;
            b.forEachOrderAtLevel(s, px, [&](OrderId, Qty q) {
                sum += q;
                ++orders;
                if (q <= 0) ok = false;
            });
            if (sum != total || total <= 0) ok = false;
        });
        if (!ok) { why = "level total != sum of orders (or non-positive)"; return false; }
    }
    if (orders != b.orderCount()) { why = "orderCount mismatch with index"; return false; }
    auto bb = b.bestBid(), ba = b.bestAsk();
    if (bb && ba && *bb >= *ba) { why = "crossed book"; return false; }
    return true;
}

void printTrades(const char* who, const std::vector<Trade>& t) {
    std::printf("  %s trades (%zu):\n", who, t.size());
    for (auto& x : t)
        std::printf("    taker=%llu maker=%llu px=%lld qty=%lld\n", (unsigned long long)x.taker,
                    (unsigned long long)x.maker, (long long)x.price, (long long)x.qty);
}

bool runSeed(std::uint64_t seed, int ops, int snapshotEvery, std::uint64_t& tradesSeen) {
    const Shape& sh = kShapes[seed % 3];
    std::mt19937_64 rng(seed);
    auto uni = [&](std::int64_t a, std::int64_t b) {
        return std::uniform_int_distribution<std::int64_t>(a, b)(rng);
    };
    auto chance = [&](double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; };

    OrderBook     eng(EngineConfig{sh.minPrice, sh.numLevels, static_cast<std::uint32_t>(ops + 16)});
    ReferenceBook ref(sh.minPrice, sh.numLevels);

    std::vector<OrderId> ids;
    std::unordered_map<OrderId, Price> lastPrice;  // price of each order when it last rested
    OrderId              nextId = 1;
    std::vector<Trade>   te, tr;
    te.reserve(4096);
    tr.reserve(4096);

    auto drawPrice = [&]() -> Price {
        if (chance(0.01)) return chance(0.5) ? sh.minPrice - 1 : sh.minPrice + sh.numLevels;  // out of window
        return uni(sh.lo, sh.hi);
    };
    auto drawQty = [&]() -> Qty {
        if (chance(0.01)) return uni(-2, 0);
        return uni(1, sh.maxQty);
    };
    auto drawNewId = [&]() -> OrderId {
        if (!ids.empty() && chance(0.02)) return ids[uni(0, ids.size() - 1)];  // maybe duplicate
        if (chance(0.001)) return kInvalidOrderId;
        ids.push_back(nextId);
        return nextId++;
    };
    auto drawExistingId = [&]() -> OrderId {
        if (ids.empty() || chance(0.15)) return nextId + 1000 + uni(0, 1000);  // unknown
        const std::int64_t lo = std::max<std::int64_t>(0, static_cast<std::int64_t>(ids.size()) - 3000);
        return ids[uni(lo, ids.size() - 1)];
    };

    for (int i = 0; i < ops; ++i) {
        te.clear();
        tr.clear();
        Result      re, rr;
        const char* opName = "";
        const int   r      = static_cast<int>(uni(0, 99));
        if (r < 70) {
            OrderType t;
            if (r < 45) t = OrderType::Limit, opName = "limit";
            else if (r < 53) t = OrderType::Market, opName = "market";
            else if (r < 61) t = OrderType::IOC, opName = "ioc";
            else t = OrderType::FOK, opName = "fok";
            NewOrder o{drawNewId(), chance(0.5) ? Side::Buy : Side::Sell, t, drawPrice(), drawQty()};
            re = eng.submit(o, [&](const Trade& x) { te.push_back(x); });
            rr = ref.submit(o, tr);
            if (re.status == Status::Resting) lastPrice[o.id] = o.price;
        } else if (r < 88) {
            opName   = "cancel";
            OrderId id = drawExistingId();
            re = eng.cancel(id);
            rr = ref.cancel(id);
        } else {
            opName     = "modify";
            OrderId id = drawExistingId();
            Price   px;
            Qty     q;
            const Qty cur = eng.orderQty(id);
            const int kind = static_cast<int>(uni(0, 3));
            if (kind == 0 && cur > 1 && lastPrice.count(id)) {  // reduce in place
                px = lastPrice[id];
                q  = uni(1, cur - 1);
            } else {                       // reprice / increase / random
                px = drawPrice();
                q  = drawQty();
            }
            re = eng.modify(id, px, q, [&](const Trade& x) { te.push_back(x); });
            rr = ref.modify(id, px, q, tr);
            if (re.status != Status::Rejected) lastPrice[id] = px;
        }

        if (!(re == rr) || te != tr) {
            std::printf("MISMATCH seed=%llu shape=%s op#%d (%s)\n", (unsigned long long)seed, sh.name, i, opName);
            std::printf("  engine:    %s/%s filled=%lld resting=%lld\n", toString(re.status), toString(re.reject),
                        (long long)re.filled, (long long)re.resting);
            std::printf("  reference: %s/%s filled=%lld resting=%lld\n", toString(rr.status), toString(rr.reject),
                        (long long)rr.filled, (long long)rr.resting);
            printTrades("engine", te);
            printTrades("reference", tr);
            return false;
        }
        tradesSeen += te.size();

        if (i % snapshotEvery == 0 || i == ops - 1) {
            std::string why;
            if (!checkInvariants(eng, why)) {
                std::printf("INVARIANT seed=%llu op#%d: %s\n", (unsigned long long)seed, i, why.c_str());
                return false;
            }
            if (snapshot(eng) != snapshot(ref) || eng.orderCount() != ref.orderCount() ||
                eng.bestBid() != ref.bestBid() || eng.bestAsk() != ref.bestAsk()) {
                std::printf("SNAPSHOT MISMATCH seed=%llu shape=%s op#%d\n", (unsigned long long)seed, sh.name, i);
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const int           seeds     = argc > 1 ? std::atoi(argv[1]) : 60;
    const int           ops       = argc > 2 ? std::atoi(argv[2]) : 50000;
    const std::uint64_t firstSeed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1;
    std::uint64_t       trades    = 0;
    for (int s = 0; s < seeds; ++s) {
        if (!runSeed(firstSeed + s, ops, 500, trades)) return 1;
    }
    std::printf("fuzz_diff OK: %d seeds x %d ops = %llu ops, %llu trades, all identical\n", seeds, ops,
                (unsigned long long)seeds * ops, (unsigned long long)trades);
    return 0;
}
