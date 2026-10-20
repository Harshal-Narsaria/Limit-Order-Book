#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/message.hpp"
#include "lob/types.hpp"

namespace lob {

// Parameters for a synthetic but plausible order flow.
//
// The mid price does a slow random walk. Passive adds cluster near the mid
// (most within a few ticks, a thin tail further out). Aggressive flow is a mix
// of market orders and marketable limit orders that cross the spread.
struct GeneratorConfig {
    std::uint64_t seed = 1;
    std::size_t message_count = 1'000'000;

    // Message mix in percent. Whatever is left over is aggressive flow.
    int add_percent = 60;
    int cancel_percent = 30;
    int modify_percent = 0;
    int market_percent_of_aggressive = 50;  // the rest are marketable limits

    Price start_mid = 10'000;
    Price min_mid = 1'000;
    Price max_mid = 19'000;
    Price max_passive_offset = 20;  // passive orders rest within this many ticks of the mid

    Quantity max_passive_quantity = 100;
    Quantity max_aggressive_quantity = 300;
};

// Deterministic for a given config: same seed, same messages, on any platform
// (we avoid std::uniform_int_distribution, whose output differs between
// standard library implementations).
std::vector<Message> generate_messages(const GeneratorConfig& config);

}  // namespace lob
