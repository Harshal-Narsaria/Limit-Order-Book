// Randomised differential test.
//
// The baseline MapOrderBook is simple enough to trust after its unit tests.
// Here we feed the same long random message stream to both books and require
// that they agree on every return value, every trade (in order), and the full
// book state (every level, every order, in queue order).

#include <cstdint>
#include <iostream>
#include <vector>

#include "lob/flat_order_book.hpp"
#include "lob/map_order_book.hpp"
#include "lob/message.hpp"
#include "lob/message_generator.hpp"
#include "test_framework.hpp"

namespace {

void run_differential(const lob::GeneratorConfig& config) {
    const std::vector<lob::Message> messages = lob::generate_messages(config);

    lob::MapOrderBook map_book;
    lob::FlatOrderBook flat_book({.min_price = 0,
                                  .max_price = 20'000,
                                  .max_orders = 1 << 20,
                                  .max_order_id = config.message_count + 2});

    std::vector<lob::Trade> map_trades;
    std::vector<lob::Trade> flat_trades;
    std::size_t trade_count = 0;
    std::size_t rejected_count = 0;
    std::size_t max_resting = 0;

    for (std::size_t i = 0; i < messages.size(); ++i) {
        map_trades.clear();
        flat_trades.clear();
        const bool map_ok = lob::apply(map_book, messages[i], map_trades);
        const bool flat_ok = lob::apply(flat_book, messages[i], flat_trades);

        if (map_ok != flat_ok || map_trades != flat_trades) {
            std::cerr << "  divergence at message " << i << " (id " << messages[i].id << ")\n";
            REQUIRE(map_ok == flat_ok);
            REQUIRE(map_trades == flat_trades);
        }
        trade_count += map_trades.size();
        rejected_count += map_ok ? 0 : 1;
        max_resting = std::max(max_resting, map_book.order_count());

        // Cheap checks often, the full (slow) snapshot comparison occasionally.
        if (i % 1'000 == 0) {
            REQUIRE(map_book.best_bid() == flat_book.best_bid());
            REQUIRE(map_book.best_ask() == flat_book.best_ask());
            REQUIRE(map_book.order_count() == flat_book.order_count());
        }
        if (i % 50'000 == 0) {
            REQUIRE(map_book.snapshot() == flat_book.snapshot());
        }
    }
    REQUIRE(map_book.snapshot() == flat_book.snapshot());

    std::cout << "  seed " << config.seed << ": " << messages.size() << " messages, " << trade_count
              << " trades, " << rejected_count << " rejected (unknown-id cancels), "
              << map_book.order_count() << " resting at end, peak " << max_resting << '\n';
    CHECK(trade_count > 0);
}

}  // namespace

TEST_CASE("differential: adds, cancels, modifies and aggressive flow (3 seeds)") {
    for (std::uint64_t seed : {1u, 2u, 3u}) {
        run_differential({.seed = seed,
                          .message_count = 300'000,
                          .add_percent = 50,
                          .cancel_percent = 25,
                          .modify_percent = 15});
    }
}

TEST_CASE("differential: benchmark message mix") {
    run_differential({.seed = 42, .message_count = 300'000});
}

TEST_CASE("differential: aggressive-heavy flow that often empties the book") {
    run_differential({.seed = 7,
                      .message_count = 200'000,
                      .add_percent = 40,
                      .cancel_percent = 20,
                      .modify_percent = 10,
                      .market_percent_of_aggressive = 70,
                      .max_passive_offset = 5});
}

int main() { return testing::run_all(); }
