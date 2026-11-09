#include "Benchmark.h"
#include "OrderBook.h"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace lab {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t maxBenchmarkOrders = 5'000'000;
constexpr std::size_t maxScalingOrders = 1'000'000;
constexpr std::size_t maxLatencySamples = 20'000;

std::vector<Order> makeWorkload(std::size_t count, std::uint64_t seed) {
    // Match the original workload's RNG and draw order. Construction is untimed.
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    std::uniform_int_distribution<std::uint64_t> price(95, 105), quantity(1, 100);
    std::uniform_int_distribution<int> side(0, 1);
    std::vector<Order> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto nextSide = side(rng) == 0 ? Side::ASK : Side::BID;
        const auto nextPrice = price(rng);
        const auto nextQuantity = quantity(rng);
        result.emplace_back(i, nextQuantity, nextPrice, 1, i % 100, nextSide);
    }
    return result;
}

std::ofstream csvFile(const std::string& path, const char* header) {
    if (path.empty()) return {};
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream file(path);
    if (!file) throw std::runtime_error("Cannot create benchmark output: " + path);
    file << header << '\n' << std::setprecision(17);
    return file;
}

double median(std::vector<double> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto half = values.size() / 2;
    return values.size() % 2 ? values[half] : (values[half - 1] + values[half]) / 2;
}

double quantile(const std::vector<double>& sorted, double fraction) {
    return sorted[static_cast<std::size_t>(std::ceil(fraction * (sorted.size() - 1)))];
}

struct Result {
    double seconds = 0;
    std::uint64_t checksum = 0, trades = 0, quantity = 0;
    std::size_t active = 0;
    bool valid = false;
};

Result describe(const OrderBook& book, double seconds, bool accepted) {
    return {seconds, book.checksum(), book.tradeCount(), book.tradedQuantity(),
            book.activeOrderCount(), accepted && book.validate()};
}

Result execute(const std::vector<Order>& workload, bool recordTrades) {
    OrderBook book(1, recordTrades, std::min<std::size_t>(workload.size(), 65'536));
    if (recordTrades) book.reserveTrades(workload.size());
    bool accepted = true;
    const auto begin = Clock::now();
    for (const auto& order : workload) accepted &= book.addOrder(order).accepted;
    const auto end = Clock::now();
    return describe(book, std::chrono::duration<double>(end - begin).count(), accepted);
}

void requireSame(const Result& result, const Result& reference) {
    if (!result.valid || result.checksum != reference.checksum ||
        result.trades != reference.trades || result.quantity != reference.quantity ||
        result.active != reference.active) {
        throw std::runtime_error("Benchmark state mismatch or failed book invariant");
    }
}

void printEnvironment(std::size_t count, std::uint64_t seed) {
    std::cout << "Workload: " << count << " pregenerated orders, seed " << seed
              << ", price ticks 95..105, quantities 1..100\n";
    std::cout << "Compiler: " <<
#if defined(__clang__)
        "Clang " __clang_version__
#elif defined(__GNUC__)
        "GCC " __VERSION__
#elif defined(_MSC_VER)
        "MSVC"
#else
        "unknown"
#endif
        << "; hardware threads reported: " << std::thread::hardware_concurrency() << '\n';
    std::cout << "Timing: steady_clock; workload generation, book construction and final validation excluded.\n";
}

struct Latency {
    std::vector<double> samples;
    double clockPairMedian = 0;
    Result result;
};

Latency sampleLatency(const std::vector<Order>& workload, bool recordTrades) {
    Latency latency;
    std::vector<double> clockPairs;
    clockPairs.reserve(maxLatencySamples);
    for (std::size_t i = 0; i < maxLatencySamples; ++i) {
        const auto start = Clock::now();
        const auto finish = Clock::now();
        clockPairs.push_back(std::chrono::duration<double, std::nano>(finish - start).count());
    }
    latency.clockPairMedian = median(std::move(clockPairs));
    latency.samples.reserve(std::min(workload.size(), maxLatencySamples));
    const auto stride = std::max<std::size_t>(1, (workload.size() + maxLatencySamples - 1) / maxLatencySamples);
    OrderBook book(1, recordTrades, std::min<std::size_t>(workload.size(), 65'536));
    if (recordTrades) book.reserveTrades(workload.size());
    bool accepted = true;
    for (std::size_t i = 0; i < workload.size(); ++i) {
        if (i % stride == 0) {
            const auto begin = Clock::now();
            const auto submitted = book.addOrder(workload[i]);
            const auto end = Clock::now();
            accepted &= submitted.accepted;
            latency.samples.push_back(std::chrono::duration<double, std::nano>(end - begin).count());
        } else {
            accepted &= book.addOrder(workload[i]).accepted;
        }
    }
    std::sort(latency.samples.begin(), latency.samples.end());
    latency.result = describe(book, 0, accepted);
    return latency;
}

struct ParallelResult {
    double seconds = 0;
    std::vector<Result> books;
};

ParallelResult executeParallel(const std::vector<Order>& workload, std::size_t workers) {
    ParallelResult output;
    output.books.resize(workers);
    std::vector<std::exception_ptr> failures(workers);
    Clock::time_point begin, end;
    std::barrier startGate(static_cast<std::ptrdiff_t>(workers + 1), [&]() noexcept { begin = Clock::now(); });
    std::barrier endGate(static_cast<std::ptrdiff_t>(workers + 1), [&]() noexcept { end = Clock::now(); });
    std::vector<std::thread> threads;
    threads.reserve(workers);
    std::exception_ptr launchFailure;
    for (std::size_t worker = 0; worker < workers; ++worker) {
        try {
            threads.emplace_back([&, worker] {
                std::unique_ptr<OrderBook> book;
                bool accepted = true;
                try {
                    book = std::make_unique<OrderBook>(1, false, std::min<std::size_t>(workload.size(), 65'536));
                } catch (...) { failures[worker] = std::current_exception(); }
                startGate.arrive_and_wait();
                try {
                    if (book) {
                        for (const auto& order : workload) accepted &= book->addOrder(order).accepted;
                    }
                } catch (...) { failures[worker] = std::current_exception(); }
                endGate.arrive_and_wait();
                // Hashing and invariant checking deliberately happen after the stop barrier.
                try {
                    if (book && !failures[worker]) output.books[worker] = describe(*book, 0, accepted);
                } catch (...) { failures[worker] = std::current_exception(); }
            });
        } catch (...) {
            launchFailure = std::current_exception();
            for (std::size_t uncreated = worker; uncreated < workers; ++uncreated) {
                startGate.arrive_and_drop();
                endGate.arrive_and_drop();
            }
            break;
        }
    }
    startGate.arrive_and_wait();
    endGate.arrive_and_wait();
    for (auto& thread : threads) thread.join();
    if (launchFailure) std::rethrow_exception(launchFailure);
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    output.seconds = std::chrono::duration<double>(end - begin).count();
    return output;
}
}

int runBenchmark(std::size_t orders, std::size_t repeats, std::uint64_t seed,
                 const std::string& outputPath) {
    if (orders == 0 || orders > maxBenchmarkOrders || repeats == 0 || repeats > 30 ||
        seed > std::numeric_limits<std::mt19937::result_type>::max() || seed > 0xffffffffULL) {
        throw std::invalid_argument("benchmark requires 1..5000000 orders, 1..30 repeats, seed <= 4294967295");
    }
    printEnvironment(orders, seed);
    const auto workload = makeWorkload(orders, seed);
    auto csv = csvFile(outputPath,
        "record_type,mode,trial,orders,seconds,orders_per_second,mean_ns_per_order,samples,p50_ns,p95_ns,p99_ns,max_ns,clock_pair_median_ns,checksum,trade_count,traded_quantity,active_orders");
    std::cout << "One full warmup per mode. Throughput trials have no per-order clocks.\n";
    Result reference;
    bool haveReference = false;
    for (bool record : {true, false}) {
        const char* mode = record ? "recorded_trades" : "digest_only";
        const auto warmup = execute(workload, record);
        if (!warmup.valid) throw std::runtime_error("Warmup failed invariant checking");
        if (!haveReference) { reference = warmup; haveReference = true; }
        requireSame(warmup, reference);
        std::vector<double> throughputs, means;
        for (std::size_t trial = 1; trial <= repeats; ++trial) {
            const auto result = execute(workload, record);
            requireSame(result, reference);
            if (result.seconds <= 0) throw std::runtime_error("Timer resolution too coarse for this workload");
            const auto throughput = static_cast<double>(orders) / result.seconds;
            const auto mean = result.seconds * 1e9 / static_cast<double>(orders);
            throughputs.push_back(throughput);
            means.push_back(mean);
            if (csv) csv << "throughput," << mode << ',' << trial << ',' << orders << ',' << result.seconds
                << ',' << throughput << ',' << mean << ",,,,,,," << result.checksum << ',' << result.trades
                << ',' << result.quantity << ',' << result.active << '\n';
        }
        const auto avg = std::accumulate(throughputs.begin(), throughputs.end(), 0.0) / throughputs.size();
        std::cout << "\n" << mode << ": " << std::fixed << std::setprecision(3)
                  << median(throughputs) / 1e6 << " M orders/s median; "
                  << *std::min_element(throughputs.begin(), throughputs.end()) / 1e6 << " min; "
                  << avg / 1e6 << " mean (" << repeats << " trials)\n"
                  << "  Median amortized time: " << median(means) << " ns/order; checksum " << reference.checksum << '\n';
        const auto latency = sampleLatency(workload, record);
        requireSame(latency.result, reference);
        const auto p50 = quantile(latency.samples, .50), p95 = quantile(latency.samples, .95),
                   p99 = quantile(latency.samples, .99), maximum = latency.samples.back();
        std::cout << "  Separate instrumented sample (" << latency.samples.size()
                  << "): p50 " << p50 << ", p95 " << p95 << ", p99 " << p99
                  << ", max " << maximum << " ns\n"
                  << "  Back-to-back steady_clock pair median: " << latency.clockPairMedian
                  << " ns (included, not subtracted).\n";
        if (csv) csv << "sampled_latency," << mode << ",0," << orders << ",,,," << latency.samples.size()
            << ',' << p50 << ',' << p95 << ',' << p99 << ',' << maximum << ',' << latency.clockPairMedian
            << ',' << reference.checksum << ',' << reference.trades << ',' << reference.quantity << ',' << reference.active << '\n';
    }
    std::cout << "\nAmortized time is elapsed/operations; instrumented samples include timer overhead and OS noise.\n"
              << "Neither number measures network tick-to-trade latency. All runs produced identical state/trade digests.\n";
    if (csv) { csv.flush(); if (!csv) throw std::runtime_error("Writing benchmark CSV failed"); }
    if (!outputPath.empty()) std::cout << "CSV: " << outputPath << '\n';
    return 0;
}

int runScaling(std::size_t ordersPerBook, std::size_t maxWorkers, std::uint64_t seed,
               const std::string& outputPath) {
    if (ordersPerBook == 0 || ordersPerBook > maxScalingOrders || maxWorkers == 0 || maxWorkers > 16 || seed > 0xffffffffULL) {
        throw std::invalid_argument("scaling requires 1..1000000 orders/book, 1..16 workers, seed <= 4294967295");
    }
    printEnvironment(ordersPerBook, seed);
    std::cout << "Weak scaling: one independent book per worker, shared read-only workload, trade digest only.\n"
              << "Three timed trials per worker count; synchronized start/stop; no shared-book writes.\n";
    const auto workload = makeWorkload(ordersPerBook, seed);
    const auto reference = execute(workload, false); // Untimed reference also warms this workload.
    if (!reference.valid) throw std::runtime_error("Scaling reference failed invariant checking");
    auto csv = csvFile(outputPath,
        "record_type,workers,trial,orders_per_book,total_orders,seconds,orders_per_second,speedup,checksum_per_book,trade_count_per_book,active_orders_per_book");
    std::vector<std::size_t> workerCounts;
    for (std::size_t count = 1; count <= maxWorkers; count *= 2) workerCounts.push_back(count);
    if (workerCounts.back() != maxWorkers) workerCounts.push_back(maxWorkers);
    double singleMedian = 0;
    for (const auto workers : workerCounts) {
        std::vector<double> rates;
        for (std::size_t trial = 1; trial <= 3; ++trial) {
            const auto result = executeParallel(workload, workers);
            for (const auto& book : result.books) requireSame(book, reference);
            if (result.seconds <= 0) throw std::runtime_error("Timer resolution too coarse for this workload");
            const auto total = ordersPerBook * workers;
            const auto rate = static_cast<double>(total) / result.seconds;
            rates.push_back(rate);
            if (csv) csv << "trial," << workers << ',' << trial << ',' << ordersPerBook << ',' << total
                << ',' << result.seconds << ',' << rate << ",," << reference.checksum << ','
                << reference.trades << ',' << reference.active << '\n';
        }
        const auto rateMedian = median(rates);
        if (workers == 1) singleMedian = rateMedian;
        const auto speedup = rateMedian / singleMedian;
        std::cout << workers << " worker" << (workers == 1 ? " " : "s") << ": "
                  << std::fixed << std::setprecision(3) << rateMedian / 1e6 << " M orders/s median, "
                  << speedup << "x single-worker throughput\n";
        if (csv) csv << "median," << workers << ",0," << ordersPerBook << ',' << ordersPerBook * workers
            << ",," << rateMedian << ',' << speedup << ',' << reference.checksum << ','
            << reference.trades << ',' << reference.active << '\n';
    }
    std::cout << "All independent books matched the reference checksum. Scaling measures aggregate work on different books.\n";
    if (csv) { csv.flush(); if (!csv) throw std::runtime_error("Writing scaling CSV failed"); }
    if (!outputPath.empty()) std::cout << "CSV: " << outputPath << '\n';
    return 0;
}
}
