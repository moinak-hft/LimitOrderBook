//
// Created by Oscar Zhang on 2026/2/28.
//

#pragma once
#include <chrono>
#include <cstdint>

using std::uint64_t;

struct alignas(64) Trade {
    uint64_t id;
    uint64_t sellerId;
    uint64_t buyerId;
    uint64_t instrumentId;
    uint64_t quantity;
    uint64_t price;
    uint64_t time;
    Trade(uint64_t id, uint64_t quantity, uint64_t price, uint64_t instrumentId, uint64_t buyerId, uint64_t sellerId) :
        id(id), sellerId(sellerId), buyerId(buyerId), instrumentId(instrumentId), quantity(quantity), price(price) {
        time = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
};