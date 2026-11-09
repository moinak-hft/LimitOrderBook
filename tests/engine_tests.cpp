#include "OrderBook.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Order limit(uint64_t id, uint64_t quantity, uint64_t price, Side side,
            uint64_t trader = 100, TimeInForce tif = TimeInForce::GTC) {
    return Order(id, quantity, price, 1, trader, side, id * 10, OrderType::Limit, tif);
}

Order market(uint64_t id, uint64_t quantity, Side side,
             TimeInForce tif = TimeInForce::IOC) {
    return Order(id, quantity, 0, 1, 999, side, id * 10, OrderType::Market, tif);
}

void fifoAndMakerPrice() {
    OrderBook book;
    book.addOrder(limit(1, 5, 101, Side::ASK, 71));
    book.addOrder(limit(2, 7, 101, Side::ASK, 72));
    book.addOrder(limit(3, 8, 102, Side::ASK, 73));
    const auto result = book.addOrder(limit(4, 15, 105, Side::BID, 81));
    require(result.accepted && result.filled == 15 && result.remaining == 0, "sweep fill result");
    const auto& trades = book.trades();
    require(trades.size() == 3, "sweep produces three fills");
    require(trades[0].makerOrderId == 1 && trades[0].quantity == 5 && trades[0].price == 101, "first FIFO fill");
    require(trades[1].makerOrderId == 2 && trades[1].quantity == 7 && trades[1].price == 101, "second FIFO fill");
    require(trades[2].makerOrderId == 3 && trades[2].quantity == 3 && trades[2].price == 102, "next price fill");
    require(trades[0].sellerId == 71 && trades[0].buyerId == 81, "trader IDs are not order IDs");
    require(trades[0].sellerOrderId == 1 && trades[0].buyerOrderId == 4 && trades[0].takerOrderId == 4, "order identity in fill");
    require(book.order(3).has_value() && book.order(3)->quantity == 5, "maker partial remainder");
    require(book.bestAsk() == 102 && !book.bestBid(), "top of book after sweep");
    require(book.tradeCount() == 3 && book.tradedQuantity() == 15 && book.activeOrderCount() == 1, "cumulative counters");
    require(book.validate(), "invariants after sweep");

    OrderBook sellSweep;
    sellSweep.addOrder(limit(11, 3, 100, Side::BID, 45));
    sellSweep.addOrder(limit(12, 4, 99, Side::BID, 46));
    sellSweep.addOrder(limit(13, 5, 97, Side::ASK, 47));
    require(sellSweep.trades().size() == 2 && sellSweep.trades()[0].price == 100 && sellSweep.trades()[1].price == 99, "descending bid sweep");
    require(sellSweep.trades()[0].buyerId == 45 && sellSweep.trades()[0].sellerId == 47, "sell taker identity");
    require(sellSweep.order(12)->quantity == 2 && sellSweep.validate(), "bid partial remainder");
}

void cancellationAndReduction() {
    OrderBook book;
    book.addOrder(limit(1, 10, 100, Side::BID));
    book.addOrder(limit(2, 10, 100, Side::BID));
    book.addOrder(limit(3, 10, 99, Side::BID));
    require(book.deleteOrder(2), "cancel interior FIFO node");
    require(!book.deleteOrder(2) && !book.deleteOrder(999), "cancel absent order");
    require(book.reduceOrder(1, 4), "reduce quantity");
    require(!book.reduceOrder(1, 5), "increase rejected");
    require(!book.reduceOrder(999, 1), "reduce absent order");
    book.addOrder(limit(4, 7, 100, Side::BID));
    book.addOrder(limit(5, 6, 100, Side::ASK));
    require(book.trades().size() == 2 && book.trades()[0].makerOrderId == 1 && book.trades()[0].quantity == 4, "reduction preserves time priority");
    require(book.order(4)->quantity == 5, "second maker remainder");
    require(book.reduceOrder(4, 0), "zero reduction cancels");
    require(book.bestBid() == 99, "empty best level removed");
    require(book.deleteOrder(3) && !book.bestBid() && book.activeOrderCount() == 0, "last level removed");
    require(book.validate(), "invariants after cancellations");
}

void rejectedRequestsAreAtomic() {
    OrderBook book;
    book.addOrder(limit(1, 8, 100, Side::BID));
    const auto unchanged = [&](Order invalid, const char* label) {
        const auto before = book.checksum();
        const auto result = book.addOrder(invalid);
        require(!result.accepted, std::string(label) + " must reject");
        require(book.checksum() == before, std::string(label) + " mutated state");
        require(book.activeOrderCount() == 1 && book.trades().empty() && book.validate(), std::string(label) + " broke invariants");
    };
    unchanged(limit(1, 2, 99, Side::ASK), "duplicate active ID");
    unchanged(limit(2, 0, 99, Side::ASK), "zero quantity");
    unchanged(limit(2, 1, 0, Side::ASK), "zero limit price");
    auto wrongInstrument = limit(2, 1, 99, Side::ASK);
    wrongInstrument.instrumentId = 2;
    unchanged(wrongInstrument, "wrong instrument");
    auto wrongSide = limit(2, 1, 99, Side::ASK);
    wrongSide.side = static_cast<Side>(99);
    unchanged(wrongSide, "invalid side");
    auto wrongType = limit(2, 1, 99, Side::ASK);
    wrongType.type = static_cast<OrderType>(99);
    unchanged(wrongType, "invalid type");
    auto wrongTif = limit(2, 1, 99, Side::ASK);
    wrongTif.tif = static_cast<TimeInForce>(99);
    unchanged(wrongTif, "invalid time in force");
    unchanged(market(2, 1, Side::ASK, TimeInForce::PostOnly), "post-only market");
    require(book.deleteOrder(1), "remove original");
    require(book.addOrder(limit(1, 3, 102, Side::ASK)).accepted, "ID reusable after cancellation");
    require(book.validate(), "invariants after ID reuse");
}

void immediateAndMarketOrders() {
    OrderBook book;
    book.addOrder(limit(1, 4, 100, Side::ASK));
    book.addOrder(limit(2, 8, 103, Side::ASK));
    const auto ioc = book.addOrder(limit(3, 10, 101, Side::BID, 200, TimeInForce::IOC));
    require(ioc.accepted && ioc.filled == 4 && ioc.remaining == 6, "IOC reports canceled unfilled quantity");
    require(!book.order(3) && book.order(2)->quantity == 8, "IOC remainder never rests");
    const auto fill = book.addOrder(market(4, 20, Side::BID));
    require(fill.accepted && fill.filled == 8 && fill.remaining == 12, "market consumes available liquidity");
    require(!book.order(4) && book.activeOrderCount() == 0, "market remainder never rests");
    require(book.addOrder(market(5, 3, Side::ASK)).accepted, "empty-book market accepted with no fill");
    const auto implicitIoc = book.addOrder(Order(6, 3, 0, 1, 202, Side::BID, 60, OrderType::Market));
    require(implicitIoc.accepted && implicitIoc.filled == 0 && implicitIoc.remaining == 3 && !book.order(6), "market with default GTC expires remainder");
    require(book.tradedQuantity() == 12 && book.validate(), "IOC/market counters and invariants");
}

void aggregateOverflowBoundary() {
    OrderBook book;
    const auto maximum = std::numeric_limits<uint64_t>::max();
    require(book.addOrder(limit(1, maximum, 100, Side::BID)).accepted, "largest individual quantity accepted");
    const auto digest = book.checksum();
    require(!book.addOrder(limit(2, 1, 100, Side::BID)).accepted, "level aggregate overflow rejected");
    require(book.checksum() == digest && book.order(1)->quantity == maximum, "overflow rejection atomic");
    require(book.addOrder(market(3, 1, Side::ASK)).filled == 1, "huge maker can partially fill");
    require(book.addOrder(limit(4, 1, 100, Side::BID)).accepted, "aggregate can reach maximum exactly");
    require(book.snapshot().bids[0].quantity == maximum && book.validate(), "boundary aggregate remains valid");
}

void fillOrKillAndPostOnly() {
    OrderBook book;
    book.addOrder(limit(1, 4, 100, Side::ASK));
    book.addOrder(limit(2, 8, 103, Side::ASK));
    const auto before = book.checksum();
    require(!book.addOrder(limit(3, 10, 101, Side::BID, 200, TimeInForce::FOK)).accepted, "FOK rejects insufficient in-limit liquidity");
    require(book.checksum() == before, "FOK rejection consumed no liquidity");
    const auto fok = book.addOrder(limit(4, 10, 103, Side::BID, 200, TimeInForce::FOK));
    require(fok.accepted && fok.filled == 10 && fok.remaining == 0, "FOK sweeps multiple levels atomically");
    require(book.order(2)->quantity == 2, "FOK partial maker remainder");
    const auto secondBefore = book.checksum();
    require(!book.addOrder(market(5, 3, Side::BID, TimeInForce::FOK)).accepted, "market FOK rejects shortage");
    require(book.checksum() == secondBefore, "market FOK atomic rejection");
    require(!book.addOrder(limit(6, 1, 103, Side::BID, 200, TimeInForce::PostOnly)).accepted, "post-only rejects touching quote");
    require(book.checksum() == secondBefore, "post-only rejection atomic");
    require(book.addOrder(limit(7, 9, 102, Side::BID, 200, TimeInForce::PostOnly)).accepted, "noncrossing post-only rests");
    require(book.order(7).has_value() && book.validate(), "post-only invariant");
    require(book.addOrder(market(8, 2, Side::BID, TimeInForce::FOK)).filled == 2, "market FOK exact fill");

    OrderBook sell;
    sell.addOrder(limit(11, 5, 99, Side::BID));
    require(!sell.addOrder(limit(12, 6, 99, Side::ASK, 30, TimeInForce::FOK)).accepted, "sell FOK shortage");
    require(!sell.addOrder(limit(13, 1, 99, Side::ASK, 30, TimeInForce::PostOnly)).accepted, "sell post-only crossing");
    require(sell.addOrder(limit(14, 5, 99, Side::ASK, 30, TimeInForce::FOK)).filled == 5 && sell.validate(), "sell FOK exact fill");
}

void snapshotsAndIsolation() {
    OrderBook first(1), second(2);
    first.addOrder(limit(1, 5, 98, Side::BID));
    first.addOrder(limit(2, 7, 98, Side::BID));
    first.addOrder(limit(3, 3, 97, Side::BID));
    first.addOrder(limit(4, 9, 101, Side::ASK));
    first.addOrder(limit(5, 2, 102, Side::ASK));
    const auto snapshot = first.snapshot(1);
    require(snapshot.bids.size() == 1 && snapshot.asks.size() == 1, "snapshot depth bound");
    require(snapshot.bids[0].price == 98 && snapshot.bids[0].quantity == 12 && snapshot.bids[0].orders == 2, "aggregated bid depth");
    require(snapshot.asks[0].price == 101 && snapshot.asks[0].quantity == 9 && snapshot.activeOrders == 5, "aggregated ask depth");
    require(first.snapshot(0).bids.empty() && first.snapshot(0).asks.empty(), "zero-depth snapshot");
    require(second.addOrder(Order(1, 20, 90, 2, 50, Side::ASK)).accepted, "other instrument has own book");
    require(first.tradeCount() == 0 && second.tradeCount() == 0 && first.validate() && second.validate(), "instrument books isolated");
}

void deterministicAndRecordingModes() {
    OrderBook left(1, true), right(1, true), noHistory(1, false);
    const std::vector<Order> tape = {
        limit(1, 20, 101, Side::ASK, 11), limit(2, 8, 102, Side::ASK, 12),
        limit(3, 23, 102, Side::BID, 13), limit(4, 7, 99, Side::BID, 14),
        market(5, 4, Side::ASK), limit(6, 5, 100, Side::BID, 15)
    };
    for (const auto& order : tape) {
        left.addOrder(order); right.addOrder(order); noHistory.addOrder(order);
        require(left.checksum() == right.checksum(), "same event tape yields identical state");
    }
    require(left.trades().size() == right.trades().size() && !left.trades().empty(), "deterministic trade history");
    for (std::size_t i = 0; i < left.trades().size(); ++i) {
        const auto& a = left.trades()[i]; const auto& b = right.trades()[i];
        require(a.id == b.id && a.time == b.time && a.quantity == b.quantity && a.price == b.price && a.makerOrderId == b.makerOrderId, "deterministic trade content");
    }
    require(noHistory.trades().empty(), "history disabled");
    require(left.tradeCount() == noHistory.tradeCount() && left.tradedQuantity() == noHistory.tradedQuantity(), "history mode preserves cumulative counters");
    require(left.checksum() == noHistory.checksum(), "history mode preserves execution digest");
    const auto digest = left.checksum(); const auto count = left.tradeCount();
    left.clearTrades();
    require(left.trades().empty() && left.tradeCount() == count && left.checksum() == digest, "clear history preserves cumulative state");
    require(left.validate() && noHistory.validate(), "recording modes invariant");
}

struct ReferenceTrade {
    uint64_t quantity, price, maker, taker, buyer, seller, buyerOrder, sellerOrder;
};

// Intentionally uses a linear scan over arrival-ordered records, independent of
// the engine's trees, queues, hash index, cached aggregates, and memory pool.
struct ReferenceBook {
    std::vector<Order> orders;
    std::vector<ReferenceTrade> trades;
    uint64_t submitted = 0, canceled = 0, traded = 0;

    void add(Order incoming) {
        submitted += incoming.quantity;
        while (incoming.quantity > 0) {
            std::size_t best = orders.size();
            for (std::size_t i = 0; i < orders.size(); ++i) {
                const auto& candidate = orders[i];
                if (candidate.quantity == 0 || candidate.side == incoming.side) continue;
                const bool crosses = incoming.side == Side::BID ? candidate.price <= incoming.price : candidate.price >= incoming.price;
                if (!crosses) continue;
                if (best == orders.size() || (incoming.side == Side::BID ? candidate.price < orders[best].price : candidate.price > orders[best].price)) best = i;
            }
            if (best == orders.size()) break;
            auto& maker = orders[best];
            const auto quantity = std::min(maker.quantity, incoming.quantity);
            const bool buyTaker = incoming.side == Side::BID;
            trades.push_back({quantity, maker.price, maker.id, incoming.id,
                buyTaker ? incoming.traderId : maker.traderId,
                buyTaker ? maker.traderId : incoming.traderId,
                buyTaker ? incoming.id : maker.id,
                buyTaker ? maker.id : incoming.id});
            traded += quantity;
            maker.quantity -= quantity; incoming.quantity -= quantity;
        }
        if (incoming.quantity > 0) orders.push_back(incoming);
    }

    bool cancel(uint64_t id) {
        for (auto& order : orders) if (order.id == id && order.quantity > 0) {
            canceled += order.quantity; order.quantity = 0; return true;
        }
        return false;
    }

    bool reduce(uint64_t id, uint64_t quantity) {
        for (auto& order : orders) if (order.id == id && order.quantity > 0) {
            if (quantity > order.quantity) return false;
            canceled += order.quantity - quantity; order.quantity = quantity; return true;
        }
        return false;
    }
};

void compareReference(const OrderBook& book, const ReferenceBook& reference, std::size_t event) {
    const std::string where = " at random event " + std::to_string(event);
    require(book.validate(), "invariants" + where);
    std::map<uint64_t, std::pair<uint64_t, std::size_t>, std::greater<>> bids;
    std::map<uint64_t, std::pair<uint64_t, std::size_t>> asks;
    std::size_t active = 0;
    uint64_t resting = 0;
    for (const auto& order : reference.orders) if (order.quantity > 0) {
        ++active; resting += order.quantity;
        if (order.side == Side::BID) { bids[order.price].first += order.quantity; ++bids[order.price].second; }
        else { asks[order.price].first += order.quantity; ++asks[order.price].second; }
        const auto engineOrder = book.order(order.id);
        require(engineOrder.has_value() && engineOrder->quantity == order.quantity && engineOrder->price == order.price && engineOrder->side == order.side, "resting order mismatch" + where);
    }
    const auto depth = book.snapshot(10000);
    require(book.activeOrderCount() == active && depth.activeOrders == active, "active order count" + where);
    require(depth.bids.size() == bids.size() && depth.asks.size() == asks.size(), "level count" + where);
    std::size_t index = 0;
    for (const auto& [price, values] : bids) {
        const auto& level = depth.bids[index++];
        require(level.price == price && level.quantity == values.first && level.orders == values.second, "bid depth mismatch" + where);
    }
    index = 0;
    for (const auto& [price, values] : asks) {
        const auto& level = depth.asks[index++];
        require(level.price == price && level.quantity == values.first && level.orders == values.second, "ask depth mismatch" + where);
    }
    require(book.tradeCount() == reference.trades.size() && book.tradedQuantity() == reference.traded, "trade counters" + where);
    require(book.trades().size() == reference.trades.size(), "trade history count" + where);
    if (!reference.trades.empty()) {
        const auto& a = book.trades().back(); const auto& b = reference.trades.back();
        require(a.quantity == b.quantity && a.price == b.price && a.makerOrderId == b.maker && a.takerOrderId == b.taker && a.buyerId == b.buyer && a.sellerId == b.seller && a.buyerOrderId == b.buyerOrder && a.sellerOrderId == b.sellerOrder, "last trade mismatch" + where);
    }
    require(reference.submitted == resting + reference.canceled + 2 * reference.traded, "quantity conservation" + where);
}

void randomizedDifferential() {
    for (const uint64_t seed : {42ULL, 291ULL, 8913ULL}) {
        OrderBook book;
        ReferenceBook reference;
        std::mt19937_64 rng(seed);
        uint64_t nextId = 1;
        for (std::size_t event = 0; event < 4000; ++event) {
            const auto choice = rng() % 100;
            if (choice < 70) {
                const auto side = rng() % 2 ? Side::BID : Side::ASK;
                auto incoming = limit(nextId++, 1 + rng() % 80, 90 + rng() % 21, side, 1000 + rng() % 17);
                require(book.addOrder(incoming).accepted, "random valid limit accepted");
                reference.add(incoming);
            } else if (choice < 90) {
                const auto id = rng() % (nextId + 20);
                require(book.deleteOrder(id) == reference.cancel(id), "random cancellation status");
            } else {
                const auto id = rng() % (nextId + 20), quantity = rng() % 85;
                require(book.reduceOrder(id, quantity) == reference.reduce(id, quantity), "random reduction status");
            }
            compareReference(book, reference, event);
        }
        for (std::size_t i = 0; i < reference.trades.size(); ++i) {
            const auto& a = book.trades()[i]; const auto& b = reference.trades[i];
            require(a.quantity == b.quantity && a.price == b.price && a.makerOrderId == b.maker && a.takerOrderId == b.taker && a.buyerId == b.buyer && a.sellerId == b.seller && a.buyerOrderId == b.buyerOrder && a.sellerOrderId == b.sellerOrder, "full trade history differential comparison");
        }
    }
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"FIFO, maker prices, partial fills and identities", fifoAndMakerPrice},
        {"cancellation and priority-preserving reduction", cancellationAndReduction},
        {"atomic input rejection and ID reuse", rejectedRequestsAreAtomic},
        {"IOC and market remainder handling", immediateAndMarketOrders},
        {"quantity aggregate overflow boundary", aggregateOverflowBoundary},
        {"FOK atomicity and post-only boundaries", fillOrKillAndPostOnly},
        {"depth snapshots and instrument isolation", snapshotsAndIsolation},
        {"deterministic replay and history modes", deterministicAndRecordingModes},
        {"12,000-event differential model and conservation", randomizedDifferential},
    };
    std::size_t passed = 0;
    for (const auto& [name, run] : tests) {
        try { run(); ++passed; std::cout << "[PASS] " << name << '\n'; }
        catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n'; return 1;
        }
    }
    std::cout << passed << " test groups passed (checks remain active in Release builds).\n";
    return 0;
}
