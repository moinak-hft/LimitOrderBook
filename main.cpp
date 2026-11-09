#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include "Benchmark.h"
#include "ReplayLab.h"

namespace {
void help() {
    std::cout << R"(Limit Order Book / Replay Lab

Commands:
  benchmark  Repeated original-style throughput + separate sampled latency
  scale      Run independent books on 1, 2, 4, 8 (up to 16) CPU workers
  simulate   Export an interactive counterfactual execution replay
  experiment Compare all three execution policies across multiple random seeds

Examples (run from the project directory):
  limit_order_book benchmark --orders 1000000 --repeats 5 --output reports/benchmark.csv
  limit_order_book scale --orders 250000 --workers 8 --output reports/scaling.csv
  limit_order_book simulate --scenario shock --seed 42 --output reports/replay.html
  limit_order_book experiment --scenario rally --seeds 30 --output reports/experiment.csv

Options:
  --orders N      Orders per trial/book (default 1000000/250000)
  --repeats N     Throughput trials (default 5, maximum 30)
  --workers N     Maximum independent CPU workers (default min(8, hardware threads))
  --seed N        Deterministic seed (default 42, uint32 range)
  --steps N       Synthetic replay steps (default 240, range 24..2000)
  --quantity N    Buy target in integer units (default 3000, maximum 1000000)
  --scenario S    calm, rally, or shock (default shock)
  --fee-bps N     Taker fee in basis points of executed notional (default 0)
  --seeds N       Experiment seeds starting at --seed (default 30, maximum 200)
  --output PATH   Destination CSV or self-contained replay HTML

Prices are integer ticks. Simulator flow is synthetic. Policies observe only
current/past book state. One writer per book; independent books run in parallel.
)";
}
using Arguments = std::unordered_map<std::string, std::string>;
Arguments parse(int argc, char** argv) {
    Arguments args;
    for (int i = 2; i < argc; i += 2) {
        const std::string key = argv[i];
        if (key.rfind("--", 0) != 0 || i + 1 >= argc)
            throw std::invalid_argument("Expected --option value; use --help.");
        if (!args.emplace(key, argv[i + 1]).second)
            throw std::invalid_argument("Duplicate option: " + key);
    }
    return args;
}
std::uint64_t number(Arguments& args, const std::string& key, std::uint64_t fallback,
                     std::uint64_t min, std::uint64_t max) {
    auto it = args.find(key);
    if (it == args.end()) return fallback;
    const std::string value = it->second;
    args.erase(it);
    std::uint64_t result = 0;
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (ec != std::errc{} || end != value.data() + value.size() || result < min || result > max)
        throw std::invalid_argument(key + " must be an integer in " + std::to_string(min) + ".." + std::to_string(max));
    return result;
}
std::string str(Arguments& args, const std::string& key, const std::string& fallback) {
    auto it = args.find(key);
    if (it == args.end()) return fallback;
    std::string value = it->second;
    args.erase(it);
    if (value.empty()) throw std::invalid_argument(key + " cannot be empty");
    return value;
}
void consumed(const Arguments& args) {
    if (!args.empty()) throw std::invalid_argument("Unknown option for this command: " + args.begin()->first);
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 1 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") {
            help(); return 0;
        }
        const std::string command = argv[1];
        auto args = parse(argc, argv);
        const auto seed = number(args, "--seed", 42, 0, std::numeric_limits<std::uint32_t>::max());
        if (command == "benchmark") {
            const auto orders = number(args, "--orders", 1000000, 100, 5000000);
            const auto repeats = number(args, "--repeats", 5, 1, 30);
            const auto output = str(args, "--output", "reports/benchmark.csv");
            consumed(args);
            return lab::runBenchmark(orders, repeats, seed, output);
        }
        if (command == "scale") {
            const auto orders = number(args, "--orders", 250000, 100, 1000000);
            const auto workers = number(args, "--workers", std::min(8u, std::max(1u, std::thread::hardware_concurrency())), 1, 16);
            const auto output = str(args, "--output", "reports/scaling.csv");
            consumed(args);
            return lab::runScaling(orders, workers, seed, output);
        }
        if (command == "simulate" || command == "experiment") {
            lab::SimulationOptions options;
            options.seed = seed;
            options.steps = number(args, "--steps", 240, 24, 2000);
            options.quantity = number(args, "--quantity", 3000, 1, 1000000);
            options.scenario = str(args, "--scenario", "shock");
            if (options.scenario != "calm" && options.scenario != "rally" && options.scenario != "shock")
                throw std::invalid_argument("--scenario must be calm, rally, or shock");
            const auto fee = str(args, "--fee-bps", "0");
            std::size_t feeEnd = 0;
            options.feeBps = std::stod(fee, &feeEnd);
            if (feeEnd != fee.size() || !std::isfinite(options.feeBps) || options.feeBps < 0 || options.feeBps > 1000)
                throw std::invalid_argument("--fee-bps must be a finite number in 0..1000");
            options.outputPath = str(args, "--output", command == "simulate" ? "reports/replay.html" : "reports/experiment.csv");
            const auto seeds = command == "experiment" ? number(args, "--seeds", 30, 1, 200) : 0;
            if (seeds && seed + seeds - 1 > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("--seed plus --seeds exceeds uint32 range");
            consumed(args);
            return command == "simulate" ? lab::runSimulation(options) : lab::runExperiment(options, seeds, options.outputPath);
        }
        throw std::invalid_argument("Unknown command: " + command + ". Use --help.");
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
