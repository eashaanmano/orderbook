#pragma once
// The slow, obviously-correct reference engine.
//
// std::map of price -> std::list of orders, std::unordered_map for lookup.
// Written to be read, not to be fast: every rule is spelled out in the most
// direct way possible so it can serve as the oracle in differential fuzzing.
// It must implement EXACTLY the semantics documented in order_book.hpp,
// including validation order and reject reasons.
//
// It is also the "naive version" the benchmarks compare against.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "orderbook/types.hpp"

namespace ob {

class ReferenceBook {
public:
    ReferenceBook(Price minPrice, std::uint32_t numLevels)
        : minPrice_(minPrice), maxPrice_(minPrice + static_cast<Price>(numLevels) - 1) {}

    Result submit(const NewOrder& o, std::vector<Trade>& trades) {
        if (o.qty <= 0) return rejected(Reject::BadQty);
        if (o.id == kInvalidOrderId || where_.count(o.id)) return rejected(Reject::DuplicateId);
        const bool market = o.type == OrderType::Market;
        if (!market && (o.price < minPrice_ || o.price > maxPrice_)) return rejected(Reject::BadPrice);

        if (o.type == OrderType::FOK) {
            Qty avail = 0;
            if (o.side == Side::Buy) {
                for (auto& [px, q] : asks_)
                    if (px <= o.price) avail += levelTotal(q);
            } else {
                for (auto& [px, q] : bids_)
                    if (px >= o.price) avail += levelTotal(q);
            }
            if (avail < o.qty) return rejected(Reject::FokUnfillable);
        }

        Qty rem = o.qty;
        if (o.side == Side::Buy) {
            while (rem > 0 && !asks_.empty()) {
                auto it = asks_.begin();  // lowest ask
                if (!market && it->first > o.price) break;
                fillLevel(o, it->first, it->second, rem, trades);
                if (it->second.empty()) asks_.erase(it);
            }
        } else {
            while (rem > 0 && !bids_.empty()) {
                auto it = std::prev(bids_.end());  // highest bid
                if (!market && it->first < o.price) break;
                fillLevel(o, it->first, it->second, rem, trades);
                if (it->second.empty()) bids_.erase(it);
            }
        }

        const Qty filled = o.qty - rem;
        if (rem == 0) return {Status::Filled, Reject::None, filled, 0};
        if (o.type == OrderType::Limit) {
            auto& book = o.side == Side::Buy ? bids_ : asks_;
            book[o.price].push_back(RestingOrder{o.id, rem});
            where_[o.id] = {o.side, o.price};
            return {Status::Resting, Reject::None, filled, rem};
        }
        return {Status::Cancelled, Reject::None, filled, 0};
    }

    Result cancel(OrderId id) {
        auto w = where_.find(id);
        if (w == where_.end()) return rejected(Reject::UnknownId);
        auto [side, px] = w->second;
        auto& book = side == Side::Buy ? bids_ : asks_;
        auto& q    = book[px];
        auto  it   = std::find_if(q.begin(), q.end(), [&](auto& r) { return r.id == id; });
        const Qty removed = it->qty;
        q.erase(it);
        if (q.empty()) book.erase(px);
        where_.erase(w);
        return {Status::Cancelled, Reject::None, 0, removed};
    }

    Result modify(OrderId id, Price newPrice, Qty newQty, std::vector<Trade>& trades) {
        auto w = where_.find(id);
        if (w == where_.end()) return rejected(Reject::UnknownId);
        if (newQty <= 0) return rejected(Reject::BadQty);
        if (newPrice < minPrice_ || newPrice > maxPrice_) return rejected(Reject::BadPrice);
        auto [side, px] = w->second;
        auto& q  = (side == Side::Buy ? bids_ : asks_)[px];
        auto  it = std::find_if(q.begin(), q.end(), [&](auto& r) { return r.id == id; });
        if (newPrice == px && newQty <= it->qty) {
            it->qty = newQty;
            return {Status::Resting, Reject::None, 0, newQty};
        }
        cancel(id);
        return submit(NewOrder{id, side, OrderType::Limit, newPrice, newQty}, trades);
    }

    // ---- inspection (same shape as OrderBook's, for snapshot comparison)
    std::optional<Price> bestBid() const {
        if (bids_.empty()) return std::nullopt;
        return std::prev(bids_.end())->first;
    }
    std::optional<Price> bestAsk() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }
    std::uint64_t orderCount() const { return where_.size(); }

    template <class F>
    void forEachLevel(Side s, F&& f) const {
        if (s == Side::Buy)
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) f(it->first, levelTotal(it->second));
        else
            for (auto& [px, q] : asks_) f(px, levelTotal(q));
    }
    template <class F>
    void forEachOrderAtLevel(Side s, Price px, F&& f) const {
        auto& book = s == Side::Buy ? bids_ : asks_;
        auto  it   = book.find(px);
        if (it == book.end()) return;
        for (auto& r : it->second) f(r.id, r.qty);
    }

private:
    struct RestingOrder {
        OrderId id;
        Qty     qty;
    };
    using Queue = std::list<RestingOrder>;

    static Result rejected(Reject r) { return {Status::Rejected, r, 0, 0}; }
    static Qty    levelTotal(const Queue& q) {
        Qty t = 0;
        for (auto& r : q) t += r.qty;
        return t;
    }

    void fillLevel(const NewOrder& o, Price px, Queue& q, Qty& rem, std::vector<Trade>& trades) {
        while (rem > 0 && !q.empty()) {
            RestingOrder& m = q.front();
            const Qty     f = std::min(rem, m.qty);
            m.qty -= f;
            rem -= f;
            trades.push_back(Trade{o.id, m.id, o.side, px, f});
            if (m.qty == 0) {
                where_.erase(m.id);
                q.pop_front();
            }
        }
    }

    Price                                                  minPrice_, maxPrice_;
    std::map<Price, Queue>                                 bids_, asks_;
    std::unordered_map<OrderId, std::pair<Side, Price>>    where_;
};

}  // namespace ob
