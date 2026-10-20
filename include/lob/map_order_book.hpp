#pragma once

#include <cstddef>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Baseline implementation built from standard containers.
//
//   price levels : std::map<Price, Level>, one map per side, sorted ascending.
//                  Best ask = first element, best bid = last element.
//   each level   : std::list<Order> in arrival order (front = oldest), so
//                  time priority is just "take from the front".
//   order lookup : std::unordered_map<OrderId, Location> storing iterators to
//                  both the level and the order, so cancel is O(1) on average.
//
// std::map and std::list iterators stay valid when other elements are inserted
// or erased, which is what makes storing them in the index safe.
class MapOrderBook {
public:
    bool add_limit(OrderId id, Side side, Price price, Quantity quantity, std::vector<Trade>& trades);
    bool add_market(OrderId id, Side side, Quantity quantity, std::vector<Trade>& trades);
    bool cancel(OrderId id);
    bool modify(OrderId id, Price new_price, Quantity new_quantity, std::vector<Trade>& trades);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    Quantity volume_at(Side side, Price price) const;
    std::size_t order_count() const { return orders_.size(); }
    BookSnapshot snapshot() const;

private:
    struct Order {
        OrderId id;
        Side side;
        Price price;
        Quantity quantity;
    };

    using OrderQueue = std::list<Order>;

    struct Level {
        OrderQueue orders;
        Quantity total_quantity = 0;
    };

    using Levels = std::map<Price, Level>;

    struct Location {
        Levels::iterator level;
        OrderQueue::iterator order;
    };

    Levels& levels_for(Side side) { return side == Side::Buy ? bids_ : asks_; }
    const Levels& levels_for(Side side) const { return side == Side::Buy ? bids_ : asks_; }

    // Fills `quantity` of an incoming order against the opposite side, best
    // price first, and returns the quantity left unfilled.
    Quantity match(OrderId aggressor_id, Side side, Price limit, Quantity quantity,
                   std::vector<Trade>& trades);
    void rest(OrderId id, Side side, Price price, Quantity quantity);
    void remove(const Location& location);

    Levels bids_;
    Levels asks_;
    std::unordered_map<OrderId, Location> orders_;
};

}  // namespace lob
