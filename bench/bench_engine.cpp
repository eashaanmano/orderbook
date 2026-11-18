// Google Benchmark suite: every data-structure choice vs. its naive alternative.
//
//   Engine vs Reference      flat array + intrusive lists + pool  vs  std::map + std::list
//   FlatHashMap vs std::unordered_map   (order-id index)
//   ObjectPool  vs new/delete           (order storage)
//
// Run:  ./build/bench_engine --benchmark_min_time=0.5s
// Pin and quiet the machine for stable numbers:
//       taskset -c 2 ./build/bench_engine --benchmark_repetitions=5 --benchmark_report_aggregates_only=true

#include <benchmark/benchmark.h>

#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include "orderbook/flat_hash_map.hpp"
#include "orderbook/object_pool.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/reference_book.hpp"

using namespace ob;

namespace {

enum class OpType { Submit, Cancel, Modify };
struct Op {
    OpType   type;
    NewOrder o;  // for Submit; o.id/price/qty reused for Cancel/Modify
};

// A valid, realistic-ish flow: mostly limits near the touch, lots of cancels,
// some marketable orders. Same stream for both engines.
std::vector<Op> makeFlow(std::size_t n, std::uint64_t seed, Price centre, Price spread) {
    std::mt19937_64 rng(seed);
    auto uni = [&](std::int64_t a, std::int64_t b) { return std::uniform_int_distribution<std::int64_t>(a, b)(rng); };
    std::vector<Op>      ops;
    std::vector<OrderId> live;
    ops.reserve(n);
    OrderId next = 1;
    for (std::size_t i = 0; i < n; ++i) {
        const int r = static_cast<int>(uni(0, 99));
        if (r < 55 || live.empty()) {
            const Side  s  = uni(0, 1) ? Side::Buy : Side::Sell;
            const Price px = s == Side::Buy ? centre - uni(0, spread) : centre + uni(0, spread);
            ops.push_back({OpType::Submit, {next, s, OrderType::Limit, px, uni(1, 100)}});
            live.push_back(next++);
        } else if (r < 85) {
            const std::size_t k = static_cast<std::size_t>(uni(0, live.size() - 1));
            ops.push_back({OpType::Cancel, {live[k], Side::Buy, OrderType::Limit, 0, 0}});
            live[k] = live.back();
            live.pop_back();
        } else if (r < 92) {
            const std::size_t k = static_cast<std::size_t>(uni(0, live.size() - 1));
            ops.push_back({OpType::Modify, {live[k], Side::Buy, OrderType::Limit, centre + uni(-spread, spread), uni(1, 100)}});
        } else {
            const Side s = uni(0, 1) ? Side::Buy : Side::Sell;
            const Price px = s == Side::Buy ? centre + uni(0, spread / 4) : centre - uni(0, spread / 4);
            ops.push_back({OpType::Submit, {next++, s, OrderType::IOC, px, uni(1, 300)}});
        }
    }
    return ops;
}

const std::vector<Op>& flow() {
    static const std::vector<Op> f = makeFlow(200'000, 1, 50'000, 50);
    return f;
}

constexpr EngineConfig kCfg{0, 100'000, 1 << 18};

void BM_Engine_MixedFlow(benchmark::State& st) {
    const auto& ops = flow();
    for (auto _ : st) {
        st.PauseTiming();
        auto book = std::make_unique<OrderBook>(kCfg);
        st.ResumeTiming();
        std::uint64_t trades = 0;
        auto sink = [&](const Trade&) { ++trades; };
        for (const Op& op : ops) {
            switch (op.type) {
                case OpType::Submit: book->submit(op.o, sink); break;
                case OpType::Cancel: book->cancel(op.o.id); break;
                case OpType::Modify: book->modify(op.o.id, op.o.price, op.o.qty, sink); break;
            }
        }
        benchmark::DoNotOptimize(trades);
        st.PauseTiming();
        book.reset();
        st.ResumeTiming();
    }
    st.SetItemsProcessed(static_cast<std::int64_t>(st.iterations() * ops.size()));
}
BENCHMARK(BM_Engine_MixedFlow)->Unit(benchmark::kMillisecond);

void BM_Reference_MixedFlow(benchmark::State& st) {
    const auto& ops = flow();
    std::vector<Trade> trades;
    trades.reserve(1024);
    for (auto _ : st) {
        st.PauseTiming();
        auto book = std::make_unique<ReferenceBook>(kCfg.minPrice, kCfg.numLevels);
        st.ResumeTiming();
        for (const Op& op : ops) {
            trades.clear();
            switch (op.type) {
                case OpType::Submit: book->submit(op.o, trades); break;
                case OpType::Cancel: book->cancel(op.o.id); break;
                case OpType::Modify: book->modify(op.o.id, op.o.price, op.o.qty, trades); break;
            }
        }
        benchmark::DoNotOptimize(trades.data());
        st.PauseTiming();
        book.reset();
        st.ResumeTiming();
    }
    st.SetItemsProcessed(static_cast<std::int64_t>(st.iterations() * ops.size()));
}
BENCHMARK(BM_Reference_MixedFlow)->Unit(benchmark::kMillisecond);

// Add + cancel against a deep resting book (no matching): pure data-structure cost.
template <class AddFn, class CancelFn>
void addCancelLoop(benchmark::State& st, AddFn add, CancelFn cancel) {
    std::mt19937_64 rng(3);
    OrderId id = 10'000'000;
    for (auto _ : st) {
        const Price px = 49'000 + static_cast<Price>(rng() % 900);  // bids only, below asks
        add(id, px);
        cancel(id);
        ++id;
    }
    st.SetItemsProcessed(st.iterations() * 2);
}

void BM_Engine_AddCancel_DeepBook(benchmark::State& st) {
    auto book = std::make_unique<OrderBook>(kCfg);
    for (OrderId i = 1; i <= 50'000; ++i) book->submit({i, Side::Buy, OrderType::Limit, 49'000 + Price(i % 1000), 10});
    addCancelLoop(st, [&](OrderId id, Price px) { book->submit({id, Side::Buy, OrderType::Limit, px, 10}); },
                  [&](OrderId id) { book->cancel(id); });
}
BENCHMARK(BM_Engine_AddCancel_DeepBook);

void BM_Reference_AddCancel_DeepBook(benchmark::State& st) {
    auto               book = std::make_unique<ReferenceBook>(kCfg.minPrice, kCfg.numLevels);
    std::vector<Trade> t;
    for (OrderId i = 1; i <= 50'000; ++i) book->submit({i, Side::Buy, OrderType::Limit, 49'000 + Price(i % 1000), 10}, t);
    addCancelLoop(st, [&](OrderId id, Price px) { book->submit({id, Side::Buy, OrderType::Limit, px, 10}, t); },
                  [&](OrderId id) { book->cancel(id); });
}
BENCHMARK(BM_Reference_AddCancel_DeepBook);

// ---- order index: FlatHashMap vs std::unordered_map (insert, find, erase churn)
constexpr std::uint32_t kLive = 100'000;

// Arg 0 = sequential keys (like exchange order ids), Arg 1 = random 64-bit keys.
const std::vector<std::uint64_t>& keyStream(bool random) {
    static const std::vector<std::uint64_t> seq = [] {
        std::vector<std::uint64_t> v(1 << 20);
        for (std::size_t i = 0; i < v.size(); ++i) v[i] = i;
        return v;
    }();
    static const std::vector<std::uint64_t> rnd = [] {
        std::vector<std::uint64_t> v(1 << 20);
        std::mt19937_64 rng(11);
        for (auto& x : v) x = rng() >> 1;
        return v;
    }();
    return random ? rnd : seq;
}

template <class Map, class Insert, class Find, class Erase>
void churn(benchmark::State& st, Map& m, Insert ins, Find fnd, Erase ers) {
    const auto& keys = keyStream(st.range(0) == 1);
    const std::size_t mask = keys.size() - 1;
    for (std::uint64_t k = 0; k < kLive; ++k) ins(m, keys[k]);
    std::uint64_t lo = 0, hi = kLive;
    for (auto _ : st) {
        ins(m, keys[hi++ & mask]);
        benchmark::DoNotOptimize(fnd(m, keys[(lo + (hi * 7919) % kLive) & mask]));
        ers(m, keys[lo++ & mask]);
    }
    st.SetItemsProcessed(st.iterations() * 3);
}

void BM_FlatHashMap_Churn(benchmark::State& st) {
    FlatHashMap m(kLive * 2);
    churn(st, m, [](auto& mm, std::uint64_t k) { mm.insert(k, 1); },
          [](auto& mm, std::uint64_t k) { return mm.find(k) != nullptr; },
          [](auto& mm, std::uint64_t k) { mm.erase(k); });
}
BENCHMARK(BM_FlatHashMap_Churn)->Arg(0)->Arg(1);

void BM_UnorderedMap_Churn(benchmark::State& st) {
    std::unordered_map<std::uint64_t, std::uint32_t> m;
    m.reserve(kLive * 2);
    churn(st, m, [](auto& mm, std::uint64_t k) { mm.emplace(k, 1); },
          [](auto& mm, std::uint64_t k) { return mm.find(k) != mm.end(); },
          [](auto& mm, std::uint64_t k) { mm.erase(k); });
}
BENCHMARK(BM_UnorderedMap_Churn)->Arg(0)->Arg(1);

// ---- order storage: pool vs heap
struct Obj { std::uint64_t a, b, c, d; };

void BM_ObjectPool_AllocFree(benchmark::State& st) {
    ObjectPool<Obj> pool(1 << 16);
    std::vector<std::uint32_t> held;
    held.reserve(1024);
    for (auto _ : st) {
        for (int i = 0; i < 1024; ++i) held.push_back(pool.alloc());
        for (auto h : held) pool.release(h);
        held.clear();
    }
    st.SetItemsProcessed(st.iterations() * 2048);
}
BENCHMARK(BM_ObjectPool_AllocFree);

void BM_NewDelete_AllocFree(benchmark::State& st) {
    std::vector<Obj*> held;
    held.reserve(1024);
    for (auto _ : st) {
        for (int i = 0; i < 1024; ++i) {
            held.push_back(new Obj{});
            benchmark::DoNotOptimize(held.back());
        }
        for (auto* h : held) delete h;
        held.clear();
    }
    st.SetItemsProcessed(st.iterations() * 2048);
}
BENCHMARK(BM_NewDelete_AllocFree);

}  // namespace

BENCHMARK_MAIN();
