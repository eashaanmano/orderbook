// Hand-written scenario tests. Dependency free: a tiny CHECK macro and a test registry.
// Each test pins down one rule from the spec in order_book.hpp.

#include <atomic>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../replay/l2_converter.hpp"
#include "orderbook/flat_hash_map.hpp"
#include "orderbook/latency_histogram.hpp"
#include "orderbook/level_bitmap.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/spsc_ring.hpp"

using namespace ob;

namespace {

int g_failures = 0;
std::vector<std::pair<const char*, std::function<void()>>>& registry() {
    static std::vector<std::pair<const char*, std::function<void()>>> r;
    return r;
}
struct Registrar {
    Registrar(const char* n, std::function<void()> f) { registry().emplace_back(n, std::move(f)); }
};

#define TEST(name)                                       \
    static void name();                                  \
    static Registrar reg_##name(#name, name);            \
    static void name()

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("    CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        auto _va = (a);                                                                                  \
        auto _vb = (b);                                                                                  \
        if (!(_va == _vb)) {                                                                             \
            std::printf("    CHECK_EQ failed %s:%d: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a,    \
                        (long long)_va, #b, (long long)_vb);                                             \
            ++g_failures;                                                                                \
        }                                                                                                \
    } while (0)

EngineConfig smallCfg() { return EngineConfig{0, 1024, 1024}; }

struct Collect {
    std::vector<Trade> trades;
    auto sink() {
        return [this](const Trade& t) { trades.push_back(t); };
    }
};

NewOrder lim(OrderId id, Side s, Price p, Qty q) { return {id, s, OrderType::Limit, p, q}; }

}  // namespace

// ------------------------------------------------------------------- matching

TEST(example_from_readme) {
    // Sell 5 @100, sell 3 @101; buyer wants 6 up to 101 -> 5 @100 then 1 @101.
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Sell, 100, 5));
    b.submit(lim(2, Side::Sell, 101, 3));
    Collect c;
    Result  r = b.submit(lim(3, Side::Buy, 101, 6), c.sink());
    CHECK(r.status == Status::Filled);
    CHECK_EQ(r.filled, 6);
    CHECK_EQ(c.trades.size(), 2u);
    CHECK(c.trades[0] == (Trade{3, 1, Side::Buy, 100, 5}));
    CHECK(c.trades[1] == (Trade{3, 2, Side::Buy, 101, 1}));
    CHECK_EQ(b.levelQty(Side::Sell, 101), 2);
    CHECK(b.bestAsk() == Price{101});
    CHECK(!b.bestBid().has_value());
}

TEST(time_priority_at_same_price) {
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Buy, 50, 4));
    b.submit(lim(2, Side::Buy, 50, 4));
    b.submit(lim(3, Side::Buy, 50, 4));
    Collect c;
    b.submit(lim(9, Side::Sell, 50, 6), c.sink());
    CHECK_EQ(c.trades.size(), 2u);
    CHECK_EQ(c.trades[0].maker, 1u);
    CHECK_EQ(c.trades[0].qty, 4);
    CHECK_EQ(c.trades[1].maker, 2u);
    CHECK_EQ(c.trades[1].qty, 2);
    CHECK_EQ(b.orderQty(2), 2);
    CHECK_EQ(b.orderQty(3), 4);
}

TEST(trade_happens_at_maker_price) {
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Sell, 100, 1));
    Collect c;
    b.submit(lim(2, Side::Buy, 110, 1), c.sink());
    CHECK_EQ(c.trades.at(0).price, 100);
}

TEST(limit_rests_remainder) {
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Sell, 100, 2));
    Result r = b.submit(lim(2, Side::Buy, 100, 5));
    CHECK(r.status == Status::Resting);
    CHECK_EQ(r.filled, 2);
    CHECK_EQ(r.resting, 3);
    CHECK(b.bestBid() == Price{100});
    CHECK(!b.bestAsk().has_value());
}

TEST(market_sweeps_and_cancels_rest) {
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Sell, 100, 2));
    b.submit(lim(2, Side::Sell, 900, 2));
    Collect c;
    Result  r = b.submit(NewOrder{3, Side::Buy, OrderType::Market, 0, 10}, c.sink());
    CHECK(r.status == Status::Cancelled);
    CHECK_EQ(r.filled, 4);
    CHECK_EQ(c.trades.size(), 2u);
    CHECK_EQ(b.orderCount(), 0u);
    CHECK(!b.contains(3));
}

TEST(market_on_empty_book) {
    OrderBook b(smallCfg());
    Result    r = b.submit(NewOrder{1, Side::Sell, OrderType::Market, 0, 10});
    CHECK(r.status == Status::Cancelled);
    CHECK_EQ(r.filled, 0);
}

TEST(ioc_partial) {
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Sell, 100, 3));
    b.submit(lim(2, Side::Sell, 102, 3));
    Result r = b.submit(NewOrder{3, Side::Buy, OrderType::IOC, 101, 5});
    CHECK(r.status == Status::Cancelled);
    CHECK_EQ(r.filled, 3);
    CHECK(!b.contains(3));
    CHECK_EQ(b.levelQty(Side::Sell, 102), 3);
}

TEST(fok_all_or_nothing) {
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Sell, 100, 3));
    b.submit(lim(2, Side::Sell, 101, 3));
    Collect c;
    Result  r = b.submit(NewOrder{3, Side::Buy, OrderType::FOK, 100, 4}, c.sink());
    CHECK(r.status == Status::Rejected);
    CHECK(r.reject == Reject::FokUnfillable);
    CHECK(c.trades.empty());
    CHECK_EQ(b.levelQty(Side::Sell, 100), 3);
    r = b.submit(NewOrder{4, Side::Buy, OrderType::FOK, 101, 6}, c.sink());
    CHECK(r.status == Status::Filled);
    CHECK_EQ(c.trades.size(), 2u);
    CHECK_EQ(b.orderCount(), 0u);
}

// ------------------------------------------------------------- cancel / modify

TEST(cancel_and_best_price_update) {
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Buy, 100, 1));
    b.submit(lim(2, Side::Buy, 90, 1));
    Result r = b.cancel(1);
    CHECK(r.status == Status::Cancelled);
    CHECK_EQ(r.resting, 1);
    CHECK(b.bestBid() == Price{90});
    CHECK(b.cancel(1).reject == Reject::UnknownId);
    b.cancel(2);
    CHECK(!b.bestBid().has_value());
}

TEST(modify_reduce_keeps_priority) {
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Sell, 100, 5));
    b.submit(lim(2, Side::Sell, 100, 5));
    Result r = b.modify(1, 100, 2);
    CHECK(r.status == Status::Resting);
    CHECK_EQ(b.levelQty(Side::Sell, 100), 7);
    Collect c;
    b.submit(lim(3, Side::Buy, 100, 1), c.sink());
    CHECK_EQ(c.trades.at(0).maker, 1u);  // still first in line
}

TEST(modify_increase_loses_priority) {
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Sell, 100, 5));
    b.submit(lim(2, Side::Sell, 100, 5));
    b.modify(1, 100, 6);
    Collect c;
    b.submit(lim(3, Side::Buy, 100, 1), c.sink());
    CHECK_EQ(c.trades.at(0).maker, 2u);  // order 1 went to the back
}

TEST(modify_reprice_can_cross) {
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Sell, 100, 5));
    b.submit(lim(2, Side::Buy, 95, 5));
    Collect c;
    Result  r = b.modify(2, 100, 5, c.sink());
    CHECK(r.status == Status::Filled);
    CHECK_EQ(c.trades.size(), 1u);
    CHECK_EQ(b.orderCount(), 0u);
}

TEST(modify_bad_price_leaves_order_untouched) {
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Buy, 100, 5));
    CHECK(b.modify(1, 5000, 5).reject == Reject::BadPrice);
    CHECK(b.modify(1, 100, 0).reject == Reject::BadQty);
    CHECK(b.modify(77, 100, 1).reject == Reject::UnknownId);
    CHECK_EQ(b.orderQty(1), 5);
}

// -------------------------------------------------------------------- rejects

TEST(validation_rejects) {
    OrderBook b(smallCfg());
    CHECK(b.submit(lim(1, Side::Buy, 100, 0)).reject == Reject::BadQty);
    CHECK(b.submit(lim(1, Side::Buy, -1, 1)).reject == Reject::BadPrice);
    CHECK(b.submit(lim(1, Side::Buy, 1024, 1)).reject == Reject::BadPrice);
    CHECK(b.submit(lim(kInvalidOrderId, Side::Buy, 10, 1)).reject == Reject::DuplicateId);
    CHECK(b.submit(lim(1, Side::Buy, 1023, 1)).status == Status::Resting);  // window edge ok
    CHECK(b.submit(lim(1, Side::Buy, 10, 1)).reject == Reject::DuplicateId);
}

TEST(capacity_exhaustion) {
    OrderBook b(EngineConfig{0, 64, 4});
    for (OrderId i = 1; i <= 4; ++i) CHECK(b.submit(lim(i, Side::Buy, 10, 1)).status == Status::Resting);
    CHECK(b.submit(lim(5, Side::Buy, 10, 1)).reject == Reject::CapacityExhausted);
    b.cancel(1);
    CHECK(b.submit(lim(5, Side::Buy, 10, 1)).status == Status::Resting);
}

TEST(id_reusable_after_fill) {
    OrderBook b(smallCfg());
    b.submit(lim(1, Side::Buy, 10, 1));
    b.submit(lim(2, Side::Sell, 10, 1));
    CHECK(b.submit(lim(1, Side::Buy, 10, 1)).status == Status::Resting);
}

// ------------------------------------------------------------ building blocks

TEST(level_bitmap_scans) {
    LevelBitmap bm(200);
    CHECK_EQ(bm.highestAtOrBelow(199), -1);
    CHECK_EQ(bm.lowestAtOrAbove(0), -1);
    bm.set(0);
    bm.set(63);
    bm.set(64);
    bm.set(199);
    CHECK_EQ(bm.highestAtOrBelow(198), 64);
    CHECK_EQ(bm.highestAtOrBelow(63), 63);
    CHECK_EQ(bm.highestAtOrBelow(62), 0);
    CHECK_EQ(bm.lowestAtOrAbove(1), 63);
    CHECK_EQ(bm.lowestAtOrAbove(65), 199);
    CHECK_EQ(bm.lowestAtOrAbove(200), -1);
    CHECK_EQ(bm.highestAtOrBelow(-1), -1);
    bm.clear(199);
    CHECK_EQ(bm.lowestAtOrAbove(65), -1);
}

TEST(flat_hash_map_matches_unordered_map) {
    FlatHashMap                                  m(5000);
    std::unordered_map<std::uint64_t, std::uint32_t> ref;
    std::mt19937_64                              rng(7);
    for (int i = 0; i < 200000; ++i) {
        std::uint64_t k = rng() % 8000;  // small key space -> lots of collisions / churn
        int           op = static_cast<int>(rng() % 3);
        if (op == 0 && ref.size() < 5000) {
            bool ins = m.insert(k, static_cast<std::uint32_t>(i));
            bool rin = ref.emplace(k, static_cast<std::uint32_t>(i)).second;
            CHECK(ins == rin);
        } else if (op == 1) {
            CHECK(m.erase(k) == (ref.erase(k) == 1));
        } else {
            auto* v = m.find(k);
            auto  it = ref.find(k);
            CHECK((v != nullptr) == (it != ref.end()));
            if (v && it != ref.end()) CHECK(*v == it->second);
        }
    }
    CHECK_EQ(m.size(), ref.size());
}

TEST(spsc_ring_two_threads_in_order) {
    static SpscRing<std::uint64_t, 1024> ring;  // static: large object
    constexpr std::uint64_t N = 2'000'000;
    std::atomic<bool>       ok{true};
    std::thread consumer([&] {
        std::uint64_t expect = 0, v;
        while (expect < N) {
            if (ring.tryPop(v)) {
                if (v != expect) ok = false;
                ++expect;
            }
        }
    });
    for (std::uint64_t i = 0; i < N;) {
        if (ring.tryPush(i)) ++i;
    }
    consumer.join();
    CHECK(ok.load());
}

TEST(latency_histogram_percentiles) {
    LatencyHistogram h;
    for (std::uint64_t v = 1; v <= 100000; ++v) h.record(v);
    auto within = [](std::uint64_t got, double want) { return got >= want * 0.99 && got <= want * 1.01; };
    CHECK(within(h.percentile(50), 50000));
    CHECK(within(h.percentile(99), 99000));
    CHECK_EQ(h.max(), 100000u);
    CHECK_EQ(h.min(), 1u);
}

// ------------------------------------------------------------------ converter

TEST(l2_converter_reconciles_levels) {
    OrderBook   b(EngineConfig{0, 1024, 1024});
    L2Converter conv(b);
    auto emit = [&](const Command& c) { apply(b, c, [](const Trade&) {}); };
    auto lvl  = [](Side s, Price p, Qty q) { MdRecord r{}; r.kind = MdKind::Level; r.side = s; r.price = p; r.qty = q; return r; };
    auto trd  = [](Side s, Price p, Qty q) { MdRecord r{}; r.kind = MdKind::Trade; r.side = s; r.price = p; r.qty = q; return r; };

    conv.onRecord(lvl(Side::Buy, 100, 10), emit);
    CHECK_EQ(b.levelQty(Side::Buy, 100), 10);
    conv.onRecord(lvl(Side::Buy, 100, 15), emit);  // +5 -> second order
    CHECK_EQ(b.levelQty(Side::Buy, 100), 15);
    conv.onRecord(lvl(Side::Buy, 100, 7), emit);   // -8 -> cancel back order (5), reduce front by 3
    CHECK_EQ(b.levelQty(Side::Buy, 100), 7);
    CHECK_EQ(b.orderCount(), 1u);
    conv.onRecord(trd(Side::Sell, 100, 3), emit);  // seller hits the bid
    CHECK_EQ(b.levelQty(Side::Buy, 100), 4);
    conv.onRecord(lvl(Side::Buy, 100, 4), emit);   // feed confirms: nothing to do
    CHECK_EQ(b.orderCount(), 1u);
    conv.onRecord(lvl(Side::Buy, 100, 0), emit);
    CHECK_EQ(b.orderCount(), 0u);

    // Strategy orders are invisible to reconciliation.
    b.submit(lim(L2Converter::kStrategyBit | 1, Side::Sell, 200, 9));
    conv.onRecord(lvl(Side::Sell, 200, 4), emit);
    CHECK_EQ(b.levelQty(Side::Sell, 200), 13);
    conv.onRecord(lvl(Side::Sell, 200, 0), emit);
    CHECK_EQ(b.levelQty(Side::Sell, 200), 9);
}

int main() {
    for (auto& [name, fn] : registry()) {
        const int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " ok " : "FAIL", name);
    }
    std::printf("%zu tests, %d failed checks\n", registry().size(), g_failures);
    return g_failures == 0 ? 0 : 1;
}
