#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lab {
// Output is CSV when outputPath is nonempty. Each function also prints a summary.
int runBenchmark(std::size_t orders, std::size_t repeats, std::uint64_t seed,
                 const std::string& outputPath);
int runScaling(std::size_t ordersPerBook, std::size_t maxWorkers, std::uint64_t seed,
               const std::string& outputPath);
}
