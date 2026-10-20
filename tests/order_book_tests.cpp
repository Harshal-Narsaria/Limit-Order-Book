// Unit tests run against every OrderBook implementation.
//
// Each BOOK_TEST body is a template over `Book`, registered once per
// implementation, so both books are held to exactly the same behaviour.

#include <string>
#include <vector>

#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"
#include "test_framework.hpp"

using lob::BookSnapshot;
using lob::LevelSnapshot;
using lob::OrderSnapshot;
using lob::Side;
using lob::Trade;

static_assert(lob::OrderBook<lob::MapOrderBook>);

template <typename Test>
bool register_book_test(const std::string& name) {
    testing::add_test(name + " [map]", &Test::template run<lob::MapOrderBook>);
    return true;
}

#define BOOK_TEST(name)                                                                  \
    struct name {                                                                        \
        template <typename Book>                                                         \
        static void run();                                                               \
    };                                                                                   \
    static const bool name##_registered = register_book_test<name>(#name);               \
    template <typename Book>                                                             \
    void name::run()

// --- Resting orders ----------------------------------------------------------

BOOK_TEST(empty_book_has_no_prices) {
    Book book;
    CHECK(!book.best_bid().has_value());
    CHECK(!book.best_ask().has_value());
    CHECK_EQ(book.order_count(), 0u);
    CHECK(book.snapshot() == BookSnapshot{});
}

BOOK_TEST(non_crossing_orders_rest) {
    Book book;
    std::vector<Trade> trades;
    CHECK(book.add_limit(1, Side::Buy, 99, 10, trades));
    CHECK(book.add_limit(2, Side::Buy, 98, 20, trades));
    CHECK(book.add_limit(3, Side::Sell, 101, 30, trades));
    CHECK(book.add_limit(4, Side::Sell, 101, 5, trades));

    CHECK(trades.empty());
    CHECK_EQ(book.best_bid(), std::optional<lob::Price>{99});
    CHECK_EQ(book.best_ask(), std::optional<lob::Price>{101});
    CHECK_EQ(book.volume_at(Side::Buy, 98), 20);
    CHECK_EQ(book.volume_at(Side::Sell, 101), 35);
    CHECK_EQ(book.volume_at(Side::Sell, 99), 0);
    CHECK_EQ(book.order_count(), 4u);

    const BookSnapshot expected{
        {LevelSnapshot{99, {{1, 10}}}, LevelSnapshot{98, {{2, 20}}}},
        {LevelSnapshot{101, {{3, 30}, {4, 5}}}},
    };
    CHECK(book.snapshot() == expected);
}

// --- Fills -------------------------------------------------------------------

BOOK_TEST(exact_full_fill_empties_book) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 10, trades);
    CHECK(book.add_limit(2, Side::Buy, 100, 10, trades));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0] == (Trade{2, 1, 100, 10}));
    CHECK_EQ(book.order_count(), 0u);
    CHECK(!book.best_bid().has_value());
    CHECK(!book.best_ask().has_value());
}

BOOK_TEST(trade_happens_at_resting_price) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Buy, 100, 10, trades);
    book.add_limit(2, Side::Sell, 95, 10, trades);  // willing to sell lower

    REQUIRE(trades.size() == 1);
    CHECK(trades[0] == (Trade{2, 1, 100, 10}));
}

BOOK_TEST(partial_fill_rests_aggressor_remainder) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 5, trades);
    book.add_limit(2, Side::Buy, 101, 8, trades);

    REQUIRE(trades.size() == 1);
    CHECK(trades[0] == (Trade{2, 1, 100, 5}));
    CHECK(!book.best_ask().has_value());
    CHECK_EQ(book.best_bid(), std::optional<lob::Price>{101});
    CHECK_EQ(book.volume_at(Side::Buy, 101), 3);
}

BOOK_TEST(partial_fill_leaves_resting_remainder_at_front) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 10, trades);
    book.add_limit(2, Side::Sell, 100, 10, trades);
    book.add_limit(3, Side::Buy, 100, 4, trades);

    REQUIRE(trades.size() == 1);
    CHECK(trades[0] == (Trade{3, 1, 100, 4}));
    CHECK_EQ(book.volume_at(Side::Sell, 100), 16);
    CHECK(!book.best_bid().has_value());

    // Order 1 keeps its place at the front with 6 remaining.
    const BookSnapshot expected{{}, {LevelSnapshot{100, {{1, 6}, {2, 10}}}}};
    CHECK(book.snapshot() == expected);
}

BOOK_TEST(buy_sweeps_multiple_levels) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 5, trades);
    book.add_limit(2, Side::Sell, 101, 5, trades);
    book.add_limit(3, Side::Sell, 102, 5, trades);
    book.add_limit(4, Side::Buy, 101, 12, trades);

    const std::vector<Trade> expected{{4, 1, 100, 5}, {4, 2, 101, 5}};
    CHECK(trades == expected);
    CHECK_EQ(book.best_bid(), std::optional<lob::Price>{101});  // 2 left over rests
    CHECK_EQ(book.volume_at(Side::Buy, 101), 2);
    CHECK_EQ(book.best_ask(), std::optional<lob::Price>{102});
}

BOOK_TEST(sell_sweeps_multiple_levels) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Buy, 100, 5, trades);
    book.add_limit(2, Side::Buy, 99, 5, trades);
    book.add_limit(3, Side::Buy, 98, 5, trades);
    book.add_limit(4, Side::Sell, 98, 15, trades);

    const std::vector<Trade> expected{{4, 1, 100, 5}, {4, 2, 99, 5}, {4, 3, 98, 5}};
    CHECK(trades == expected);
    CHECK_EQ(book.order_count(), 0u);
}

// --- Priority ----------------------------------------------------------------

BOOK_TEST(time_priority_within_level) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 5, trades);
    book.add_limit(2, Side::Sell, 100, 5, trades);
    book.add_limit(3, Side::Sell, 100, 5, trades);
    book.add_limit(4, Side::Buy, 100, 7, trades);

    const std::vector<Trade> expected{{4, 1, 100, 5}, {4, 2, 100, 2}};
    CHECK(trades == expected);
    const BookSnapshot expected_book{{}, {LevelSnapshot{100, {{2, 3}, {3, 5}}}}};
    CHECK(book.snapshot() == expected_book);
}

BOOK_TEST(price_priority_beats_time_priority) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 101, 5, trades);  // earlier but worse price
    book.add_limit(2, Side::Sell, 100, 5, trades);
    book.add_limit(3, Side::Buy, 101, 5, trades);

    REQUIRE(trades.size() == 1);
    CHECK(trades[0] == (Trade{3, 2, 100, 5}));
}

// --- Cancel ------------------------------------------------------------------

BOOK_TEST(cancel_resting_order) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Buy, 100, 10, trades);
    book.add_limit(2, Side::Buy, 99, 10, trades);

    CHECK(book.cancel(1));
    CHECK_EQ(book.best_bid(), std::optional<lob::Price>{99});
    CHECK_EQ(book.volume_at(Side::Buy, 100), 0);
    CHECK_EQ(book.order_count(), 1u);
    CHECK(!book.cancel(1));  // already gone
}

BOOK_TEST(cancel_unknown_id_is_rejected) {
    Book book;
    std::vector<Trade> trades;
    CHECK(!book.cancel(42));
    book.add_limit(1, Side::Sell, 100, 10, trades);
    CHECK(!book.cancel(42));
    CHECK_EQ(book.order_count(), 1u);
}

BOOK_TEST(cancel_middle_of_queue_preserves_order) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 1, trades);
    book.add_limit(2, Side::Sell, 100, 2, trades);
    book.add_limit(3, Side::Sell, 100, 3, trades);
    CHECK(book.cancel(2));

    const BookSnapshot expected{{}, {LevelSnapshot{100, {{1, 1}, {3, 3}}}}};
    CHECK(book.snapshot() == expected);
    CHECK_EQ(book.volume_at(Side::Sell, 100), 4);
}

BOOK_TEST(cancelled_order_never_trades) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 5, trades);
    book.add_limit(2, Side::Sell, 101, 5, trades);
    book.cancel(1);
    book.add_limit(3, Side::Buy, 101, 5, trades);

    REQUIRE(trades.size() == 1);
    CHECK(trades[0] == (Trade{3, 2, 101, 5}));
}

BOOK_TEST(filled_order_cannot_be_cancelled) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 5, trades);
    book.add_limit(2, Side::Buy, 100, 5, trades);
    CHECK(!book.cancel(1));
    CHECK(!book.cancel(2));
}

// --- Modify ------------------------------------------------------------------

BOOK_TEST(modify_reduce_keeps_priority) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 10, trades);
    book.add_limit(2, Side::Sell, 100, 10, trades);
    CHECK(book.modify(1, 100, 4, trades));
    CHECK_EQ(book.volume_at(Side::Sell, 100), 14);

    book.add_limit(3, Side::Buy, 100, 5, trades);
    const std::vector<Trade> expected{{3, 1, 100, 4}, {3, 2, 100, 1}};
    CHECK(trades == expected);
}

BOOK_TEST(modify_increase_loses_priority) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 10, trades);
    book.add_limit(2, Side::Sell, 100, 10, trades);
    CHECK(book.modify(1, 100, 15, trades));

    const BookSnapshot expected{{}, {LevelSnapshot{100, {{2, 10}, {1, 15}}}}};
    CHECK(book.snapshot() == expected);
}

BOOK_TEST(modify_price_change_moves_and_loses_priority) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Buy, 99, 10, trades);
    book.add_limit(2, Side::Buy, 100, 10, trades);
    CHECK(book.modify(1, 100, 5, trades));  // price change, even with a smaller size

    CHECK(trades.empty());
    CHECK_EQ(book.volume_at(Side::Buy, 99), 0);
    const BookSnapshot expected{{LevelSnapshot{100, {{2, 10}, {1, 5}}}}, {}};
    CHECK(book.snapshot() == expected);
}

BOOK_TEST(modify_that_crosses_trades_as_aggressor) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 101, 4, trades);
    book.add_limit(2, Side::Buy, 99, 10, trades);
    CHECK(book.modify(2, 101, 10, trades));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0] == (Trade{2, 1, 101, 4}));
    CHECK_EQ(book.best_bid(), std::optional<lob::Price>{101});
    CHECK_EQ(book.volume_at(Side::Buy, 101), 6);
}

BOOK_TEST(modify_same_values_is_noop) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 10, trades);
    book.add_limit(2, Side::Sell, 100, 10, trades);
    CHECK(book.modify(1, 100, 10, trades));

    const BookSnapshot expected{{}, {LevelSnapshot{100, {{1, 10}, {2, 10}}}}};
    CHECK(book.snapshot() == expected);
}

BOOK_TEST(modify_invalid_requests_are_rejected) {
    Book book;
    std::vector<Trade> trades;
    CHECK(!book.modify(7, 100, 10, trades));  // unknown id
    book.add_limit(1, Side::Sell, 100, 10, trades);
    CHECK(!book.modify(1, 100, 0, trades));   // zero quantity: use cancel instead
    CHECK(!book.modify(1, 100, -3, trades));
    CHECK_EQ(book.volume_at(Side::Sell, 100), 10);
}

// --- Market orders -------------------------------------------------------------

BOOK_TEST(market_order_on_empty_book_does_nothing) {
    Book book;
    std::vector<Trade> trades;
    CHECK(book.add_market(1, Side::Buy, 10, trades));
    CHECK(book.add_market(2, Side::Sell, 10, trades));
    CHECK(trades.empty());
    CHECK_EQ(book.order_count(), 0u);
    CHECK(book.snapshot() == BookSnapshot{});
}

BOOK_TEST(market_order_on_thin_book_discards_remainder) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 3, trades);
    book.add_limit(2, Side::Sell, 105, 2, trades);
    CHECK(book.add_market(3, Side::Buy, 10, trades));

    const std::vector<Trade> expected{{3, 1, 100, 3}, {3, 2, 105, 2}};
    CHECK(trades == expected);
    CHECK_EQ(book.order_count(), 0u);
    CHECK(!book.best_bid().has_value());  // the unfilled 5 did not rest
}

BOOK_TEST(market_sell_partially_consumes_level) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Buy, 100, 10, trades);
    book.add_limit(2, Side::Buy, 99, 10, trades);
    CHECK(book.add_market(3, Side::Sell, 4, trades));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0] == (Trade{3, 1, 100, 4}));
    CHECK_EQ(book.volume_at(Side::Buy, 100), 6);
}

// --- Validation ----------------------------------------------------------------

BOOK_TEST(invalid_adds_are_rejected) {
    Book book;
    std::vector<Trade> trades;
    CHECK(!book.add_limit(1, Side::Buy, 100, 0, trades));
    CHECK(!book.add_limit(1, Side::Buy, 100, -5, trades));
    CHECK(!book.add_market(1, Side::Buy, 0, trades));
    CHECK(book.add_limit(1, Side::Buy, 100, 5, trades));
    CHECK(!book.add_limit(1, Side::Sell, 200, 5, trades));  // duplicate live id
    CHECK_EQ(book.order_count(), 1u);
    CHECK(!book.best_ask().has_value());
}

BOOK_TEST(id_can_be_reused_after_order_is_gone) {
    Book book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Buy, 100, 5, trades);
    book.cancel(1);
    CHECK(book.add_limit(1, Side::Sell, 101, 5, trades));
    CHECK_EQ(book.best_ask(), std::optional<lob::Price>{101});
}

BOOK_TEST(trades_are_appended_not_overwritten) {
    Book book;
    std::vector<Trade> trades{{99, 98, 1, 1}};
    book.add_limit(1, Side::Sell, 100, 5, trades);
    book.add_limit(2, Side::Buy, 100, 5, trades);
    CHECK_EQ(trades.size(), 2u);
}

int main() { return testing::run_all(); }
