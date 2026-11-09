# Limit Order Book / Replay Lab

A deterministic C++20 matching engine and a local lab for exploring execution decisions. This extends the original small matching-engine benchmark into a tested engine, reproducible performance suite, and interactive synthetic market replay.

## Start on this laptop

Double-click **Open-Lab.cmd** in this folder. It builds a Release executable, runs the default shock scenario, verifies an identical replay, and opens the interactive report in your browser. No server, account, API key, or market-data download is needed.

Reports already generated during development:

- `reports/replay.html`: falling-price shock, seed 42
- `reports/calm.html`: random-walk scenario, seed 42
- `reports/rally.html`: rising-price scenario, seed 42
- `reports/benchmark.csv` and `reports/scaling.csv`: actual measurements on this laptop
- `reports/experiment-{calm,shock,rally}.csv`: 30 common seeds per scenario
- `reports/hardware.json`: local hardware inventory without serial numbers

The original seven files are preserved unchanged in `original/`. This download did not contain Git history.

## What changed

- **Safer matching:** one instrument per book, active-ID uniqueness, validated inputs, integer price ticks, maker-price fills, FIFO queues, partial fills, and observable submit results.
- **More order behavior:** market orders, limit orders, GTC, immediate-or-cancel (IOC), atomic fill-or-kill (FOK), and post-only. Quantity reductions preserve queue position; increases or repricing require cancel + new submit and lose priority.
- **Faster cancellation and reusable memory:** direct order and price-level iterators, pooled map/list/hash nodes, and maintained level quantities. Cancellation is expected/amortized O(1); a new price level still uses O(log L) tree work. Pools can grow and allocate; this is not a zero-allocation engine.
- **Deterministic event time:** callers can supply timestamps; zero uses the event sequence for trade time. Matching makes no operating-system clock calls. Trade records separate order IDs from trader IDs. An order ID may be reused after it leaves the active book; clients should use unique IDs if they need unambiguous lifetime identity.
- **Inspectability:** depth snapshots, best prices, per-order lookup, cumulative counters, validation, and stable state/trade fingerprints, with optional trade retention. Fingerprints are reproducibility checks, not cryptographic hashes.
- **Honest measurement:** pregenerated input, full-precision steady-clock durations, warmup, repeated throughput trials, separate sampled latency, CSV exports, and deterministic result checks.
- **Multicore experiments:** independent books across CPU workers. Each book has exactly one writer. Workers do not concurrently mutate the same book.
- **Execution replay:** three policies receive the same synthetic background events in separate books; the exported report lets you scrub time, compare cumulative VWAP, and inspect price-level depth.

## Your laptop and measured results

Detected on **9 November 2026**:

| Component | Local inventory |
|---|---|
| Laptop | HP Victus 15-fb3xxx |
| CPU | AMD Ryzen 9 8945HS, 8 cores / 16 logical threads |
| Memory | 16 GiB installed, about 15.2 GiB usable, DDR5-5600 reported |
| Dedicated GPU | NVIDIA RTX 4060 Laptop GPU, 8188 MiB reported VRAM |
| Integrated GPU | AMD Radeon graphics |
| OS / compiler | Windows 11 x64 / MSYS2 MinGW64 GCC 15.1.0 |

Local Release build: `-O3 -DNDEBUG -march=native`. Each single-book trial used 1,000,000 seeded orders in the original 95..105 tick range, with five timed trials. The original and upgraded retained-trade runs both reserved trade capacity before timing.

| Run | Median throughput | Median amortized time |
|---|---:|---:|
| Preserved original | 4.210 million orders/s | 237.51 ns/order |
| Upgraded, retained trade ledger | 5.361 million orders/s | 186.54 ns/order |
| Upgraded, trade digest only | 5.298 million orders/s | 188.76 ns/order |

The retained-ledger upgrade was about **27% faster in this session**, while adding validation and richer execution identities. These are workload measurements, not a promise across laptops or workloads. Engine behavior changed: the original stamps every trade with a wall clock; the new engine uses deterministic event time and a rolling trade digest. Its trade records are also larger. The two builds process the same RNG workload but do not have identical feature sets. Small differences between retention modes are within the variation seen in these runs.

Independent-book scaling used **500,000 orders per worker**, digest-only mode, three trials per worker count:

| Workers / independent books | Median aggregate throughput | Relative to 1 worker |
|---|---:|---:|
| 1 | 6.473 million orders/s | 1.00x |
| 2 | 11.608 million orders/s | 1.79x |
| 4 | 18.954 million orders/s | 2.93x |
| 8 | 35.550 million orders/s | 5.49x |

Scaling uses a different per-book workload size from the single-book table. These are aggregate results over independent books, not 35 million operations on one shared book. Checksums agreed for every worker. Windows scheduling, background load, power mode, and temperature affect repeatability; these were ordinary laptop-session measurements, with no core isolation or power-policy changes.

The original README's Apple Silicon result omitted the chip model, compiler, power state, distribution statistics, and raw results. It cannot establish that one processor is faster. Its `122 ns/order` was elapsed time divided by operations, not measured network tick-to-trade latency. The original code also had no fee implementation, no instrument check, and tree lookups during cancellation despite stronger README claims. The original README remains available in `original/README.md`.

The Ryzen CPU is the useful resource for individual matching books. The RTX 4060 is a candidate for a later batch-learning or large scenario-evaluation layer; no GPU acceleration is claimed or implemented here. The current code demonstrates the CPU parallelism directly.

## Build and run

Requires a C++20 compiler, CMake 3.20+, and optionally Ninja. Python 3 adds report integration tests; the C++ tests work without it.

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOB_NATIVE=ON
cmake --build build/release --parallel 4
ctest --test-dir build/release --output-on-failure
.\build\release\limit_order_book.exe --help
```

If CMake does not select your compiler, add `-DCMAKE_CXX_COMPILER=D:/Tools/msys64/mingw64/bin/g++.exe` when configuring a new build directory. Visual Studio generators are also supported; use `--config Release` and run the executable from their `Release` subdirectory. `LOB_NATIVE=ON` optimizes GCC/Clang builds for the build machine; use the default OFF for a more portable binary. The option currently has no extra effect on MSVC.

The Windows helper handles the compiler path and uses a separate portable build directory when passed `-Portable`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-lab.ps1 -Mode test
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-lab.ps1 -Mode simulate -Scenario rally
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-lab.ps1 -Mode all
```

Individual commands:

```powershell
.\build\release\original_benchmark.exe
.\build\release\limit_order_book.exe benchmark --orders 1000000 --repeats 5 --output reports/benchmark.csv
.\build\release\limit_order_book.exe scale --orders 500000 --workers 8 --output reports/scaling.csv
.\build\release\limit_order_book.exe simulate --scenario shock --quantity 3000 --steps 240 --seed 42 --output reports/replay.html
.\build\release\limit_order_book.exe simulate --scenario rally --fee-bps 2 --output reports/rally-fees.html
.\build\release\limit_order_book.exe experiment --scenario calm --seeds 30 --output reports/experiment-calm.csv
```

Run the simulator from the project or build directory so the report template is available. The local build also embeds a fallback source path. Keep `assets/replay.html` alongside the project/build when moving it. Report HTML files themselves are self-contained and can be moved or shared independently. Each simulation also writes a matching JSON sidecar. Running a command again overwrites its chosen report.

## How the replay works

The three policies buy the same target quantity:

1. **Immediate:** sweep available asks at step zero. Any shortfall remains unfilled.
2. **TWAP:** aim for an equal cumulative quantity per step, carry missed quantity forward, and sweep remaining quantity on the final step.
3. **Liquidity-aware:** experimental heuristic that sizes children using current top-five bid/ask imbalance, a trailing midpoint average, available ask depth, and time remaining. It does not read future events. It sweeps any remaining target on the final step.

The background tape contains a seeded integer random walk, passive quote replenishment, and small market orders. There are 20 price levels on each side. Passive quotes refresh every four steps; the shock also forces a refresh, drops the reference by 35 ticks halfway through, and temporarily cuts depth. Rally adds an upward tick every three steps. These are event steps, not calibrated wall-clock intervals. Policies all receive the exact same exogenous event tape; their executions change their own subsequent book state until liquidity is refreshed.

This is a controlled toy market. There is no strategic response from other traders, persistent impact model, cross-venue routing, hidden liquidity, network delay, or calibration to real data. The plotted synthetic reference is visible for explanation; policies use only current/past book state. The adaptive method is a candidate to investigate, not a claim of a new algorithm or a trading edge.

Prices and quantities are integers. Buy-side slippage is `10,000 * (VWAP / arrival_midpoint - 1)`, with arrival midpoint 10,000 ticks. Lower is cheaper for these buy-only experiments. VWAP excludes fees; configurable taker fees are shown separately in tick-units of notional. Unfilled targets must be considered before comparing VWAP; no opportunity-cost model is imposed.

Thirty-seed experiments (42..71), 3,000 units, 240 steps, zero fees, all policies fully filled:

| Scenario | TWAP mean savings vs Immediate | Liquidity-aware mean savings vs Immediate |
|---|---:|---:|
| Calm | +7.425 bps | +7.466 bps |
| Shock | +24.851 bps | +24.012 bps |
| Rally | -31.575 bps | -48.809 bps |

Positive savings mean lower execution cost. The adaptive heuristic loses badly in a rally; waiting for cheaper prices can defer too much into an expensive final sweep. These results describe the chosen synthetic worlds, not expected live returns. The experiment command reports completion counts, paired fully-filled comparisons, win counts, and sample standard deviations. If either policy misses its target, that pair is excluded from cost comparison and its fill rate remains in the CSV.

## Validation and boundaries

C++ tests use checks that remain active in Release. Nine groups cover price-time priority, fills, trader/order identities, cancellations, reductions, invalid inputs without mutation, order modifiers, snapshots, deterministic replay, instrument isolation, and uint64 quantity limits. A separate vector-scan reference model checks **12,000 randomized events across three seeds**, including depth, order state, full fill tape, and conservation.

Python integration tests validate exported JSON and embedded HTML data, deterministic repeats, common scenario paths, monotonic fills, execution accounting, VWAP/slippage, fee invariance, explicit incomplete targets, CSV experiment pairing, CLI errors, output validation, and seed boundaries. Browser QA covers report controls and responsive rendering.

Benchmark input generation, book construction, trade preallocation, and final validation are outside throughput timers. Allocations during submissions remain inside. Sampled latency comes from a separate pass and includes clock overhead, timestamp quantization, cache effects, and OS noise. On this Windows toolchain, many clock differences are quantized to 100 ns and the back-to-back clock median can be zero; that does not mean the clock is free. Percentiles are descriptive samples, not wire latency.

This is an educational research engine. It has no durable journal/recovery, risk gateway, self-trade prevention, authentication, networking, session protocol, or exception-transaction recovery. Memory allocation failures can interrupt a mutation; they are not transactionally rolled back. Cumulative traded quantity saturates at uint64 maximum. Limit quantities and price-level sums are checked before accepted mutation.

## Useful next experiments

- **Mixed-event performance:** add cancel/replace-heavy and sparse-price traces, track allocation counts and memory high-water marks, and profile before replacing the tree with a bounded tick ladder.
- **Durable event tape:** explicit event serialization, checkpoints, deterministic crash replay, and state comparison after restart.
- **Market ecology:** add independent inventory-sensitive makers and latency-delayed takers, then test if policies survive changing agent behavior and persistent impact.
- **Adaptive-policy ablations:** compare depth-only, urgency-only, trend-aware, and current schedules on held-out seeds; penalize shortfalls, fees, and final-sweep risk.
- **GPU research layer:** batch many independent scenarios or train a policy using the RTX 4060. Measure transfer/setup costs and compare against the multicore CPU baseline before making acceleration claims.
- **Real-data replay:** import a documented historical feed, define event ordering and integer tick conversion, and distinguish observed historical flow from hypothetical strategy impact.

The engine follows common price/time priority and IOC/FOK concepts; venue-specific rules differ. References: [Nasdaq market model](https://www.nasdaq.com/docs/2025/03/03/NEQD-Market-Model-3.1.8.pdf) and [Nasdaq equity trading overview](https://www.nasdaqtrader.com/trader.aspx?id=TradingUSEquities).

Original source headers credit Oscar Zhang. Existing attribution and the original files have been retained.
