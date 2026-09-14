//
// Created by Oscar Zhang on 2026/2/23.
//

#pragma once
#include <map>
#include <list>
#include <vector>
#include <unordered_map>
#include "Order.h"
#include "Trade.h"

using std::uint64_t;

class OrderBook {
    std::map<uint64_t, std::list<Order>> asks;
    std::map<uint64_t, std::list<Order>, std::greater<>> bids;
    std::unordered_map<uint64_t, std::list<Order>::iterator> orders;
    std::vector<Trade> trades;
    uint64_t nextTradeId = 0;

public:
    OrderBook() {
        trades.reserve(1000000);
    }

    void addOrder(const Order& incomingOrder) {
        uint64_t remainingQty = incomingOrder.quantity;

        if (incomingOrder.side == Side::ASK) {
            while (!bids.empty() && remainingQty > 0 && bids.begin()->first >= incomingOrder.price) {
                auto& bestBidList = bids.begin()->second;
                auto& restingOrder = bestBidList.front(); // Pass by reference

                if (restingOrder.quantity <= remainingQty) {
                    uint64_t fillQty = restingOrder.quantity;
                    remainingQty -= fillQty;
                    completeExchange(restingOrder, incomingOrder, fillQty);
                    deleteOrder(restingOrder.id);
                } else {
                    restingOrder.quantity -= remainingQty;
                    completeExchange(restingOrder, incomingOrder, remainingQty);
                    remainingQty = 0;
                }
            }
            if (remainingQty > 0) {
                Order restingRemainder = incomingOrder;
                restingRemainder.quantity = remainingQty;
                auto& orderList = asks[incomingOrder.price];
                auto it = orderList.insert(orderList.end(), restingRemainder);
                orders[incomingOrder.id] = it;
            }
        } else {
            while (!asks.empty() && remainingQty > 0 && asks.begin()->first <= incomingOrder.price) {
                auto& bestAskList = asks.begin()->second;
                auto& restingOrder = bestAskList.front(); // Pass by reference

                if (restingOrder.quantity <= remainingQty) {
                    uint64_t fillQty = restingOrder.quantity;
                    remainingQty -= fillQty;
                    completeExchange(restingOrder, incomingOrder, fillQty);
                    deleteOrder(restingOrder.id);
                } else {
                    restingOrder.quantity -= remainingQty;
                    completeExchange(restingOrder, incomingOrder, remainingQty);
                    remainingQty = 0;
                }
            }
            if (remainingQty > 0) {
                Order restingRemainder = incomingOrder;
                restingRemainder.quantity = remainingQty;
                auto& orderList = bids[incomingOrder.price];
                auto it = orderList.insert(orderList.end(), restingRemainder);
                orders[incomingOrder.id] = it;
            }
        }
    }

    void completeExchange(const Order& restingOrder, const Order& incomingOrder, uint64_t quantity) {
        uint64_t sellerId = restingOrder.side == Side::ASK ? restingOrder.id : incomingOrder.id;
        uint64_t buyerId = restingOrder.side == Side::BID ? restingOrder.id : incomingOrder.id;

        trades.emplace_back(nextTradeId++, quantity, restingOrder.price, restingOrder.instrumentId, buyerId, sellerId);
    }

    void deleteOrder(uint64_t orderId) {
        auto mapIter = orders.find(orderId);
        if (mapIter == orders.end()) {
            return;
        }
        auto listIter = mapIter->second;
        uint64_t price = listIter->price;
        Side side = listIter->side;

        if (side == Side::ASK) {
            auto& orderList = asks[price];
            orderList.erase(listIter);
            if (orderList.empty()) asks.erase(price);
        } else {
            auto& orderList = bids[price];
            orderList.erase(listIter);
            if (orderList.empty()) bids.erase(price);
        }
        orders.erase(mapIter);
    }
};