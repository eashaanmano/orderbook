#pragma once
// Apply one serialized Command to the engine. Shared by replay, pipeline,
// converter and backtest so every tool interprets commands identically.

#include "orderbook/formats.hpp"
#include "orderbook/order_book.hpp"

namespace ob {

template <class OnTrade>
inline Result apply(OrderBook& book, const Command& c, OnTrade&& onTrade) {
    switch (c.type) {
        case CmdType::New:
            return book.submit(NewOrder{c.id, c.side, c.otype, c.price, c.qty}, onTrade);
        case CmdType::Cancel:
            return book.cancel(c.id);
        case CmdType::Modify:
            return book.modify(c.id, c.price, c.qty, onTrade);
    }
    return Result{};
}

inline Command makeNew(std::uint64_t ts, OrderId id, Side s, OrderType t, Price px, Qty q) {
    Command c{};
    c.ts = ts; c.id = id; c.side = s; c.otype = t; c.price = px; c.qty = q; c.type = CmdType::New;
    return c;
}
inline Command makeCancel(std::uint64_t ts, OrderId id) {
    Command c{};
    c.ts = ts; c.id = id; c.type = CmdType::Cancel;
    return c;
}
inline Command makeModify(std::uint64_t ts, OrderId id, Price px, Qty q) {
    Command c{};
    c.ts = ts; c.id = id; c.price = px; c.qty = q; c.type = CmdType::Modify;
    return c;
}

}  // namespace ob
