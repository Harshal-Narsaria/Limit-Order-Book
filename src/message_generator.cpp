#include "lob/message_generator.hpp"

#include <algorithm>
#include <optional>
#include <random>

#include "lob/map_order_book.hpp"

namespace lob {
namespace {

class Generator {
public:
    explicit Generator(const GeneratorConfig& config)
        : config_(config), rng_(config.seed), mid_(config.start_mid) {}

    std::vector<Message> run() {
        std::vector<Message> messages;
        messages.reserve(config_.message_count);
        for (std::size_t i = 0; i < config_.message_count; ++i) {
            step_mid();
            const Message message = next_message();

            // Feed a shadow book so we know which orders are still resting.
            // That lets cancels and modifies target live orders, like real flow.
            trades_.clear();
            apply(shadow_, message, trades_);
            if (message.type == MessageType::AddLimit && shadow_.contains(message.id)) {
                live_.push_back({message.id, message.side, message.price});
            }
            messages.push_back(message);
        }
        return messages;
    }

private:
    struct LiveOrder {
        OrderId id;
        Side side;
        Price price;
    };

    // Uniform integer in [0, n). The modulo bias is negligible for our small n.
    std::int64_t uniform(std::int64_t n) {
        return static_cast<std::int64_t>(rng_() % static_cast<std::uint64_t>(n));
    }
    bool percent_chance(int percent) { return uniform(100) < percent; }
    Side random_side() { return uniform(2) == 0 ? Side::Buy : Side::Sell; }

    void step_mid() {
        if (uniform(100) == 0) {  // the mid moves one tick about once per 100 messages
            mid_ += uniform(2) == 0 ? 1 : -1;
            mid_ = std::clamp(mid_, config_.min_mid, config_.max_mid);
        }
    }

    Message next_message() {
        std::int64_t roll = uniform(100);
        if (roll < config_.add_percent) {
            return passive_add();
        }
        roll -= config_.add_percent;
        if (roll < config_.cancel_percent) {
            return cancel();
        }
        roll -= config_.cancel_percent;
        if (roll < config_.modify_percent) {
            return modify();
        }
        return aggressive();
    }

    // Distance from the mid, skewed towards 0: the product of two uniforms
    // puts most orders right at the touch and a thin tail deeper in the book.
    Price passive_offset() {
        const Price max = config_.max_passive_offset;
        return uniform(max + 1) * uniform(max + 1) / max;
    }

    Message passive_add() {
        const Side side = random_side();
        const Price offset = passive_offset();
        const Price price = side == Side::Buy ? mid_ - offset : mid_ + 1 + offset;
        const Quantity quantity = 1 + uniform(config_.max_passive_quantity);
        return {MessageType::AddLimit, side, next_id_++, price, quantity};
    }

    Message aggressive() {
        const Side side = random_side();
        const Quantity quantity = 1 + uniform(config_.max_aggressive_quantity);
        if (percent_chance(config_.market_percent_of_aggressive)) {
            return {MessageType::AddMarket, side, next_id_++, 0, quantity};
        }
        // A limit order priced through the touch; any remainder rests.
        const Price price = side == Side::Buy ? mid_ + 1 + uniform(3) : mid_ - uniform(3);
        return {MessageType::AddLimit, side, next_id_++, price, quantity};
    }

    Message cancel() {
        // A small share of cancels target an id that may already be gone,
        // as happens when a cancel races a fill.
        if (percent_chance(5)) {
            const auto id = static_cast<OrderId>(1 + uniform(static_cast<std::int64_t>(next_id_)));
            return {MessageType::Cancel, Side::Buy, id, 0, 0};
        }
        const std::optional<std::size_t> index = pick_live();
        if (!index) {
            return passive_add();
        }
        return {MessageType::Cancel, live_[*index].side, live_[*index].id, 0, 0};
    }

    Message modify() {
        const std::optional<std::size_t> index = pick_live();
        if (!index) {
            return passive_add();
        }
        LiveOrder& order = live_[*index];
        const Quantity quantity = 1 + uniform(config_.max_passive_quantity);
        // Half keep their price (quantity up or down), half move by a few ticks.
        if (uniform(2) == 1) {
            order.price += uniform(2) == 0 ? -(1 + uniform(3)) : 1 + uniform(3);
        }
        return {MessageType::Modify, order.side, order.id, order.price, quantity};
    }

    // Picks a random live order, lazily dropping entries that have since been
    // filled or cancelled.
    std::optional<std::size_t> pick_live() {
        while (!live_.empty()) {
            const auto index = static_cast<std::size_t>(uniform(static_cast<std::int64_t>(live_.size())));
            if (shadow_.contains(live_[index].id)) {
                return index;
            }
            live_[index] = live_.back();
            live_.pop_back();
        }
        return std::nullopt;
    }

    GeneratorConfig config_;
    std::mt19937_64 rng_;
    Price mid_;
    OrderId next_id_ = 1;
    MapOrderBook shadow_;
    std::vector<Trade> trades_;
    std::vector<LiveOrder> live_;
};

}  // namespace

std::vector<Message> generate_messages(const GeneratorConfig& config) {
    return Generator(config).run();
}

}  // namespace lob
