// orderbook_gen: writes a SYNTHETIC .obmd file in the same format the Binance
// recorder produces, so the whole chain (convert -> replay -> pipeline ->
// backtest) runs offline and in CI. It is a toy random-walk market, not a
// model of BTCUSDT: use it to exercise the code, never to quote numbers from.
//
// Usage: orderbook_gen out.obmd [records=2000000] [seed=42]

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>

#include "orderbook/formats.hpp"

using namespace ob;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s out.obmd [records=2000000] [seed=42]\n", argv[0]);
        return 2;
    }
    const std::uint64_t N    = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2'000'000;
    const std::uint64_t seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 42;

    MdWriter        out(argv[1], 0.01, 0.00001, "SYNTH-BTCUSDT");
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    std::exponential_distribution<double>  interArrivalMs(1.0 / 2.0);  // mean 2 ms between events
    auto qtyDraw = [&] { return static_cast<Qty>(std::exp(std::normal_distribution<double>(7.5, 1.2)(rng))) + 1; };

    constexpr int D = 200;      // levels kept on each side
    Price m = 6'700'000;        // bids <= m, asks >= m + 1   (67000.00 USDT at tick 0.01)
    std::map<Price, Qty> bids, asks;
    std::uint64_t ts = 1'700'000'000'000'000'000ull, written = 0;

    auto emit = [&](MdKind k, Side s, Price px, Qty q) {
        MdRecord r{};
        r.ts = ts; r.kind = k; r.side = s; r.price = px; r.qty = q;
        out.write(r);
        ++written;
    };

    for (int i = 0; i < D; ++i) {
        bids[m - i]     = qtyDraw();
        asks[m + 1 + i] = qtyDraw();
        emit(MdKind::Level, Side::Buy, m - i, bids[m - i]);
        emit(MdKind::Level, Side::Sell, m + 1 + i, asks[m + 1 + i]);
    }

    while (written < N) {
        ts += static_cast<std::uint64_t>(interArrivalMs(rng) * 1e6) + 1;
        const double u = U(rng);
        if (u < 0.10) {
            // Trade: aggressor takes (part of) the best opposite level, then the level update follows.
            const bool buyAgg = U(rng) < 0.5;
            auto&      book   = buyAgg ? asks : bids;
            if (book.empty()) continue;
            auto       it = buyAgg ? book.begin() : std::prev(book.end());
            const Qty  q  = std::min<Qty>(it->second, std::max<Qty>(1, qtyDraw() / 2));
            emit(MdKind::Trade, buyAgg ? Side::Buy : Side::Sell, it->first, q);
            it->second -= q;
            emit(MdKind::Level, buyAgg ? Side::Sell : Side::Buy, it->first, it->second);
            if (it->second == 0) book.erase(it);
        } else if (u < 0.13) {
            // Mid moves one tick. Remove the level that would otherwise cross first.
            if (U(rng) < 0.5) {
                ++m;
                if (asks.erase(m)) emit(MdKind::Level, Side::Sell, m, 0);
            } else {
                if (bids.erase(m)) emit(MdKind::Level, Side::Buy, m, 0);
                --m;
            }
            // Trim levels that drifted too far from the mid.
            while (!bids.empty() && bids.begin()->first < m - D) {
                emit(MdKind::Level, Side::Buy, bids.begin()->first, 0);
                bids.erase(bids.begin());
            }
            while (!asks.empty() && std::prev(asks.end())->first > m + 1 + D) {
                emit(MdKind::Level, Side::Sell, std::prev(asks.end())->first, 0);
                asks.erase(std::prev(asks.end()));
            }
        } else {
            // Level update, concentrated near the touch.
            const bool  buy = U(rng) < 0.5;
            const int   off = std::min(D - 1, static_cast<int>(std::exponential_distribution<double>(1.0 / 12.0)(rng)));
            const Price px  = buy ? m - off : m + 1 + off;
            const Qty   q   = U(rng) < 0.2 ? 0 : qtyDraw();
            auto&       book = buy ? bids : asks;
            if (q == 0) book.erase(px);
            else book[px] = q;
            emit(MdKind::Level, buy ? Side::Buy : Side::Sell, px, q);
        }
    }
    std::printf("wrote %llu records to %s (synthetic, seed %llu)\n", (unsigned long long)written, argv[1],
                (unsigned long long)seed);
    return 0;
}
