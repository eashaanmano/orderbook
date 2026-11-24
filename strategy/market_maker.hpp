#pragma once
// Inventory-skewed market maker: a simplified Avellaneda–Stoikov (2008) quoter.
//
//   reservation price  r      = mid - q * gamma * sigma^2 * T
//   optimal spread     spread = gamma * sigma^2 * T + (2/gamma) * ln(1 + gamma/k)
//   quotes             bid = floor(r - spread/2),  ask = ceil(r + spread/2)
//
//   q      inventory in units of quote size (positive = long)
//   sigma  mid volatility, ticks / sqrt(second), EWMA-estimated online
//   T      fixed horizon in seconds (the "infinite horizon" simplification)
//   gamma  risk aversion: higher = wider quotes and stronger inventory skew
//   k      how fast fill probability decays with distance from mid (1/ticks)
//
// Simplifications vs. the paper (be ready to defend these):
//   * T is a constant rather than time-to-close
//   * k is a parameter, not fitted to observed fill rates
//   * one order per side, fixed size; hard inventory cap stops quoting the risky side
//   * quotes are post-only: if a quote would cross on arrival it is rejected, not filled
//
// Everything is in integer ticks/lots except P&L, which is in quote currency (USDT).
// The strategy never touches the book directly: it returns Commands, and the
// backtest delivers them after the simulated latency.

#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "orderbook/apply.hpp"
#include "orderbook/formats.hpp"

namespace ob {

struct MMParams {
    double        gamma               = 0.01;
    double        k                   = 0.3;
    double        horizonSec          = 5.0;
    double        sigma0              = 4.0;    // initial vol guess, ticks/sqrt(s)
    double        sigmaHalfLifeSec    = 60.0;
    Qty           quoteLots           = 1000;   // 0.01 BTC at lot 1e-5
    Qty           maxInventoryLots    = 10000;
    Price         requoteTicks        = 1;      // move a quote only if off by >= this
    std::uint64_t minRequoteNs        = 50'000'000;  // throttle: 50 ms
    double        makerFeeBps         = 1.0;
    double        takerFeeBps         = 10.0;
    std::uint64_t markoutHorizonNs    = 1'000'000'000;
};

class MarketMaker {
public:
    static constexpr OrderId kIdBit = 1ull << 62;  // must equal L2Converter::kStrategyBit

    enum class St : std::uint8_t { Idle, PendingNew, Live, PendingCancel };
    struct Quote {
        OrderId id = 0;
        Side    side;
        Price   px  = 0;
        Qty     qty = 0;
        St      st  = St::Idle;
        bool    filledOnce = false;
    };

    struct Stats {
        std::uint64_t ordersSent = 0, ordersFilled = 0, fills = 0, postOnlyRejects = 0, cancelsSent = 0;
        Qty           lotsQuoted = 0, lotsFilled = 0, maxAbsInventory = 0;
        double        markoutTicksXQty = 0;  // sum of sign*(mid_later - fill_px)*qty
        Qty           markoutQty       = 0;
    };

    MarketMaker(const MMParams& p, double tick, double lot) : p_(p), tick_(tick), lot_(lot), var_(p.sigma0 * p.sigma0) {
        bid_.side = Side::Buy;
        ask_.side = Side::Sell;
    }

    // Called after every market event with the best EXTERNAL prices (own quotes excluded).
    void onMarket(std::uint64_t ts, std::optional<Price> bb, std::optional<Price> ba, std::vector<Command>& out) {
        if (!bb || !ba) return;
        mid_ = 0.5 * static_cast<double>(*bb + *ba);
        updateVol(ts);
        updateMarkouts(ts);

        if (ts - lastQuoteTs_ < p_.minRequoteNs) return;
        lastQuoteTs_ = ts;

        const double q      = static_cast<double>(inventory_) / static_cast<double>(p_.quoteLots);
        const double risk   = p_.gamma * var_ * p_.horizonSec;
        const double r      = mid_ - q * risk;
        const double spread = risk + (2.0 / p_.gamma) * std::log(1.0 + p_.gamma / p_.k);
        const double half   = std::max(0.5, spread / 2.0);

        std::optional<Price> wantBid = static_cast<Price>(std::floor(r - half));
        std::optional<Price> wantAsk = static_cast<Price>(std::ceil(r + half));
        // Never cross the external market (we'd be rejected as post-only anyway).
        if (*wantBid >= *ba) wantBid = *ba - 1;
        if (*wantAsk <= *bb) wantAsk = *bb + 1;
        // Hard inventory cap: stop adding to the risky side.
        if (inventory_ + p_.quoteLots > p_.maxInventoryLots) wantBid.reset();
        if (inventory_ - p_.quoteLots < -p_.maxInventoryLots) wantAsk.reset();

        manage(ts, bid_, wantBid, out);
        manage(ts, ask_, wantAsk, out);
    }

    void onNewAck(OrderId id, bool accepted) {
        Quote* q = find(id);
        if (!q || q->st != St::PendingNew) return;
        if (accepted) q->st = St::Live;
        else {
            q->st = St::Idle;
            ++stats_.postOnlyRejects;
        }
    }

    void onCancelAck(OrderId id) {
        Quote* q = find(id);
        if (q) q->st = St::Idle;
    }

    void onFill(std::uint64_t ts, OrderId id, Price px, Qty qty, bool maker) {
        Quote* q = find(id);
        if (!q) return;
        const double notional = static_cast<double>(px) * tick_ * static_cast<double>(qty) * lot_;
        const double fee      = notional * (maker ? p_.makerFeeBps : p_.takerFeeBps) / 1e4;
        if (q->side == Side::Buy) {
            inventory_ += qty;
            cash_ -= notional;
        } else {
            inventory_ -= qty;
            cash_ += notional;
        }
        cash_ -= fee;
        fees_ += fee;
        ++stats_.fills;
        stats_.lotsFilled += qty;
        if (!q->filledOnce) {
            q->filledOnce = true;
            ++stats_.ordersFilled;
        }
        stats_.maxAbsInventory = std::max(stats_.maxAbsInventory, inventory_ < 0 ? -inventory_ : inventory_);
        markouts_.push_back({ts, q->side == Side::Buy ? 1 : -1, px, qty});
        q->qty -= qty;
        if (q->qty == 0) q->st = St::Idle;  // any in-flight cancel will come back UnknownId and be ignored
    }

    // Own resting qty at a price on a side (so the backtest can exclude it from "the market").
    Qty ownQtyAt(Side s, Price px) const {
        const Quote& q = s == Side::Buy ? bid_ : ask_;
        return (q.st == St::Live || q.st == St::PendingCancel) && q.px == px ? q.qty : 0;
    }

    double cash() const { return cash_; }
    double fees() const { return fees_; }
    Qty    inventory() const { return inventory_; }
    double mid() const { return mid_; }
    double sigma() const { return std::sqrt(var_); }
    double markToMarket() const { return cash_ + static_cast<double>(inventory_) * lot_ * mid_ * tick_; }
    const Stats& stats() const { return stats_; }
    const Quote& bid() const { return bid_; }
    const Quote& ask() const { return ask_; }

private:
    struct Markout {
        std::uint64_t ts;
        int           sign;
        Price         px;
        Qty           qty;
    };

    Quote* find(OrderId id) {
        if (bid_.id == id) return &bid_;
        if (ask_.id == id) return &ask_;
        return nullptr;
    }

    void manage(std::uint64_t ts, Quote& q, std::optional<Price> want, std::vector<Command>& out) {
        switch (q.st) {
            case St::Idle:
                if (want) {
                    q.id         = kIdBit | nextId_++;
                    q.px         = *want;
                    q.qty        = p_.quoteLots;
                    q.st         = St::PendingNew;
                    q.filledOnce = false;
                    out.push_back(makeNew(ts, q.id, q.side, OrderType::Limit, q.px, q.qty));
                    ++stats_.ordersSent;
                    stats_.lotsQuoted += q.qty;
                }
                break;
            case St::Live:
                if (!want || std::llabs(*want - q.px) >= p_.requoteTicks) {
                    q.st = St::PendingCancel;
                    out.push_back(makeCancel(ts, q.id));
                    ++stats_.cancelsSent;
                }
                break;
            default: break;  // wait for the in-flight request to land
        }
    }

    void updateVol(std::uint64_t ts) {
        if (lastVolTs_ == 0) {
            lastVolTs_  = ts;
            lastVolMid_ = mid_;
            return;
        }
        const double dt = static_cast<double>(ts - lastVolTs_) / 1e9;
        if (dt < 1.0) return;
        const double dm    = mid_ - lastVolMid_;
        const double alpha = 1.0 - std::exp(-dt * std::log(2.0) / p_.sigmaHalfLifeSec);
        var_ += alpha * (dm * dm / dt - var_);
        lastVolTs_  = ts;
        lastVolMid_ = mid_;
    }

    void updateMarkouts(std::uint64_t ts) {
        while (!markouts_.empty() && markouts_.front().ts + p_.markoutHorizonNs <= ts) {
            const Markout& m = markouts_.front();
            stats_.markoutTicksXQty += m.sign * (mid_ - static_cast<double>(m.px)) * static_cast<double>(m.qty);
            stats_.markoutQty += m.qty;
            markouts_.pop_front();
        }
    }

    MMParams      p_;
    double        tick_, lot_;
    Quote         bid_, ask_;
    std::uint64_t nextId_      = 1;
    std::uint64_t lastQuoteTs_ = 0;
    double        mid_         = 0;
    double        var_;
    std::uint64_t lastVolTs_  = 0;
    double        lastVolMid_ = 0;
    Qty           inventory_  = 0;
    double        cash_ = 0, fees_ = 0;
    std::deque<Markout> markouts_;
    Stats         stats_;
};

}  // namespace ob
