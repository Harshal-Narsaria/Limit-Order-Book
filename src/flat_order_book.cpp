#include "lob/flat_order_book.hpp"

#include <algorithm>

namespace lob {

FlatOrderBook::FlatOrderBook(const FlatOrderBookConfig& config)
    : min_price_(config.min_price),
      max_price_(config.max_price),
      pool_(config.max_orders),
      orders_by_id_(config.max_order_id, nullptr) {
    const auto level_count = static_cast<std::size_t>(max_price_ - min_price_ + 1);
    bids_.levels.resize(level_count);
    asks_.levels.resize(level_count);
}

bool FlatOrderBook::add_limit(OrderId id, Side side, Price price, Quantity quantity,
                              std::vector<Trade>& trades) {
    // Reject up front, before any matching, so a rejected order has no side effects.
    // Running out of pool space is treated like any other rejection; the pool is
    // sized so this does not happen in normal operation.
    if (quantity <= 0 || !valid_price(price) || id >= orders_by_id_.size() ||
        orders_by_id_[static_cast<std::size_t>(id)] != nullptr || pool_.available() == 0) {
        return false;
    }
    const Quantity remaining = match(id, side, price, quantity, trades);
    if (remaining > 0) {
        rest(id, side, price, remaining);
    }
    return true;
}

bool FlatOrderBook::add_market(OrderId id, Side side, Quantity quantity, std::vector<Trade>& trades) {
    if (quantity <= 0) {
        return false;
    }
    // Every resting order is inside [min_price_, max_price_], so these limits cross everything.
    const Price limit = side == Side::Buy ? max_price_ : min_price_;
    match(id, side, limit, quantity, trades);  // leftover quantity is discarded
    return true;
}

bool FlatOrderBook::cancel(OrderId id) {
    Order* order = find(id);
    if (order == nullptr) {
        return false;
    }
    remove(order);
    return true;
}

bool FlatOrderBook::modify(OrderId id, Price new_price, Quantity new_quantity,
                           std::vector<Trade>& trades) {
    Order* order = find(id);
    if (order == nullptr || new_quantity <= 0 || !valid_price(new_price)) {
        return false;
    }

    // Reducing quantity at the same price keeps the order's place in the queue.
    if (new_price == order->price && new_quantity <= order->quantity) {
        level_at(side_for(order->side), order->price).total_quantity -= order->quantity - new_quantity;
        order->quantity = new_quantity;
        return true;
    }

    // Anything else loses priority: cancel and re-add as a fresh order.
    const Side side = order->side;
    remove(order);
    return add_limit(id, side, new_price, new_quantity, trades);
}

std::optional<Price> FlatOrderBook::best_bid() const {
    if (bids_.order_count == 0) {
        return std::nullopt;
    }
    return bids_.best;
}

std::optional<Price> FlatOrderBook::best_ask() const {
    if (asks_.order_count == 0) {
        return std::nullopt;
    }
    return asks_.best;
}

Quantity FlatOrderBook::volume_at(Side side, Price price) const {
    if (!valid_price(price)) {
        return 0;
    }
    return level_at(side_for(side), price).total_quantity;
}

BookSnapshot FlatOrderBook::snapshot() const {
    auto to_snapshot = [](Price price, const Level& level) {
        LevelSnapshot out{price, {}};
        for (const Order* order = level.head; order != nullptr; order = order->next) {
            out.orders.push_back({order->id, order->quantity});
        }
        return out;
    };

    BookSnapshot snap;
    if (bids_.order_count > 0) {
        for (Price price = bids_.best; price >= min_price_; --price) {
            const Level& level = level_at(bids_, price);
            if (!level.empty()) {
                snap.bids.push_back(to_snapshot(price, level));
            }
        }
    }
    if (asks_.order_count > 0) {
        for (Price price = asks_.best; price <= max_price_; ++price) {
            const Level& level = level_at(asks_, price);
            if (!level.empty()) {
                snap.asks.push_back(to_snapshot(price, level));
            }
        }
    }
    return snap;
}

Quantity FlatOrderBook::match(OrderId aggressor_id, Side side, Price limit, Quantity quantity,
                              std::vector<Trade>& trades) {
    BookSide& opposite_side = side_for(opposite(side));

    // Invariant: while a side has orders, its best level is non-empty, so the
    // order at the head of that level is the next one to trade with.
    while (quantity > 0 && opposite_side.order_count > 0) {
        const Price best = opposite_side.best;
        const bool crosses = side == Side::Buy ? best <= limit : best >= limit;
        if (!crosses) {
            break;
        }

        Level& level = level_at(opposite_side, best);
        Order* resting = level.head;
        const Quantity fill = std::min(quantity, resting->quantity);
        trades.push_back({aggressor_id, resting->id, best, fill});

        quantity -= fill;
        resting->quantity -= fill;
        level.total_quantity -= fill;

        if (resting->quantity == 0) {
            remove(resting);  // also moves `best` on if the level is now empty
        }
    }
    return quantity;
}

void FlatOrderBook::rest(OrderId id, Side side, Price price, Quantity quantity) {
    Order* order = pool_.allocate();  // add_limit checked that a slot is free
    *order = Order{id, price, quantity, side, nullptr, nullptr};

    BookSide& book_side = side_for(side);
    Level& level = level_at(book_side, price);

    // Append at the tail: newest order is last in time priority.
    order->prev = level.tail;
    if (level.tail != nullptr) {
        level.tail->next = order;
    } else {
        level.head = order;
    }
    level.tail = order;
    level.total_quantity += quantity;

    // A new order can only improve (or leave unchanged) the best price.
    const bool improves = book_side.order_count == 0 ||
                          (side == Side::Buy ? price > book_side.best : price < book_side.best);
    if (improves) {
        book_side.best = price;
    }
    ++book_side.order_count;
    orders_by_id_[static_cast<std::size_t>(id)] = order;
}

void FlatOrderBook::remove(Order* order) {
    BookSide& book_side = side_for(order->side);
    Level& level = level_at(book_side, order->price);

    // Unlink from the level's intrusive list in O(1).
    if (order->prev != nullptr) {
        order->prev->next = order->next;
    } else {
        level.head = order->next;
    }
    if (order->next != nullptr) {
        order->next->prev = order->prev;
    } else {
        level.tail = order->prev;
    }
    level.total_quantity -= order->quantity;
    --book_side.order_count;

    // If we just emptied the best level, walk outward to the next non-empty one.
    // The walk always terminates because the side still has at least one order.
    // Its cost is the gap in ticks to the next level, which is small for a book
    // whose activity is concentrated near the touch.
    if (level.empty() && order->price == book_side.best && book_side.order_count > 0) {
        const Price step = order->side == Side::Buy ? -1 : +1;
        do {
            book_side.best += step;
        } while (level_at(book_side, book_side.best).empty());
    }

    orders_by_id_[static_cast<std::size_t>(order->id)] = nullptr;
    pool_.release(order);
}

}  // namespace lob
