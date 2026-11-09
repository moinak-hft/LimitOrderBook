# Contributing

Start with a reproducible problem, a focused change, and evidence that the resulting behavior is correct. For a new policy or substantial engine redesign, explain the intended behavior and comparison before expanding the implementation.

## Build and test

Use a C++20 compiler, CMake 3.20 or newer, and Python 3.10 or newer. Python is required for the report integration suite; it needs no third-party packages.

From the repository root:

```sh
cmake -S . -B build/contrib -DCMAKE_BUILD_TYPE=Release -DLOB_NATIVE=OFF -DBUILD_TESTING=ON
cmake --build build/contrib --config Release --parallel 2
ctest --test-dir build/contrib -C Release --output-on-failure
```

The workflow in `.github/workflows/ci.yml` runs these checks on Linux, Windows, and macOS. Keep the default build portable. Enable `LOB_NATIVE` only for a clearly identified local performance experiment.

For a demo on Linux or macOS:

```sh
./build/contrib/limit_order_book simulate --scenario shock --seed 42 --output reports/contribution.html
```

For a Visual Studio build on Windows:

```powershell
.\build\contrib\Release\limit_order_book.exe simulate --scenario shock --seed 42 --output reports/contribution.html
```

A Windows Ninja build places the executable directly in `build/contrib`. Open the generated HTML report and check the timeline, policy selection, depth display, and incomplete-fill behavior when changing the report or simulator.

## Correctness comes first

- Keep one owning thread and one instrument per book. Parallel workloads should use independent books.
- Preserve price/time priority, maker-price execution, and the documented order-modifier behavior. Quantity reductions retain priority; repricing or increasing quantity requires cancel and resubmit.
- Validate rejected requests before changing book state. Keep order IDs and trader IDs distinct.
- Make replay behavior deterministic and keep strategies causal: they may observe current and past book state, without reading future generated events.
- Add a regression test that would fail for the reported bug. The C++ suite includes an independent reference model and conservation checks; retain checks that remain active in Release builds.
- When changing exported reports or CLI behavior, run the Python integration suite through CTest. Check fees, shortfalls, and final state alongside execution cost.

Preserve existing author credits and the upstream snapshot in `original/`. Record the source and license of any imported code or data, and include only material you are entitled to contribute.

## Performance and policy evidence

Establish correct behavior and a working demo before making performance claims. Compare builds on the same machine and workload, and include raw CSV results with the exact commands, CPU, operating system, compiler/version, build flags, seed, order count, trade-retention mode, warmup, and repeated trial count. Explain any changed functionality that affects the comparison.

Report repeated measurements and variability. Elapsed time divided by operations is amortized processing time; sampled per-operation latency includes clock and sampling overhead. Independent-book aggregate throughput does not describe concurrent writers to one shared book. Shared CI runners are for correctness checks, not stable performance thresholds.

For strategy comparisons, use paired seeds and report target completion, fees, and execution cost together. Include scenarios where the proposed policy performs poorly and explain the synthetic market assumptions. A favorable result in this simulator does not establish a live trading advantage.

## Pull requests

Explain the concrete problem, resulting behavior, relevant tradeoffs, and validation commands/results. Include a screenshot when changing report presentation and raw measurements when claiming an improvement. Keep generated builds, local hardware inventories, and personal paths out of the change. Small changes that reviewers can reproduce are easier to assess.
