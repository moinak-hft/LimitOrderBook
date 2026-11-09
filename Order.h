#pragma once

#include <cstdint>

using std::uint64_t;

enum class Side : std::uint8_t { ASK, BID };
enum class OrderType : std::uint8_t { Limit, Market };
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK, PostOnly };

// Prices and quantities are integer ticks/lots. The caller supplies event time;
// matching never reads a wall clock. An omitted time is deterministically zero.
struct Order {
    uint64_t id;
    uint64_t quantity;
    uint64_t price;
    uint64_t instrumentId;
    uint64_t traderId;
    uint64_t time;
    Side side;
    OrderType type;
    TimeInForce tif;

    Order(uint64_t id, uint64_t quantity, uint64_t price,
          uint64_t instrumentId, uint64_t traderId, Side side,
          uint64_t time = 0, OrderType type = OrderType::Limit,
          TimeInForce tif = TimeInForce::GTC)
        : id(id), quantity(quantity), price(price), instrumentId(instrumentId),
          traderId(traderId), time(time), side(side), type(type), tif(tif) {}
};
