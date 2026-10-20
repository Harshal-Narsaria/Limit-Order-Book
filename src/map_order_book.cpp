#include "lob/map_order_book.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

namespace lob {

bool MapOrderBook::add_limit(OrderId id, Side side, Price price, Quantity quantity,
                             std::vector<Trade>& trades) {
    if (quantity <= 0 || orders_.contains(id)) {
        return false;
    }
    const Quantity remaining = match(id, side, price, quantity, trades);
    if (remaining > 0) {
        rest(id, side, price, remaining);
    }
    return true;
}

bool MapOrderBook::add_market(OrderId id, Side side, Quantity quantity, std::vector<Trade>& trades) {
    if (quantity <= 0) {
        return false;
    }
    // A market order is a limit order with a limit price that crosses everything.
    const Price limit = side == Side::Buy ? std::numeric_limits<Price>::max()
                                          : std::numeric_limits<Price>::min();
    match(id, side, limit, quantity, trades);  // leftover quantity is discarded
    return true;
}

bool MapOrderBook::cancel(OrderId id) {
    const auto it = orders_.find(id);
    if (it == orders_.end()) {
        return false;
    }
    remove(it->second);
    orders_.erase(it);
    return true;
}

bool MapOrderBook::modify(OrderId id, Price new_price, Quantity new_quantity,
                          std::vector<Trade>& trades) {
    const auto it = orders_.find(id);
    if (it == orders_.end() || new_quantity <= 0) {
        return false;
    }
    Level& level = it->second.level->second;
    Order& order = *it->second.order;

    // Reducing quantity at the same price keeps the order's place in the queue.
    if (new_price == order.price && new_quantity <= order.quantity) {
        level.total_quantity -= order.quantity - new_quantity;
        order.quantity = new_quantity;
        return true;
    }

    // Anything else loses priority: cancel and re-add as a fresh order.
    const Side side = order.side;
    remove(it->second);
    orders_.erase(it);
    return add_limit(id, side, new_price, new_quantity, trades);
}

std::optional<Price> MapOrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.rbegin()->first;
}

std::optional<Price> MapOrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

Quantity MapOrderBook::volume_at(Side side, Price price) const {
    const Levels& levels = levels_for(side);
    const auto it = levels.find(price);
    return it == levels.end() ? 0 : it->second.total_quantity;
}

BookSnapshot MapOrderBook::snapshot() const {
    auto to_snapshot = [](Price price, const Level& level) {
        LevelSnapshot out{price, {}};
        for (const Order& order : level.orders) {
            out.orders.push_back({order.id, order.quantity});
        }
        return out;
    };

    BookSnapshot snap;
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
        snap.bids.push_back(to_snapshot(it->first, it->second));
    }
    for (const auto& [price, level] : asks_) {
        snap.asks.push_back(to_snapshot(price, level));
    }
    return snap;
}

Quantity MapOrderBook::match(OrderId aggressor_id, Side side, Price limit, Quantity quantity,
                             std::vector<Trade>& trades) {
    Levels& opposite_levels = levels_for(opposite(side));

    while (quantity > 0 && !opposite_levels.empty()) {
        // Best opposite price: lowest ask for a buyer, highest bid for a seller.
        const auto level_it = side == Side::Buy ? opposite_levels.begin()
                                                : std::prev(opposite_levels.end());
        const Price level_price = level_it->first;
        const bool crosses = side == Side::Buy ? level_price <= limit : level_price >= limit;
        if (!crosses) {
            break;
        }

        // Within the level, the front of the list is the oldest order.
        Level& level = level_it->second;
        Order& resting = level.orders.front();
        const Quantity fill = std::min(quantity, resting.quantity);
        trades.push_back({aggressor_id, resting.id, level_price, fill});

        quantity -= fill;
        resting.quantity -= fill;
        level.total_quantity -= fill;

        if (resting.quantity == 0) {
            orders_.erase(resting.id);
            level.orders.pop_front();
            if (level.orders.empty()) {
                opposite_levels.erase(level_it);
            }
        }
    }
    return quantity;
}

void MapOrderBook::rest(OrderId id, Side side, Price price, Quantity quantity) {
    Levels& levels = levels_for(side);
    const auto level_it = levels.try_emplace(price).first;  // creates the level if missing
    Level& level = level_it->second;
    level.orders.push_back({id, side, price, quantity});
    level.total_quantity += quantity;
    orders_.emplace(id, Location{level_it, std::prev(level.orders.end())});
}

// Unlinks an order from its level (the caller erases it from the index).
void MapOrderBook::remove(const Location& location) {
    Level& level = location.level->second;
    const Side side = location.order->side;
    level.total_quantity -= location.order->quantity;
    level.orders.erase(location.order);
    if (level.orders.empty()) {
        levels_for(side).erase(location.level);
    }
}

}  // namespace lob
