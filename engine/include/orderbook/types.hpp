#pragma once
// Core value types shared by the fast engine, the reference engine, the
// replay tools and the strategy. Prices and quantities are integers
// (ticks and lots): floating point never touches the matching path.

#include <cstdint>
#include <limits>

namespace ob {

using OrderId = std::uint64_t;
using Price   = std::int64_t;  // integer ticks
using Qty     = std::int64_t;  // integer lots

// Reserved: used as the "empty slot" marker inside FlatHashMap.
inline constexpr OrderId kInvalidOrderId = std::numeric_limits<OrderId>::max();

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : std::uint8_t { Limit = 0, Market = 1, IOC = 2, FOK = 3 };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

enum class Status : std::uint8_t {
    Resting,    // (part of) the order is resting on the book
    Filled,     // fully filled, nothing rests
    Cancelled,  // remainder cancelled (Market/IOC leftover, or explicit cancel)
    Rejected,   // nothing happened
};

enum class Reject : std::uint8_t {
    None,
    DuplicateId,
    UnknownId,
    BadQty,
    BadPrice,
    FokUnfillable,
    CapacityExhausted,
};

struct NewOrder {
    OrderId   id;
    Side      side;
    OrderType type;
    Price     price;  // ignored for Market
    Qty       qty;
};

struct Trade {
    OrderId taker;
    OrderId maker;
    Side    takerSide;
    Price   price;
    Qty     qty;
    friend bool operator==(const Trade&, const Trade&) = default;
};

struct Result {
    Status status  = Status::Rejected;
    Reject reject  = Reject::None;
    Qty    filled  = 0;  // qty executed by this call
    Qty    resting = 0;  // qty left resting after this call (cancel: qty removed)
    friend bool operator==(const Result&, const Result&) = default;
};

inline const char* toString(Status s) {
    switch (s) {
        case Status::Resting: return "Resting";
        case Status::Filled: return "Filled";
        case Status::Cancelled: return "Cancelled";
        case Status::Rejected: return "Rejected";
    }
    return "?";
}

inline const char* toString(Reject r) {
    switch (r) {
        case Reject::None: return "None";
        case Reject::DuplicateId: return "DuplicateId";
        case Reject::UnknownId: return "UnknownId";
        case Reject::BadQty: return "BadQty";
        case Reject::BadPrice: return "BadPrice";
        case Reject::FokUnfillable: return "FokUnfillable";
        case Reject::CapacityExhausted: return "CapacityExhausted";
    }
    return "?";
}

}  // namespace ob
