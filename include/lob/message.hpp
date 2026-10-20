#pragma once

#include <cstdint>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob {

enum class MessageType : std::uint8_t { AddLimit, AddMarket, Cancel, Modify };

// One instruction to the book. Fields that a message type does not use
// (e.g. price for a cancel) are ignored.
struct Message {
    MessageType type;
    Side side;
    OrderId id;
    Price price;
    Quantity quantity;
};

// Routes a message to the matching OrderBook call and returns its result.
template <OrderBook Book>
bool apply(Book& book, const Message& message, std::vector<Trade>& trades) {
    switch (message.type) {
        case MessageType::AddLimit:
            return book.add_limit(message.id, message.side, message.price, message.quantity, trades);
        case MessageType::AddMarket:
            return book.add_market(message.id, message.side, message.quantity, trades);
        case MessageType::Cancel:
            return book.cancel(message.id);
        case MessageType::Modify:
            return book.modify(message.id, message.price, message.quantity, trades);
    }
    return false;
}

}  // namespace lob
