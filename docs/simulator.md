# Synthetic execution simulator

The replay lab compares three buy policies under the same seeded exogenous event tape. Each policy runs in its own order book, so its trades affect its subsequent available liquidity. The policies observe only current and past book state; the reference price path is plotted for explanation.

## Generate a replay or experiment

From the repository root after building, using Linux/macOS paths:

```sh
./build/limit_order_book simulate --scenario shock --quantity 3000 --steps 240 --seed 42 --output reports/replay.html
./build/limit_order_book simulate --scenario rally --fee-bps 2 --output reports/rally-fees.html
./build/limit_order_book experiment --scenario calm --seeds 30 --output reports/experiment-calm.csv
```

For Visual Studio builds on Windows, substitute `.\build\Release\limit_order_book.exe`; for Windows Ninja/MinGW, use `.\build\limit_order_book.exe`. The self-contained HTML report and matching JSON sidecar are generated outputs. Open the HTML in a browser; it needs no server or external assets. The [included replay example](../examples/replay.html) can also be opened locally. The template is embedded during the build, so the executable can generate reports from any working directory when its compiler runtime libraries are available. Rebuilding after template changes refreshes the embedded report.

The report provides playback, keyboard-accessible time scrubbing, policy selection, cumulative execution VWAP paths, the synthetic fair-price path, depth snapshots, fill quantities, and end-of-session accounting. Exported JSON contains the metadata, policy outcomes, and frame-by-frame snapshots. CSV experiments retain results for each seed/policy pairing.

## Policy behavior

1. **Immediate:** submit a market buy for the target at step zero. Any shortfall remains unfilled; it is not retried.
2. **TWAP:** target an equal cumulative quantity per step. Carry missed quantities forward, then sweep the remaining target on the final step.
3. **Liquidity-aware:** size child orders using current top-five bid/ask imbalance, a trailing midpoint average, visible ask depth, and time remaining. Sweep any remaining target on the final step.

The liquidity-aware policy is an experimental causal heuristic built from familiar scheduling and order-book signals. It does not establish a new algorithm, a proven execution advantage, or a trading edge. Its urgency and trend rules can defer too much quantity into a costly final sweep.

## Background flow and scenarios

Prices and quantities are integers. The arrival reference is 10,000 ticks. There are up to 20 passive levels on each side, plus small background market orders. Passive quotes refresh every four steps. The tape is generated before running any policy, and all policies receive the same exogenous events.

- **Calm:** an integer random walk in the reference price with regular quote replenishment.
- **Rally:** the same base mechanism with an extra upward tick every three steps.
- **Shock:** a 35-tick reference-price drop halfway through the session, a forced quote refresh, and temporarily reduced depth.

Event steps are abstract simulation steps, not calibrated clock intervals. Refreshes replace background passive quotes, so their effect can dominate persistent execution impact. Between refreshes, each policy's own fills change its book, and the common background orders can consequently execute differently.

These rules define controlled toy markets. They contain no strategic response from other traders, calibrated persistent impact, cross-venue routing, hidden liquidity, queue-latency model, network delay, or real-data calibration. The fair-price path is not an input to the execution policies.

## Accounting and incomplete targets

VWAP is total executed notional divided by filled quantity and excludes fees. An unfilled order has no execution VWAP. Buy-side slippage is:

```text
slippage_bps = 10,000 × (VWAP / arrival_midpoint − 1)
```

Positive slippage means a higher buy price than the arrival reference. The configurable taker fee is expressed in basis points of executed notional and displayed separately in `tick × quantity` units. It is not a currency amount unless an external tick value is defined. Mean fill step is weighted by executed quantity.

Completion must be considered before comparing execution costs. A low VWAP on a partial target does not establish better execution; the simulator imposes no opportunity-cost or shortfall penalty. Immediate does not retry its shortfall, while TWAP and the heuristic continue scheduling and attempt a final sweep. This behavior is deliberate and visible in results.

Multi-seed comparisons pair each policy with Immediate on the same seed and include a cost comparison only when both complete the full target. Completion counts remain reported for every policy. Summaries include paired mean savings, sample standard deviation, and win counts. Excluding incomplete pairs can change the population being compared and must be reported with the result.

## Development experiment

Thirty common seeds, **42..71**, were run for each scenario with a target of **3,000 units**, **240 steps**, and **zero fees**. All policies completed their targets in these runs.

| Scenario | TWAP mean savings vs Immediate | Liquidity-aware mean savings vs Immediate |
|---|---:|---:|
| Calm | +7.425 bps | +7.466 bps |
| Shock | +24.851 bps | +24.012 bps |
| Rally | −31.575 bps | −48.809 bps |

Raw paired outcomes: [calm](../examples/measurements/experiment-calm.csv), [shock](../examples/measurements/experiment-shock.csv), and [rally](../examples/measurements/experiment-rally.csv).

Positive savings mean cheaper execution for the delayed policy; negative values mean it cost more. Waiting helps in the selected falling-price worlds and hurts in the rising-price world. The adaptive heuristic performs particularly poorly in the rally because delaying purchases can push quantity into an expensive final sweep. These numbers describe the chosen synthetic worlds and are not expected live returns or statistical evidence of an edge.

Useful follow-ups include held-out seeds, sensitivity to refill frequency and fees, depth-only/urgency-only/trend-only ablations, explicit shortfall penalties, persistent impact, and independently responding market-making agents. Real-data replay requires a documented feed, ordering conventions, tick conversion, and a clear distinction between observed historical events and hypothetical strategy impact.

## Validation

Integration checks compare repeated JSON exports, verify a common exogenous reference path, enforce monotonic fills, reconcile executed quantities and notional, check VWAP/slippage formulas, verify fee invariance of policy paths, and exercise incomplete targets and multi-seed pairing. Report browser checks cover playback, slider and keyboard input, policy selection, and narrow/mobile layouts.

The core engine follows common price/time priority and IOC/FOK concepts; actual venue rules can differ. Background references: [Nasdaq market model](https://www.nasdaq.com/docs/2025/03/03/NEQD-Market-Model-3.1.8.pdf) and [Nasdaq equity trading overview](https://www.nasdaqtrader.com/trader.aspx?id=TradingUSEquities).
