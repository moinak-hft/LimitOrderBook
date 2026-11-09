#pragma once

#include <cstdint>

using std::uint64_t;

// Participant identities and order identities are separate. Execution occurs at
// the resting maker price; time is the incoming event time (or event sequence).
struct Trade {
    uint64_t id;
    uint64_t sellerId;
    uint64_t buyerId;
    uint64_t instrumentId;
    uint64_t quantity;
    uint64_t price;
    uint64_t time;
    uint64_t sellerOrderId;
    uint64_t buyerOrderId;
    uint64_t makerOrderId;
    uint64_t takerOrderId;
};
