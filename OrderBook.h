#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Order.h"
#include "Trade.h"

struct SubmitResult {
    bool accepted;
    uint64_t filled;
    // Unfilled quantity: it may rest (GTC/PostOnly) or expire (IOC/Market).
    uint64_t remaining;
    std::string_view reason;
};

struct PriceLevel {
    uint64_t price;
    uint64_t quantity;
    std::size_t orders;
};

struct BookSnapshot {
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    uint64_t sequence;
    std::size_t activeOrders;
};

// A book belongs to exactly one instrument and one owning thread. Independent
// books can execute concurrently; shared mutation of one book needs a sequencer.
class OrderBook {
    using Queue = std::pmr::list<Order>;

    struct Level {
        Queue queue;
        uint64_t quantity = 0;
        explicit Level(std::pmr::memory_resource* resource) : queue(resource) {}
    };

    struct PriceCompare {
        bool descending = false;
        bool operator()(uint64_t left, uint64_t right) const noexcept {
            return descending ? left > right : left < right;
        }
    };

    using Ladder = std::pmr::map<uint64_t, Level, PriceCompare>;
    struct Location {
        Ladder::iterator level;
        Queue::iterator order;
    };
    using Index = std::pmr::unordered_map<uint64_t, Location>;

    // Resource outlives every container that uses it. Freed nodes are recycled.
    std::pmr::unsynchronized_pool_resource pool_;
    Ladder asks_;
    Ladder bids_;
    Index index_;
    std::vector<Trade> trades_;
    uint64_t instrumentId_;
    bool recordTrades_;
    uint64_t sequence_ = 0;
    uint64_t tradeCount_ = 0;
    uint64_t tradedQuantity_ = 0;
    uint64_t tradeDigest_ = 14695981039346656037ULL;

    static void mix(uint64_t& digest, uint64_t value) noexcept {
        digest ^= value;
        digest *= 1099511628211ULL;
    }

    static bool crosses(const Order& incoming, uint64_t oppositePrice) noexcept {
        return incoming.type == OrderType::Market ||
               (incoming.side == Side::BID ? oppositePrice <= incoming.price
                                          : oppositePrice >= incoming.price);
    }

    const Ladder& opposite(const Order& order) const noexcept {
        return order.side == Side::BID ? asks_ : bids_;
    }

    uint64_t available(const Order& order) const noexcept {
        uint64_t required = order.quantity;
        for (const auto& [price, level] : opposite(order)) {
            if (!crosses(order, price)) break;
            if (level.quantity >= required) return order.quantity;
            required -= level.quantity;
        }
        return order.quantity - required;
    }

    void exchange(const Order& maker, const Order& taker, uint64_t quantity) {
        const Order& buyer = maker.side == Side::BID ? maker : taker;
        const Order& seller = maker.side == Side::ASK ? maker : taker;
        const Trade trade{tradeCount_, seller.traderId, buyer.traderId,
                          instrumentId_, quantity, maker.price,
                          taker.time != 0 ? taker.time : sequence_,
                          seller.id, buyer.id, maker.id, taker.id};
        if (recordTrades_) trades_.push_back(trade);
        ++tradeCount_;
        // This cumulative telemetry saturates rather than wrapping on overflow.
        tradedQuantity_ = quantity > std::numeric_limits<uint64_t>::max() - tradedQuantity_
                              ? std::numeric_limits<uint64_t>::max()
                              : tradedQuantity_ + quantity;
        mix(tradeDigest_, trade.id);
        mix(tradeDigest_, trade.price);
        mix(tradeDigest_, trade.quantity);
        mix(tradeDigest_, trade.buyerId);
        mix(tradeDigest_, trade.sellerId);
        mix(tradeDigest_, trade.makerOrderId);
        mix(tradeDigest_, trade.takerOrderId);
        mix(tradeDigest_, trade.time);
    }

    void eraseLocated(Index::iterator location) {
        const auto level = location->second.level;
        const auto node = location->second.order;
        auto& ladder = node->side == Side::BID ? bids_ : asks_;
        level->second.quantity -= node->quantity;
        level->second.queue.erase(node);
        if (level->second.queue.empty()) ladder.erase(level);
        index_.erase(location);
    }

    uint64_t match(const Order& incoming, Ladder& ladder) {
        uint64_t remaining = incoming.quantity;
        while (remaining != 0 && !ladder.empty() &&
               crosses(incoming, ladder.begin()->first)) {
            const auto level = ladder.begin();
            auto& queue = level->second.queue;
            auto& maker = queue.front();
            const uint64_t fill = std::min(remaining, maker.quantity);
            exchange(maker, incoming, fill);
            remaining -= fill;
            level->second.quantity -= fill;
            maker.quantity -= fill;
            if (maker.quantity == 0) {
                index_.erase(maker.id);
                queue.pop_front();
                if (queue.empty()) ladder.erase(level);
            }
        }
        return remaining;
    }

    void rest(const Order& incoming, uint64_t remaining) {
        auto& ladder = incoming.side == Side::BID ? bids_ : asks_;
        auto level = ladder.try_emplace(incoming.price, &pool_).first;
        auto& queue = level->second.queue;
        queue.push_back(incoming);
        auto node = std::prev(queue.end());
        node->quantity = remaining;
        index_.emplace(incoming.id, Location{level, node});
        level->second.quantity += remaining;
    }

public:
    explicit OrderBook(uint64_t instrumentId = 1, bool recordTrades = true,
                       std::size_t reserveOrders = 4096)
        : asks_(PriceCompare{false}, &pool_), bids_(PriceCompare{true}, &pool_),
          index_(&pool_), instrumentId_(instrumentId), recordTrades_(recordTrades) {
        index_.reserve(reserveOrders);
        if (recordTrades_) trades_.reserve(reserveOrders);
    }

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    SubmitResult addOrder(const Order& incoming) {
        const auto reject = [&](std::string_view reason) {
            return SubmitResult{false, 0, incoming.quantity, reason};
        };
        if (incoming.side != Side::ASK && incoming.side != Side::BID)
            return reject("invalid side");
        if (incoming.type != OrderType::Limit && incoming.type != OrderType::Market)
            return reject("invalid order type");
        if (incoming.tif != TimeInForce::GTC && incoming.tif != TimeInForce::IOC &&
            incoming.tif != TimeInForce::FOK && incoming.tif != TimeInForce::PostOnly)
            return reject("invalid time in force");
        if (incoming.quantity == 0) return reject("quantity must be positive");
        if (incoming.instrumentId != instrumentId_) return reject("wrong instrument");
        if (incoming.type == OrderType::Limit && incoming.price == 0)
            return reject("limit price must be positive");
        if (index_.contains(incoming.id)) return reject("duplicate active order id");
        if (incoming.type == OrderType::Market && incoming.tif == TimeInForce::PostOnly)
            return reject("market cannot be post-only");
        const auto& other = opposite(incoming);
        if (incoming.tif == TimeInForce::PostOnly && !other.empty() &&
            crosses(incoming, other.begin()->first))
            return reject("post-only would cross");
        if (incoming.tif == TimeInForce::FOK && available(incoming) < incoming.quantity)
            return reject("insufficient FOK liquidity");

        const bool canRest = incoming.type == OrderType::Limit &&
                             (incoming.tif == TimeInForce::GTC ||
                              incoming.tif == TimeInForce::PostOnly);
        if (canRest) {
            const auto& own = incoming.side == Side::BID ? bids_ : asks_;
            const auto level = own.find(incoming.price);
            if (level != own.end() &&
                incoming.quantity > std::numeric_limits<uint64_t>::max() - level->second.quantity) {
                const uint64_t remainder = incoming.quantity - available(incoming);
                if (remainder > std::numeric_limits<uint64_t>::max() - level->second.quantity)
                    return reject("price level quantity overflow");
            }
        }

        ++sequence_;
        const uint64_t remaining = match(incoming, incoming.side == Side::BID ? asks_ : bids_);
        if (remaining != 0 && canRest) rest(incoming, remaining);
        return {true, incoming.quantity - remaining, remaining, "accepted"};
    }

    // Hash lookup + stored queue/level iterators. Expected O(1); empty level
    // removal uses map::erase(iterator), with amortized constant complexity.
    bool deleteOrder(uint64_t id) {
        const auto location = index_.find(id);
        if (location == index_.end()) return false;
        eraseLocated(location);
        ++sequence_;
        return true;
    }

    // A size reduction preserves FIFO position. A price change or size increase
    // is explicitly cancel + submit with a new identity and therefore loses it.
    bool reduceOrder(uint64_t id, uint64_t newQuantity) {
        const auto location = index_.find(id);
        if (location == index_.end() || newQuantity > location->second.order->quantity)
            return false;
        if (newQuantity == 0) return deleteOrder(id);
        auto& node = *location->second.order;
        location->second.level->second.quantity -= node.quantity - newQuantity;
        node.quantity = newQuantity;
        ++sequence_;
        return true;
    }

    const std::vector<Trade>& trades() const noexcept { return trades_; }
    void reserveTrades(std::size_t count) { if (recordTrades_) trades_.reserve(count); }
    // Clearing retained records does not reset cumulative telemetry/digest.
    void clearTrades() noexcept { trades_.clear(); }
    std::size_t activeOrderCount() const noexcept { return index_.size(); }
    uint64_t tradeCount() const noexcept { return tradeCount_; }
    uint64_t tradedQuantity() const noexcept { return tradedQuantity_; }
    uint64_t sequence() const noexcept { return sequence_; }
    uint64_t instrumentId() const noexcept { return instrumentId_; }

    std::optional<uint64_t> bestBid() const noexcept {
        return bids_.empty() ? std::nullopt : std::optional<uint64_t>{bids_.begin()->first};
    }
    std::optional<uint64_t> bestAsk() const noexcept {
        return asks_.empty() ? std::nullopt : std::optional<uint64_t>{asks_.begin()->first};
    }
    std::optional<Order> order(uint64_t id) const {
        const auto location = index_.find(id);
        if (location == index_.end()) return std::nullopt;
        return *location->second.order;
    }

    BookSnapshot snapshot(std::size_t depth = 10) const {
        BookSnapshot result{{}, {}, sequence_, index_.size()};
        const auto append = [depth](const Ladder& ladder, std::vector<PriceLevel>& output) {
            output.reserve(std::min(depth, ladder.size()));
            for (const auto& [price, level] : ladder) {
                if (output.size() == depth) break;
                output.push_back({price, level.quantity, level.queue.size()});
            }
        };
        append(bids_, result.bids);
        append(asks_, result.asks);
        return result;
    }

    // Stable across trade-retention modes, hash table layouts and processes.
    // This is a reproducibility fingerprint, not a cryptographic commitment.
    uint64_t checksum() const noexcept {
        uint64_t digest = 14695981039346656037ULL;
        mix(digest, instrumentId_);
        mix(digest, sequence_);
        mix(digest, tradeCount_);
        mix(digest, tradedQuantity_);
        mix(digest, tradeDigest_);
        for (const auto* ladder : {&bids_, &asks_}) {
            mix(digest, ladder->size());
            for (const auto& [price, level] : *ladder) {
                mix(digest, price);
                mix(digest, level.quantity);
                mix(digest, level.queue.size());
                for (const auto& order : level.queue) {
                    mix(digest, order.id);
                    mix(digest, order.quantity);
                    mix(digest, order.price);
                    mix(digest, order.instrumentId);
                    mix(digest, order.traderId);
                    mix(digest, order.time);
                    mix(digest, static_cast<uint64_t>(order.side));
                    mix(digest, static_cast<uint64_t>(order.type));
                    mix(digest, static_cast<uint64_t>(order.tif));
                }
            }
        }
        return digest;
    }

    bool validate() const noexcept {
        if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first)
            return false;
        std::size_t totalOrders = 0;
        const auto check = [&](const Ladder& ladder, Side side) {
            for (auto level = ladder.begin(); level != ladder.end(); ++level) {
                if (level->second.queue.empty()) return false;
                uint64_t quantity = 0;
                for (auto node = level->second.queue.begin(); node != level->second.queue.end(); ++node) {
                    if (node->side != side || node->instrumentId != instrumentId_ ||
                        node->price != level->first || node->quantity == 0 ||
                        node->type != OrderType::Limit ||
                        (node->tif != TimeInForce::GTC && node->tif != TimeInForce::PostOnly) ||
                        node->quantity > std::numeric_limits<uint64_t>::max() - quantity)
                        return false;
                    const auto location = index_.find(node->id);
                    if (location == index_.end() || location->second.level != level ||
                        location->second.order != node)
                        return false;
                    quantity += node->quantity;
                    ++totalOrders;
                }
                if (quantity != level->second.quantity) return false;
            }
            return true;
        };
        return check(bids_, Side::BID) && check(asks_, Side::ASK) && totalOrders == index_.size();
    }
};
