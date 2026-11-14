#pragma once
// Turns aggregated L2 market data into synthetic individual orders.
//
// THE HONEST CAVEAT: Binance's public depth feed publishes aggregate quantity
// per price level, not individual orders. Nobody outside the exchange knows the
// real queue. So we invent orders that reproduce the published levels:
//
//   Level update (side, px, target qty)
//     target > our synthetic qty at px  -> add one Limit order for the difference
//                                          (joins the BACK of the queue)
//     target < our synthetic qty at px  -> remove the difference from the BACK of
//                                          the queue (cancel whole orders, then
//                                          modify-down the last partial one, which
//                                          keeps its priority)
//   Trade (aggressor side, px, qty)
//     -> an IOC at px for qty on the aggressor side. It consumes the front of the
//        queue, exactly like the real trade did. The depth update that follows
//        then finds nothing (or little) left to reconcile.
//
// Assumptions this bakes in (state them in an interview):
//   * cancels come from the back of the queue (LIFO); in reality they can come from anywhere
//   * the depth feed is 100 ms snapshots of change, so several real orders get merged
//     into one synthetic order
//   * trades and depth updates are interleaved by arrival, not perfectly causally
//
// Reconciliation is always against the engine's ACTUAL state, so the book cannot
// drift from the feed: if a synthetic IOC filled a strategy order instead of a
// synthetic one, the next depth update cancels the now-excess synthetic qty.
// Orders whose id has kStrategyBit set belong to someone else (the market maker)
// and are never touched or counted.
//
// The converter only DECIDES; `emit(const Command&)` must apply the command to the
// same OrderBook before returning (and may also log it / feed a strategy).

#include <cstdint>
#include <utility>
#include <vector>

#include "orderbook/apply.hpp"
#include "orderbook/formats.hpp"
#include "orderbook/order_book.hpp"

namespace ob {

class L2Converter {
public:
    static constexpr OrderId kStrategyBit = 1ull << 62;
    static bool isSynthetic(OrderId id) { return (id & kStrategyBit) == 0; }

    struct Stats {
        std::uint64_t levelUpdates = 0, trades = 0, outOfWindow = 0;
        std::uint64_t adds = 0, cancels = 0, reduces = 0, iocs = 0;
    };

    explicit L2Converter(const OrderBook& book, OrderId firstId = 1) : book_(book), nextId_(firstId) {
        scratch_.reserve(4096);
    }

    template <class Emit>
    void onRecord(const MdRecord& r, Emit&& emit) {
        if (!book_.inWindow(r.price)) {
            ++stats_.outOfWindow;
            return;
        }
        if (r.kind == MdKind::Trade) {
            ++stats_.trades;
            if (r.qty <= 0) return;
            ++stats_.iocs;
            emit(makeNew(r.ts, nextId_++, r.side, OrderType::IOC, r.price, r.qty));
            return;
        }

        ++stats_.levelUpdates;
        Qty actual = 0;
        scratch_.clear();
        book_.forEachOrderAtLevelReverse(r.side, r.price, [&](OrderId id, Qty q) {
            if (isSynthetic(id)) {
                actual += q;
                scratch_.emplace_back(id, q);
            }
        });

        const Qty target = r.qty < 0 ? 0 : r.qty;
        if (target > actual) {
            ++stats_.adds;
            emit(makeNew(r.ts, nextId_++, r.side, OrderType::Limit, r.price, target - actual));
        } else if (target < actual) {
            Qty d = actual - target;
            for (auto [id, q] : scratch_) {  // back of queue first
                if (d == 0) break;
                if (q <= d) {
                    ++stats_.cancels;
                    emit(makeCancel(r.ts, id));
                    d -= q;
                } else {
                    ++stats_.reduces;
                    emit(makeModify(r.ts, id, r.price, q - d));
                    d = 0;
                }
            }
        }
    }

    const Stats& stats() const { return stats_; }
    OrderId      nextId() const { return nextId_; }

private:
    const OrderBook&                     book_;
    OrderId                              nextId_;
    std::vector<std::pair<OrderId, Qty>> scratch_;
    Stats                                stats_;
};

}  // namespace ob
