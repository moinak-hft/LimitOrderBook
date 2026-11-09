# Benchmarking

The benchmark separates bulk throughput from instrumented submission samples. Amortized time per operation is not network tick-to-trade latency.

## Reproduce a run

Configure a Release build, then run these commands from the repository root. Examples use Linux/macOS executable paths. For Visual Studio builds on Windows, substitute `.\build\Release\limit_order_book.exe`; for Windows Ninja/MinGW, use `.\build\limit_order_book.exe`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
./build/limit_order_book benchmark --orders 1000000 --repeats 5 --seed 42 --output reports/benchmark.csv
./build/limit_order_book scale --orders 500000 --workers 8 --seed 42 --output reports/scaling.csv
```

`LOB_NATIVE=ON` adds `-march=native` for GCC/Clang. Use it only when the binary can depend on the compiling machine's instruction set. It currently has no extra effect on MSVC. The default is OFF. Compare builds with identical optimization settings. `LOB_BUILD_BASELINE=ON` builds `original_benchmark` from the preserved original source.

On Windows, the PowerShell helper uses portable CPU settings by default. Select native benchmark settings explicitly:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-lab.ps1 -Mode benchmark -Native
```

## Throughput methodology

- The input is generated before timing using `std::mt19937`, seed 42 by default. Each order draws side, price, then quantity, matching the original workload's draw order. Prices are 95..105 ticks, quantities are 1..100, instrument ID is 1, and trader ID is order ID modulo 100.
- Each retention mode gets one full warmup, followed by repeated trials on fresh books. The default is five trials. RNG/distribution behavior can differ across standard-library implementations, so seed equality alone does not establish byte-identical workloads between platforms.
- Timing uses `std::chrono::steady_clock` with full-precision elapsed seconds. Workload generation, book construction, trade preallocation, final state hashing, and invariant validation are outside the timer. Allocations during submissions remain inside.
- Retained-trade mode preallocates capacity for the number of submitted orders, matching the original benchmark's one-million-trade reserve for a one-million-order run. Further growth, if needed, remains timed. Hash capacity is prepared before timing and pools grow as submissions require.
- Digest-only mode retains no full trade ledger, but still performs validation, maintains trade counters, and updates the rolling trade digest. It is not a matching-only microbenchmark.
- All trials and both retention modes must produce identical state/trade fingerprints, trade counts, traded quantities, and active-order counts. Invariants are checked after each run.
- Console summaries report median, minimum, and arithmetic mean of per-trial throughput, plus median amortized nanoseconds/order. CSV files retain individual trial data.

## Instrumented submission samples

A separate pass times at most 20,000 evenly spaced submissions, then reports p50, p95, p99, and maximum measured duration. These samples include clock-call overhead, cache effects, timestamp quantization, interrupts, and OS scheduling noise. They are descriptive observations under instrumentation rather than a wire-latency distribution.

The program also measures the median difference between back-to-back clock reads. It prints this value without subtracting it from samples. On the measured Windows toolchain, many differences are quantized to 100 ns and the clock-pair median can be zero. A zero median does not imply a free timer or zero-duration operation.

## Development measurement

Recorded on 9 November 2026, using Windows 11 x64 and MSYS2 MinGW64 GCC 15.1.0 on an **AMD Ryzen 9 8945HS, 8 cores / 16 logical threads**. Release flags were `-O3 -DNDEBUG -march=native`. This was an ordinary development session without core isolation, scheduler tuning, or power-policy changes. Background load, thermal state, and power management were not controlled.

Single-book measurements used 1,000,000 seeded orders and five timed trials. Both the original and upgraded retained-trade runs reserved trade capacity before timing. Raw evidence: [upgraded benchmark](../examples/measurements/benchmark.csv) and [original baseline](../examples/measurements/original-native.csv).

| Engine / mode | Median throughput | Median amortized time |
|---|---:|---:|
| Preserved original | 4.210 million orders/s | 237.51 ns/order |
| Upgraded, retained trades | 5.361 million orders/s | 186.54 ns/order |
| Upgraded, digest only | 5.298 million orders/s | 188.76 ns/order |

Retained-trade throughput was approximately **27% higher** in this session. The implementations differ: the original reads a wall clock for every trade, whereas the upgraded engine uses deterministic event time, richer trade identities, additional validation, and a rolling trade digest. Trade record sizes also differ. This comparison measures the resulting implementations on the same local RNG workload; it does not isolate the contribution of a particular optimization. Small differences between retention modes should be interpreted alongside trial variation.

The preserved original README reports an Apple Silicon result without a specific chip model, compiler, power state, distributions, or raw trials. That number cannot establish a cross-processor ranking. Its reciprocal-throughput value is not a measured network tick-to-trade latency.

## Independent-book scaling

The scaling command runs one independent book per worker, using a shared, immutable, pregenerated workload. Each worker performs the complete workload on its own book. This is weak scaling: total operations increase with worker count. It does not introduce concurrent writers to one book.

Book construction occurs before a synchronized start barrier. A stop barrier captures the end when all workers have completed submissions. Validation, checksum computation, and thread joining happen afterward. Each worker must match the single-book reference result. Three timed trials are collected for worker counts 1, 2, 4, 8, and further counts up to the selected limit of 16.

The development run used **500,000 orders per worker**, digest-only mode. [Raw scaling trials](../examples/measurements/scaling.csv):

| Workers / independent books | Median aggregate throughput | Relative to one worker |
|---|---:|---:|
| 1 | 6.473 million orders/s | 1.00x |
| 2 | 11.608 million orders/s | 1.79x |
| 4 | 18.954 million orders/s | 2.93x |
| 8 | 35.550 million orders/s | 5.49x |

The per-book workload size differs from the single-book table. Aggregate throughput over separate books is not the processing rate of a shared order book. All worker fingerprints agreed with the reference. No GPU acceleration is implemented or claimed.

## Boundaries

The compressed price range produces a particular mix of fills and resting orders. It does not cover sparse books, large price ranges, adversarial hash behavior, cancel/replace-heavy flow, or real exchange event distributions. A faster result here need not transfer to another workload.

Cancellation uses hash lookup, direct list/level iterators, and iterator-based level removal; its expected/amortized complexity is O(1). New levels still require O(log L) map work. Memory pools can allocate and retain their high-water capacity; the engine is not allocation-free. The hash index offers expected rather than worst-case constant-time lookup.

Quantity reductions preserve FIFO position. Increases and repricing require cancel plus a new submission and lose priority. Price-level sums are checked before accepted mutation. Cumulative traded quantity saturates at the uint64 maximum. Active IDs may be reused after removal, so callers needing unambiguous lifetime identity should use globally unique IDs. Fingerprints are reproducibility checks, not cryptographic hashes.

There is no durable journal, restart recovery, risk gateway, self-trade prevention, authentication, networking, session protocol, or transaction-level recovery from allocation failures. An allocation failure can interrupt a mutation without rollback. Single-writer ownership is the caller's responsibility.

Useful follow-up measurements include allocation counts, peak memory, mixed-event traces, cancellation-heavy books, sparse ladders, and profiles before replacing the tree with a bounded tick ladder. Run comparisons on the same compiler, flags, workload, and machine state; retain repeated raw trials alongside summaries.
