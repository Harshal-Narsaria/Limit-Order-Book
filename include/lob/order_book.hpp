#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// The common interface both implementations satisfy.
//
// We use a C++20 concept rather than an abstract base class so that tests and
// benchmarks are templates: the compiler sees the concrete type, can inline
// everything, and there is no virtual-call cost in the measurements.
//
// Semantics shared by every implementation:
//   add_limit  - match against the opposite side (price-time priority), then
//                rest any remaining quantity. Returns false (and does nothing)
//                if the id is already live, quantity <= 0, or the price is not
//                supported by the book.
//   add_market - match against the opposite side; unfilled quantity is
//                discarded. Market orders never rest, so their id is only used
//                to tag trades. Returns false if quantity <= 0.
//   cancel     - remove a resting order. Returns false for an unknown id.
//   modify     - change a resting order's price and/or quantity.
//                  * same price, smaller quantity -> reduced in place, keeps
//                    its time priority
//                  * same price, same quantity    -> no-op
//                  * anything else                -> cancel + re-add with the
//                    same id; loses priority and may trade if it now crosses
//                Returns false (and does nothing) for an unknown id,
//                new_quantity <= 0, or an unsupported price.
//
// Every fill is appended to `trades`; the caller owns and clears the vector.
template <typename Book>
concept OrderBook = requires(Book book, const Book cbook, OrderId id, Side side, Price price,
                             Quantity qty, std::vector<Trade>& trades) {
    { book.add_limit(id, side, price, qty, trades) } -> std::same_as<bool>;
    { book.add_market(id, side, qty, trades) } -> std::same_as<bool>;
    { book.cancel(id) } -> std::same_as<bool>;
    { book.modify(id, price, qty, trades) } -> std::same_as<bool>;

    { cbook.best_bid() } -> std::same_as<std::optional<Price>>;
    { cbook.best_ask() } -> std::same_as<std::optional<Price>>;
    { cbook.volume_at(side, price) } -> std::same_as<Quantity>;
    { cbook.order_count() } -> std::same_as<std::size_t>;
    { cbook.snapshot() } -> std::same_as<BookSnapshot>;
};

}  // namespace lob
