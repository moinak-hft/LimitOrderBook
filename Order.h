//
// Created by Oscar Zhang on 2026/2/22.
//

#pragma once

#include <cstdint>
#include <chrono>

using std::uint64_t;

enum class Side : std::uint8_t {
    ASK,
    BID
};

struct Order {
    uint64_t id;
    uint64_t quantity;
    uint64_t price;
    uint64_t instrumentId;
    uint64_t traderId;
    uint64_t time;
    Side side;

    std::uint8_t _padding[15] = {0};

    Order(uint64_t id, uint64_t quantity, uint64_t price, uint64_t instrumentId, uint64_t traderId, Side side) :
        id(id), quantity(quantity), price(price), instrumentId(instrumentId), traderId(traderId), side(side) {
        time = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
};