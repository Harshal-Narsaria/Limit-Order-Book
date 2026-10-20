#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "lob/object_pool.hpp"
#include "lob/types.hpp"

namespace lob {

struct FlatOrderBookConfig {
    Price min_price = 0;               // lowest supported tick (inclusive)
    Price max_price = 65'535;          // highest supported tick (inclusive)
    std::size_t max_orders = 1 << 16;    // resting orders the pool can hold at once
    std::size_t max_order_id = 1 << 20;  // order ids must be < this value
};

// Optimised implementation for a known, bounded price range.
//
//   price levels : one std::vector<Level> per side, indexed by
//                  (price - min_price). Finding a level is an array index,
//                  not a tree search.
//   best prices  : tracked explicitly per side. Adding can only improve the
//                  best price; when the best level empties we scan outward to
//                  the next non-empty level.
//   orders       : allocated from a preallocated ObjectPool and linked into
//                  their level through prev/next pointers stored inside the
//                  Order itself (an intrusive doubly linked list).
//   order lookup : std::vector<Order*> indexed directly by order id.
//
// After construction nothing on the hot path touches the heap (as long as the
// caller's trade vector has reserved capacity).
class FlatOrderBook {
public:
    explicit FlatOrderBook(const FlatOrderBookConfig& config = {});

    // Orders hold pointers into the pool, so the book is neither copyable nor movable.
    FlatOrderBook(const FlatOrderBook&) = delete;
    FlatOrderBook& operator=(const FlatOrderBook&) = delete;

    bool add_limit(OrderId id, Side side, Price price, Quantity quantity, std::vector<Trade>& trades);
    bool add_market(OrderId id, Side side, Quantity quantity, std::vector<Trade>& trades);
    bool cancel(OrderId id);
    bool modify(OrderId id, Price new_price, Quantity new_quantity, std::vector<Trade>& trades);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    Quantity volume_at(Side side, Price price) const;
    std::size_t order_count() const { return bids_.order_count + asks_.order_count; }
    BookSnapshot snapshot() const;

private:
    struct Order {
        OrderId id = 0;
        Price price = 0;
        Quantity quantity = 0;
        Side side = Side::Buy;
        Order* prev = nullptr;  // towards the front (older orders)
        Order* next = nullptr;  // towards the back (newer orders)
    };

    struct Level {
        Order* head = nullptr;  // oldest order: matched first
        Order* tail = nullptr;  // newest order: new arrivals go here
        Quantity total_quantity = 0;

        bool empty() const { return head == nullptr; }
    };

    struct BookSide {
        std::vector<Level> levels;
        Price best = 0;  // only meaningful while order_count > 0
        std::size_t order_count = 0;
    };

    bool valid_price(Price price) const { return price >= min_price_ && price <= max_price_; }
    BookSide& side_for(Side side) { return side == Side::Buy ? bids_ : asks_; }
    const BookSide& side_for(Side side) const { return side == Side::Buy ? bids_ : asks_; }
    Level& level_at(BookSide& book_side, Price price) {
        return book_side.levels[static_cast<std::size_t>(price - min_price_)];
    }
    const Level& level_at(const BookSide& book_side, Price price) const {
        return book_side.levels[static_cast<std::size_t>(price - min_price_)];
    }
    Order* find(OrderId id) const {
        return id < orders_by_id_.size() ? orders_by_id_[static_cast<std::size_t>(id)] : nullptr;
    }

    Quantity match(OrderId aggressor_id, Side side, Price limit, Quantity quantity,
                   std::vector<Trade>& trades);
    void rest(OrderId id, Side side, Price price, Quantity quantity);
    void remove(Order* order);

    Price min_price_;
    Price max_price_;
    BookSide bids_;
    BookSide asks_;
    ObjectPool<Order> pool_;
    std::vector<Order*> orders_by_id_;
};

}  // namespace lob
