#include "ReplayLab.h"
#include "OrderBook.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace lab {
namespace {
constexpr std::uint64_t arrivalMid = 10000;
constexpr std::uint64_t strategyTrader = 900000;
constexpr std::size_t ladderLevels = 20;
struct Tick {
    std::uint64_t fair;
    std::array<std::uint64_t, ladderLevels> bidSize{}, askSize{};
    std::uint64_t marketSize;
    Side marketSide;
    bool refresh;
};
struct Frame {
    std::size_t step;
    std::uint64_t fair, filled, executed;
    double vwap;
    BookSnapshot book;
};
struct Run {
    std::string name, description;
    std::uint64_t filled = 0;
    long double notional = 0, weightedStep = 0;
    std::uint64_t checksum = 0;
    std::vector<Frame> frames;
    double vwap() const { return filled ? static_cast<double>(notional / filled) : 0.0; }
    double slippage() const { return (vwap() / arrivalMid - 1.0) * 10000.0; }
};
void ensureParent(const std::filesystem::path& path) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
}
std::ofstream output(const std::filesystem::path& path) {
    ensureParent(path);
    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot write " + path.string());
    file.exceptions(std::ios::badbit | std::ios::failbit);
    return file;
}
void checkOptions(const SimulationOptions& o) {
    if (o.steps < 24 || o.steps > 2000 || o.quantity == 0 || o.quantity > 1000000 ||
        o.seed > std::numeric_limits<std::uint32_t>::max() || !std::isfinite(o.feeBps) ||
        o.feeBps < 0 || o.feeBps > 1000 ||
        (o.scenario != "calm" && o.scenario != "rally" && o.scenario != "shock"))
        throw std::invalid_argument("Invalid simulation options");
}
std::vector<Tick> tape(const SimulationOptions& o) {
    std::mt19937 rng(static_cast<std::uint32_t>(o.seed));
    std::vector<Tick> events;
    events.reserve(o.steps);
    std::int64_t fair = arrivalMid;
    for (std::size_t step = 0; step < o.steps; ++step) {
        if (step) fair += static_cast<int>(rng() % 3) - 1;
        if (o.scenario == "rally" && step && step % 3 == 0) ++fair;
        const bool shockStart = o.scenario == "shock" && step == o.steps / 2;
        if (shockStart) fair -= 35;
        const bool thin = o.scenario == "shock" && step >= o.steps / 2 && step < o.steps / 2 + o.steps / 8;
        Tick tick{};
        tick.fair = static_cast<std::uint64_t>(fair);
        tick.refresh = step % 4 == 0 || shockStart;
        for (std::size_t level = 0; level < ladderLevels; ++level) {
            tick.bidSize[level] = 120 + rng() % 161;
            tick.askSize[level] = 120 + rng() % 161;
            if (thin) { tick.bidSize[level] /= 3; tick.askSize[level] /= 3; }
        }
        tick.marketSize = rng() % 4 == 0 ? 0 : 10 + rng() % 61;
        tick.marketSide = rng() % 2 ? Side::BID : Side::ASK;
        events.push_back(tick);
    }
    return events;
}
Run execute(const SimulationOptions& o, const std::vector<Tick>& events, int policy, bool capture) {
    static const std::array<std::string, 3> names{"Immediate", "TWAP", "Liquidity-aware"};
    static const std::array<std::string, 3> descriptions{
        "Buy the entire target at step zero; any unavailable quantity stays unfilled.",
        "Buy evenly over the horizon, carrying missed quantity forward; sweep on the final step.",
        "Experimental causal schedule: adjust child size using top-five depth imbalance and a moving midpoint; sweep on the final step."
    };
    Run run;
    run.name = names.at(policy);
    run.description = descriptions.at(policy);
    if (capture) run.frames.reserve(o.steps);
    OrderBook book(1, true, 256);
    std::vector<std::uint64_t> quotes;
    std::uint64_t nextId = 1;
    double midpointEMA = arrivalMid;
    for (std::size_t step = 0; step < o.steps; ++step) {
        const auto& tick = events[step];
        if (tick.refresh) {
            for (auto id : quotes) book.deleteOrder(id);
            quotes.clear();
            for (std::size_t level = 0; level < ladderLevels; ++level) {
                const auto distance = static_cast<std::uint64_t>(level + 1);
                const auto bidId = nextId++, askId = nextId++;
                if (!book.addOrder(Order(bidId, tick.bidSize[level], tick.fair - distance, 1, 1 + level,
                                         Side::BID, step)).accepted ||
                    !book.addOrder(Order(askId, tick.askSize[level], tick.fair + distance, 1, 101 + level,
                                         Side::ASK, step)).accepted)
                    throw std::runtime_error("Synthetic quote was rejected");
                quotes.push_back(bidId); quotes.push_back(askId);
            }
        }
        if (tick.marketSize) {
            book.addOrder(Order(nextId++, tick.marketSize, 0, 1, 500, tick.marketSide, step,
                                OrderType::Market, TimeInForce::IOC));
        }
        const auto visible = book.snapshot(5);
        const auto bestBid = book.bestBid(), bestAsk = book.bestAsk();
        const double mid = bestBid && bestAsk ? (static_cast<double>(*bestBid) + *bestAsk) / 2.0 : midpointEMA;
        const auto remaining = o.quantity - run.filled;
        std::uint64_t child = 0;
        if (policy == 0) {
            if (step == 0) child = remaining;
        } else if (step + 1 == o.steps) {
            child = remaining;
        } else if (policy == 1) {
            const auto scheduled = o.quantity * (step + 1) / o.steps;
            child = scheduled > run.filled ? scheduled - run.filled : 0;
        } else if (remaining && bestAsk) {
            std::uint64_t bidDepth = 0, askDepth = 0;
            for (const auto& p : visible.bids) bidDepth += p.quantity;
            for (const auto& p : visible.asks) askDepth += p.quantity;
            const double imbalance = bidDepth + askDepth ?
                (static_cast<double>(bidDepth) - askDepth) / (bidDepth + askDepth) : 0;
            const double multiplier = std::clamp(1.0 + 1.2 * imbalance + 0.2 * (midpointEMA - mid), 0.15, 3.0);
            const double pace = static_cast<double>(remaining) / (o.steps - step);
            child = static_cast<std::uint64_t>(std::ceil(pace * multiplier));
            child = std::min(child, std::max<std::uint64_t>(1, askDepth / 5));
        }
        midpointEMA = 0.92 * midpointEMA + 0.08 * mid;
        child = std::min(child, remaining);
        std::uint64_t executed = 0;
        if (child) {
            const auto strategyOrderId = (std::uint64_t{1} << 63) + step;
            const auto tradeStart = book.trades().size();
            const auto result = book.addOrder(Order(strategyOrderId, child, 0, 1, strategyTrader,
                                                   Side::BID, step, OrderType::Market, TimeInForce::IOC));
            if (!result.accepted) throw std::runtime_error("Strategy child was rejected");
            for (std::size_t i = tradeStart; i < book.trades().size(); ++i) {
                const auto& trade = book.trades()[i];
                if (trade.buyerId != strategyTrader || trade.buyerOrderId != strategyOrderId)
                    throw std::runtime_error("Execution identity mismatch");
                executed += trade.quantity;
                run.notional += static_cast<long double>(trade.quantity) * trade.price;
                run.weightedStep += static_cast<long double>(trade.quantity) * step;
            }
            if (executed != result.filled) throw std::runtime_error("Fill accounting mismatch");
            run.filled += executed;
            if (run.filled > o.quantity) throw std::runtime_error("Strategy exceeded target");
        }
        if (!book.validate()) throw std::runtime_error("Book invariant failed during replay");
        if (capture) run.frames.push_back(Frame{step, tick.fair, run.filled, executed, run.vwap(), book.snapshot(20)});
        book.clearTrades();
    }
    run.checksum = book.checksum();
    return run;
}
std::vector<Run> simulate(const SimulationOptions& o, bool capture) {
    const auto events = tape(o);
    std::vector<Run> runs;
    for (int policy = 0; policy < 3; ++policy) runs.push_back(execute(o, events, policy, capture));
    return runs;
}
void levelsJSON(std::ostream& out, const std::vector<PriceLevel>& levels) {
    out << '[';
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i) out << ',';
        out << '[' << levels[i].price << ',' << levels[i].quantity << ']';
    }
    out << ']';
}
std::string json(const SimulationOptions& o, const std::vector<Run>& runs) {
    std::ostringstream out;
    out << std::setprecision(12) << "{\"meta\":{\"scenario\":\"" << o.scenario << "\",\"seed\":" << o.seed
        << ",\"steps\":" << o.steps << ",\"target\":" << o.quantity << ",\"arrivalMid\":" << arrivalMid
        << ",\"feeBps\":" << o.feeBps << "},\"runs\":[";
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (i) out << ',';
        const auto& run = runs[i];
        out << "{\"name\":\"" << run.name << "\",\"description\":\"" << run.description
            << "\",\"filled\":" << run.filled << ",\"unfilled\":" << (o.quantity - run.filled)
            << ",\"complete\":" << (run.filled == o.quantity ? "true" : "false") << ",\"vwap\":";
        if (run.filled) out << run.vwap(); else out << "null";
        out << ",\"slippageBps\":";
        if (run.filled) out << run.slippage(); else out << "null";
        out << ",\"fee\":" << static_cast<double>(run.notional * o.feeBps / 10000.0)
            << ",\"averageStep\":" << (run.filled ? static_cast<double>(run.weightedStep / run.filled) : 0)
            << ",\"checksum\":\"" << run.checksum << "\",\"frames\":[";
        for (std::size_t j = 0; j < run.frames.size(); ++j) {
            if (j) out << ',';
            const auto& f = run.frames[j];
            out << "{\"step\":" << f.step << ",\"fair\":" << f.fair << ",\"filled\":" << f.filled
                << ",\"executed\":" << f.executed << ",\"vwap\":";
            if (f.filled) out << f.vwap; else out << "null";
            out << ",\"bid\":";
            if (!f.book.bids.empty()) out << f.book.bids.front().price; else out << "null";
            out << ",\"ask\":";
            if (!f.book.asks.empty()) out << f.book.asks.front().price; else out << "null";
            out << ",\"bids\":"; levelsJSON(out, f.book.bids);
            out << ",\"asks\":"; levelsJSON(out, f.book.asks);
            out << '}';
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}
std::string reportTemplate() {
    std::vector<std::filesystem::path> candidates{"assets/replay.html"};
#ifdef LOB_SOURCE_DIR
    candidates.emplace_back(std::filesystem::path(LOB_SOURCE_DIR) / "assets/replay.html");
#endif
    for (const auto& candidate : candidates) {
        std::ifstream file(candidate, std::ios::binary);
        if (file) return std::string(std::istreambuf_iterator<char>(file), {});
    }
    throw std::runtime_error("Cannot find assets/replay.html; run from the project/build directory");
}
void printRuns(const SimulationOptions& o, const std::vector<Run>& runs) {
    std::cout << "\nSynthetic " << o.scenario << " | seed " << o.seed << " | target " << o.quantity
              << " | " << o.steps << " steps | fee " << o.feeBps << " bps\n";
    std::cout << std::left << std::setw(19) << "Policy" << std::setw(16) << "Filled/target"
              << std::setw(16) << "VWAP (ticks)" << "Slippage (bps vs arrival midpoint)\n";
    for (const auto& run : runs) {
        std::cout << std::left << std::setw(19) << run.name << std::setw(16)
                  << (std::to_string(run.filled) + "/" + std::to_string(o.quantity));
        if (run.filled) std::cout << std::fixed << std::setprecision(3) << std::setw(16) << run.vwap() << run.slippage();
        else std::cout << std::setw(16) << "n/a" << "n/a";
        std::cout << '\n';
    }
    std::cout << "VWAP excludes fees. Compare full targets; incomplete fills have opportunity cost.\n";
}
} // namespace

int runSimulation(const SimulationOptions& options) {
    checkOptions(options);
    auto runs = simulate(options, true);
    // Replaying without frame capture must reproduce fills, notional and every book digest.
    const auto replay = simulate(options, false);
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (runs[i].checksum != replay[i].checksum || runs[i].filled != replay[i].filled || runs[i].notional != replay[i].notional)
            throw std::runtime_error("Deterministic replay verification failed");
    }
    const auto data = json(options, runs);
    auto html = reportTemplate();
    const std::string token = "__LOB_DATA__";
    const auto position = html.find(token);
    if (position == std::string::npos || html.find(token, position + token.size()) != std::string::npos)
        throw std::runtime_error("Replay template must have exactly one data placeholder");
    html.replace(position, token.size(), data);
    const std::filesystem::path path(options.outputPath);
    auto dataPath = path; dataPath.replace_extension(".json");
    if (path == dataPath) throw std::invalid_argument("Simulation output must use .html extension");
    if (path.extension() != ".html") throw std::invalid_argument("Simulation output must use .html extension");
    output(path) << html;
    output(dataPath) << data << '\n';
    printRuns(options, runs);
    std::cout << "Replay verified. Open " << std::filesystem::absolute(path).string() << '\n';
    return 0;
}

int runExperiment(const SimulationOptions& options, std::size_t seeds, const std::string& outputPath) {
    checkOptions(options);
    if (!seeds || seeds > 200 || options.seed + seeds - 1 > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Experiment requires 1..200 valid seeds");
    auto file = output(outputPath);
    file << std::setprecision(12)
         << "seed,scenario,policy,target,filled,fill_rate,vwap_ticks,slippage_bps,fee_ticks_units,average_step,checksum\n";
    std::array<std::size_t, 3> full{}, paired{}, wins{};
    std::array<double, 3> pairedSavings{}, sumSquares{};
    for (std::size_t n = 0; n < seeds; ++n) {
        auto current = options;
        current.seed += n;
        const auto runs = simulate(current, false);
        for (std::size_t i = 0; i < runs.size(); ++i) {
            const auto& run = runs[i];
            file << current.seed << ',' << current.scenario << ',' << run.name << ',' << current.quantity << ','
                 << run.filled << ',' << static_cast<double>(run.filled) / current.quantity << ',';
            if (run.filled) file << run.vwap();
            file << ',';
            if (run.filled) file << run.slippage();
            file << ',' << static_cast<double>(run.notional * current.feeBps / 10000.0) << ','
                 << (run.filled ? static_cast<double>(run.weightedStep / run.filled) : 0) << ',' << run.checksum << '\n';
            if (run.filled == current.quantity) ++full[i];
            if (i && run.filled == current.quantity && runs[0].filled == current.quantity) {
                const double saving = runs[0].slippage() - run.slippage();
                ++paired[i]; pairedSavings[i] += saving; sumSquares[i] += saving * saving;
                if (saving > 0) ++wins[i];
            }
        }
    }
    std::cout << "\n" << options.scenario << " experiment: " << seeds << " common seeds, " << options.quantity << " buy units\n";
    const std::array<std::string, 3> names{"Immediate", "TWAP", "Liquidity-aware"};
    for (std::size_t i = 0; i < 3; ++i) {
        std::cout << names[i] << ": full target on " << full[i] << '/' << seeds << " seeds";
        if (i && paired[i]) {
            const double mean = pairedSavings[i] / paired[i];
            const double variance = paired[i] > 1 ? std::max(0.0, (sumSquares[i] - paired[i] * mean * mean) / (paired[i] - 1)) : 0;
            std::cout << "; mean savings vs immediate " << std::fixed << std::setprecision(3) << mean
                      << " bps; sample SD " << std::sqrt(variance) << "; lower cost on " << wins[i] << '/' << paired[i]
                      << " fully-filled pairs";
        }
        std::cout << '\n';
    }
    std::cout << "Synthetic scenarios do not establish a trading advantage. CSV: " << outputPath << '\n';
    return 0;
}
} // namespace lab
