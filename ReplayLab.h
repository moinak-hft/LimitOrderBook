#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace lab {
struct SimulationOptions {
    std::size_t steps = 240;
    std::uint64_t quantity = 3000;
    std::uint64_t seed = 42;
    std::string scenario = "shock";
    double feeBps = 0.0;
    std::string outputPath = "reports/replay.html";
};
int runSimulation(const SimulationOptions& options);
int runExperiment(const SimulationOptions& options, std::size_t seeds, const std::string& outputPath);
}
