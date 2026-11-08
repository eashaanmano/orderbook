#pragma once
// The fast matching engine.
//
// Layout
//   * Each side is a flat array of Levels indexed by (price - minPrice).
//     Finding a level is one subtraction and one array index; no tree walk.
//   * Each Level is an intrusive doubly linked list of Orders (FIFO = time priority).
//     Orders live in a preallocated ObjectPool and link to each other by 32-bit index.
//   * OrderId -> pool index lives in a fixed-capacity open-addressing FlatHashMap,
//     so cancel/modify are O(1).
//   * A LevelBitmap per side finds the next non-empty level 64 levels at a time.
//
// Hot path guarantees: submit/cancel/modify never allocate, never lock, never throw.
// The engine is single-threaded by design; determinism comes from that.
//
// Semantics (mirrored exactly by reference/ReferenceBook, checked by fuzz_diff):
//   Limit   match what crosses, rest the remainder
//   Market  match at any price, cancel the remainder (price field ignored)
//   IOC     match what crosses the limit, cancel the remainder
//   FOK     if the full qty can't be filled within the limit, reject with no trades
//   cancel  remove a resting order
//   modify  same price and qty reduced (or unchanged) -> in place, keeps queue priority
//           price change or qty increase              -> cancel + re-enter (loses priority,
//                                                        may match immediately)
// Validation order (submit): qty > 0, id unused, price in window, capacity, FOK check.
// Validation order (modify): id exists, qty > 0, price in window.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "orderbook/flat_hash_map.hpp"
#include "orderbook/level_bitmap.hpp"
#include "orderbook/object_pool.hpp"
#include "orderbook/types.hpp"

namespace ob {

struct EngineConfig {
    Price         minPrice  = 0;          // lowest tradable price (ticks)
    std::uint32_t numLevels = 1u << 16;   // tradable window: [minPrice, minPrice + numLevels)
    std::uint32_t maxOrders = 1u << 20;   // max simultaneously resting orders
};

class OrderBook {
public:
    explicit OrderBook(const EngineConfig& cfg)
        : cfg_(cfg),
          sides_{SideBook(cfg.numLevels), SideBook(cfg.numLevels)},
          pool_(cfg.maxOrders),
          index_(cfg.maxOrders) {}

    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // ---------------------------------------------------------------- commands

    template <class OnTrade>
    Result submit(const NewOrder& o, OnTrade&& onTrade) {
        if (o.qty <= 0) return rejected(Reject::BadQty);
        if (o.id == kInvalidOrderId || index_.find(o.id)) return rejected(Reject::DuplicateId);

        std::int64_t limitIdx;
        if (o.type == OrderType::Market) {
            limitIdx = (o.side == Side::Buy) ? static_cast<std::int64_t>(cfg_.numLevels) - 1 : 0;
        } else {
            if (!inWindow(o.price)) return rejected(Reject::BadPrice);
            limitIdx = o.price - cfg_.minPrice;
        }
        // Conservative: a Limit needs a free slot up front, even if it would fully fill.
        if (o.type == OrderType::Limit && pool_.inUse() == pool_.capacity())
            return rejected(Reject::CapacityExhausted);
        if (o.type == OrderType::FOK && available(o.side, limitIdx, o.qty) < o.qty)
            return rejected(Reject::FokUnfillable);

        const Qty filled = match(o.id, o.side, limitIdx, o.qty, onTrade);
        const Qty rem    = o.qty - filled;
        if (rem == 0) return {Status::Filled, Reject::None, filled, 0};
        if (o.type == OrderType::Limit) {
            rest(o.id, o.side, static_cast<std::uint32_t>(limitIdx), rem);
            return {Status::Resting, Reject::None, filled, rem};
        }
        return {Status::Cancelled, Reject::None, filled, 0};
    }

    Result submit(const NewOrder& o) {
        return submit(o, [](const Trade&) {});
    }

    Result cancel(OrderId id) {
        const std::uint32_t* p = index_.find(id);
        if (!p) return rejected(Reject::UnknownId);
        const Qty q = removeOrder(*p);
        return {Status::Cancelled, Reject::None, 0, q};
    }

    template <class OnTrade>
    Result modify(OrderId id, Price newPrice, Qty newQty, OnTrade&& onTrade) {
        const std::uint32_t* p = index_.find(id);
        if (!p) return rejected(Reject::UnknownId);
        if (newQty <= 0) return rejected(Reject::BadQty);
        if (!inWindow(newPrice)) return rejected(Reject::BadPrice);

        const std::uint32_t oi = *p;
        Order&              o  = pool_[oi];
        const auto          li = static_cast<std::uint32_t>(newPrice - cfg_.minPrice);
        if (li == o.level && newQty <= o.qty) {
            // Reduce in place: time priority is preserved.
            sides_[idx(o.side)].levels[li].total -= (o.qty - newQty);
            o.qty = newQty;
            return {Status::Resting, Reject::None, 0, newQty};
        }
        const Side side = o.side;
        removeOrder(oi);
        return submit(NewOrder{id, side, OrderType::Limit, newPrice, newQty}, onTrade);
    }

    Result modify(OrderId id, Price newPrice, Qty newQty) {
        return modify(id, newPrice, newQty, [](const Trade&) {});
    }

    // ----------------------------------------------------------------- queries

    std::optional<Price> bestBid() const { return bestOf(Side::Buy); }
    std::optional<Price> bestAsk() const { return bestOf(Side::Sell); }

    Qty levelQty(Side s, Price px) const {
        if (!inWindow(px)) return 0;
        return sides_[idx(s)].levels[px - cfg_.minPrice].total;
    }

    std::uint64_t orderCount() const { return index_.size(); }
    bool          contains(OrderId id) const { return index_.find(id) != nullptr; }
    Qty           orderQty(OrderId id) const {
        const std::uint32_t* p = index_.find(id);
        return p ? pool_[*p].qty : 0;
    }

    // Levels best -> worst: f(Price, Qty total)
    template <class F>
    void forEachLevel(Side s, F&& f) const {
        forEachLevelWhile(s, [&](Price px, Qty q) { f(px, q); return true; });
    }

    // Same, but stops as soon as f returns false.
    template <class F>
    void forEachLevelWhile(Side s, F&& f) const {
        const SideBook& sb = sides_[idx(s)];
        std::int64_t    i  = sb.best;
        while (i >= 0) {
            if (!f(cfg_.minPrice + i, sb.levels[i].total)) return;
            i = nextBest(s, static_cast<std::uint32_t>(i));
        }
    }

    // Orders at a level, queue front -> back: f(OrderId, Qty)
    template <class F>
    void forEachOrderAtLevel(Side s, Price px, F&& f) const {
        if (!inWindow(px)) return;
        for (std::uint32_t oi = sides_[idx(s)].levels[px - cfg_.minPrice].head; oi != kNil;
             oi               = pool_[oi].next)
            f(pool_[oi].id, pool_[oi].qty);
    }

    // Orders at a level, queue back -> front: f(OrderId, Qty)
    template <class F>
    void forEachOrderAtLevelReverse(Side s, Price px, F&& f) const {
        if (!inWindow(px)) return;
        for (std::uint32_t oi = sides_[idx(s)].levels[px - cfg_.minPrice].tail; oi != kNil;
             oi               = pool_[oi].prev)
            f(pool_[oi].id, pool_[oi].qty);
    }

    const EngineConfig& config() const { return cfg_; }
    bool inWindow(Price px) const {
        return px >= cfg_.minPrice && px < cfg_.minPrice + static_cast<Price>(cfg_.numLevels);
    }

private:
    struct Order {
        OrderId       id;
        Qty           qty;
        std::uint32_t level;  // index into the side's level array
        std::uint32_t prev;
        std::uint32_t next;
        Side          side;
    };
    static_assert(sizeof(Order) == 32, "keep Order at 2 per cache line");

    struct Level {
        std::uint32_t head  = kNil;
        std::uint32_t tail  = kNil;
        Qty           total = 0;
    };
    static_assert(sizeof(Level) == 16, "keep Level at 4 per cache line");

    struct SideBook {
        explicit SideBook(std::uint32_t n) : levels(n), bits(n) {}
        std::vector<Level> levels;
        LevelBitmap        bits;
        std::int64_t       best = -1;  // level index, -1 = empty side
    };

    static constexpr std::size_t idx(Side s) { return static_cast<std::size_t>(s); }
    static Result rejected(Reject r) { return {Status::Rejected, r, 0, 0}; }

    std::optional<Price> bestOf(Side s) const {
        const std::int64_t b = sides_[idx(s)].best;
        if (b < 0) return std::nullopt;
        return cfg_.minPrice + b;
    }

    // Is `oppBest` (a level on the opposite side) tradable for an aggressor on `side`?
    static bool crosses(Side side, std::int64_t oppBest, std::int64_t limitIdx) {
        return side == Side::Buy ? oppBest <= limitIdx : oppBest >= limitIdx;
    }

    // Next best level strictly worse than `from` on book side s.
    std::int64_t nextBest(Side s, std::uint32_t from) const {
        const SideBook& sb = sides_[idx(s)];
        return s == Side::Buy ? sb.bits.highestAtOrBelow(static_cast<std::int64_t>(from) - 1)
                              : sb.bits.lowestAtOrAbove(static_cast<std::int64_t>(from) + 1);
    }

    // Liquidity available to an aggressor on `side` within limitIdx, stopping at `want`.
    Qty available(Side side, std::int64_t limitIdx, Qty want) const {
        const Side      os  = opposite(side);
        const SideBook& opp = sides_[idx(os)];
        Qty             sum = 0;
        for (std::int64_t i = opp.best; i >= 0 && crosses(side, i, limitIdx) && sum < want;
             i              = nextBest(os, static_cast<std::uint32_t>(i)))
            sum += opp.levels[i].total;
        return sum;
    }

    template <class OnTrade>
    Qty match(OrderId taker, Side side, std::int64_t limitIdx, Qty qty, OnTrade& onTrade) {
        const Side os  = opposite(side);
        SideBook&  opp = sides_[idx(os)];
        Qty        rem = qty;
        while (rem > 0 && opp.best >= 0 && crosses(side, opp.best, limitIdx)) {
            const auto  li = static_cast<std::uint32_t>(opp.best);
            Level&      lv = opp.levels[li];
            const Price px = cfg_.minPrice + li;
            std::uint32_t oi = lv.head;
            while (rem > 0 && oi != kNil) {
                Order&    m = pool_[oi];
                const Qty f = std::min(rem, m.qty);
                m.qty -= f;
                lv.total -= f;
                rem -= f;
                onTrade(Trade{taker, m.id, side, px, f});
                const std::uint32_t next = m.next;
                if (m.qty == 0) {  // maker fully filled: it is always the queue head
                    lv.head = next;
                    if (next != kNil) pool_[next].prev = kNil;
                    else lv.tail = kNil;
                    index_.erase(m.id);
                    pool_.release(oi);
                }
                oi = next;
            }
            if (lv.head == kNil) {
                opp.bits.clear(li);
                opp.best = nextBest(os, li);
            }
        }
        return qty - rem;
    }

    void rest(OrderId id, Side side, std::uint32_t li, Qty qty) {
        const std::uint32_t oi = pool_.alloc();  // capacity checked in submit()
        SideBook&           sb = sides_[idx(side)];
        Level&              lv = sb.levels[li];
        pool_[oi]              = Order{id, qty, li, lv.tail, kNil, side};
        if (lv.tail != kNil) {
            pool_[lv.tail].next = oi;
        } else {
            lv.head = oi;
            sb.bits.set(li);
        }
        lv.tail = oi;
        lv.total += qty;
        index_.insert(id, oi);
        const bool better = side == Side::Buy ? static_cast<std::int64_t>(li) > sb.best
                                              : (sb.best < 0 || static_cast<std::int64_t>(li) < sb.best);
        if (better) sb.best = li;
    }

    Qty removeOrder(std::uint32_t oi) {
        Order&    o  = pool_[oi];
        SideBook& sb = sides_[idx(o.side)];
        Level&    lv = sb.levels[o.level];
        if (o.prev != kNil) pool_[o.prev].next = o.next;
        else lv.head = o.next;
        if (o.next != kNil) pool_[o.next].prev = o.prev;
        else lv.tail = o.prev;
        lv.total -= o.qty;
        if (lv.head == kNil) {
            sb.bits.clear(o.level);
            if (sb.best == static_cast<std::int64_t>(o.level)) sb.best = nextBest(o.side, o.level);
        }
        const Qty q = o.qty;
        index_.erase(o.id);
        pool_.release(oi);
        return q;
    }

    EngineConfig      cfg_;
    SideBook          sides_[2];
    ObjectPool<Order> pool_;
    FlatHashMap       index_;
};

}  // namespace ob
