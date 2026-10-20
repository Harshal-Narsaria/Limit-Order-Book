#pragma once

#include <cstdint>
#include <vector>

namespace lob {

// Prices are integer ticks. A price of 10050 might mean 100.50 for an
// instrument with a 0.01 tick size; the book never needs to know.
using Price = std::int64_t;
using Quantity = std::int64_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }

// One fill between an incoming (aggressor) order and a resting order.
// The trade always happens at the resting order's price.
struct Trade {
    OrderId aggressor_id;
    OrderId resting_id;
    Price price;
    Quantity quantity;

    bool operator==(const Trade&) const = default;
};

// A read-only view of the book used by tests and the differential checker.
// Not used on the hot path.
struct OrderSnapshot {
    OrderId id;
    Quantity quantity;

    bool operator==(const OrderSnapshot&) const = default;
};

struct LevelSnapshot {
    Price price;
    std::vector<OrderSnapshot> orders;  // in time priority, earliest first

    bool operator==(const LevelSnapshot&) const = default;
};

struct BookSnapshot {
    std::vector<LevelSnapshot> bids;  // best (highest) price first
    std::vector<LevelSnapshot> asks;  // best (lowest) price first

    bool operator==(const BookSnapshot&) const = default;
};

}  // namespace lob
