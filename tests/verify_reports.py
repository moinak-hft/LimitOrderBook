"""Integration checks for a built Replay Lab executable (Python standard library).

Usage: python tests/verify_reports.py build/release/limit_order_book.exe
All generated reports live in a temporary directory and are removed on success.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile


POLICIES = {"Immediate", "TWAP", "Liquidity-aware"}


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def close(actual: float, expected: float, label: str) -> None:
    check(math.isfinite(actual), f"{label}: nonfinite value {actual}")
    check(math.isclose(actual, expected, rel_tol=2e-9, abs_tol=2e-7),
          f"{label}: {actual} != {expected}")


def validate_report(data: dict) -> None:
    meta = data["meta"]
    target, steps, arrival = meta["target"], meta["steps"], meta["arrivalMid"]
    runs = data["runs"]
    check(len(runs) == 3 and {run["name"] for run in runs} == POLICIES,
          "report must contain exactly the three comparison policies")
    fair_paths = [[frame["fair"] for frame in run["frames"]] for run in runs]
    check(all(path == fair_paths[0] for path in fair_paths),
          "policies must share the same exogenous fair-price path")
    check(fair_paths[0][0] == arrival, "initial fair price equals arrival midpoint")

    for run in runs:
        label = f"{meta['scenario']} / {run['name']}"
        frames = run["frames"]
        check(len(frames) == steps, f"{label}: one frame per step")
        check(isinstance(run["checksum"], str) and run["checksum"].isdigit(),
              f"{label}: checksum must retain full integer precision as text")
        check(0 <= run["filled"] <= target, f"{label}: target fill bound")
        check(run["unfilled"] == target - run["filled"], f"{label}: explicit unfilled amount")
        check(run["complete"] is (run["filled"] == target), f"{label}: explicit completion flag")
        previous_filled = 0
        previous_notional = 0.0
        weighted_step = 0
        for index, frame in enumerate(frames):
            where = f"{label} frame {index}"
            check(frame["step"] == index, f"{where}: contiguous step numbering")
            check(previous_filled <= frame["filled"] <= target, f"{where}: monotone bounded fills")
            check(frame["executed"] == frame["filled"] - previous_filled,
                  f"{where}: incremental/cumulative fill accounting")
            weighted_step += frame["executed"] * index
            if frame["filled"]:
                check(frame["vwap"] is not None and frame["vwap"] > 0, f"{where}: positive VWAP")
                notional = frame["filled"] * frame["vwap"]
                if frame["executed"] == 0:
                    close(notional, previous_notional, f"{where}: unchanged VWAP without fills")
                else:
                    # Positive prices imply increasing cumulative purchase notional.
                    check(notional > previous_notional, f"{where}: positive incremental notional")
                previous_notional = notional
            else:
                check(frame["vwap"] is None, f"{where}: unfilled VWAP must be null")
            for side, descending in (("bids", True), ("asks", False)):
                levels = frame[side]
                prices = [level[0] for level in levels]
                check(prices == sorted(set(prices), reverse=descending),
                      f"{where}: sorted distinct {side}")
                check(all(price > 0 and quantity > 0 for price, quantity in levels),
                      f"{where}: positive resting prices/quantities")
                quote = frame["bid" if descending else "ask"]
                check(quote == (prices[0] if prices else None), f"{where}: quote matches depth")
            if frame["bid"] is not None and frame["ask"] is not None:
                check(frame["bid"] < frame["ask"], f"{where}: uncrossed book")
            previous_filled = frame["filled"]

        check(sum(frame["executed"] for frame in frames) == run["filled"],
              f"{label}: total execution conservation")
        check(frames[-1]["filled"] == run["filled"], f"{label}: final frame matches summary")
        if run["filled"]:
            close(run["vwap"], frames[-1]["vwap"], f"{label}: summary VWAP")
            close(run["slippageBps"], (run["vwap"] / arrival - 1) * 10000,
                  f"{label}: arrival slippage")
            close(run["fee"], run["vwap"] * run["filled"] * meta["feeBps"] / 10000,
                  f"{label}: notional fee")
            close(run["averageStep"], weighted_step / run["filled"], f"{label}: fill-weighted timing")
        else:
            check(run["vwap"] is None and run["slippageBps"] is None,
                  f"{label}: no-fill execution metrics are null")
            check(run["fee"] == 0, f"{label}: no fee without fills")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="built limit_order_book executable")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    calls = 0

    def invoke(*arguments: str, expected: int = 0,
               executable: Path | None = None) -> subprocess.CompletedProcess[str]:
        nonlocal calls
        calls += 1
        result = subprocess.run([str(executable or binary), *map(str, arguments)], cwd=temporary,
                                capture_output=True, text=True, timeout=60)
        check((result.returncode == 0) == (expected == 0),
              f"Unexpected exit {result.returncode}: {' '.join(map(str, arguments))}\n"
              f"{result.stdout}\n{result.stderr}")
        return result

    with tempfile.TemporaryDirectory(prefix="lob-report-check-") as directory:
        temporary = Path(directory)

        def simulate(name: str, **options: object) -> tuple[dict, str]:
            destination = temporary / f"{name}.html"
            arguments = ["simulate", "--output", str(destination)]
            for key, value in options.items():
                arguments += ["--" + key.replace("_", "-"), str(value)]
            result = invoke(*arguments)
            check(destination.is_file() and destination.with_suffix(".json").is_file(),
                  f"{name}: HTML and JSON must both exist")
            data = json.loads(destination.with_suffix(".json").read_text(encoding="utf-8"))
            html = destination.read_text(encoding="utf-8")
            check("__LOB_DATA__" not in html, f"{name}: unreplaced template placeholder")
            prefix = "const data = "
            check(prefix in html, f"{name}: report must embed replay data")
            embedded, _ = json.JSONDecoder().raw_decode(html[html.index(prefix) + len(prefix):])
            check(embedded == data, f"{name}: embedded report and sidecar disagree")
            validate_report(data)
            check("Replay verified" in result.stdout, f"{name}: internal replay verification")
            return data, result.stdout

        calm, _ = simulate("calm", scenario="calm", steps=24, quantity=1200, seed=42)
        shock, _ = simulate("shock", scenario="shock", steps=48, quantity=1200, seed=42)
        rally, _ = simulate("rally", scenario="rally", steps=48, quantity=1200, seed=42)
        default, _ = simulate("default")
        check(default["meta"]["steps"] == 240, "default horizon remains 240 steps")
        repeated, _ = simulate("shock-again", scenario="shock", steps=48, quantity=1200, seed=42)
        check(repeated == shock, "same options must reproduce the entire report exactly")
        # Copy only the binary, without the source tree or adjacent assets, to
        # exercise the embedded report template from an unrelated working dir.
        relocated_directory = temporary / "relocated"
        relocated_directory.mkdir()
        relocated_binary = relocated_directory / binary.name
        shutil.copy2(binary, relocated_binary)
        relocated_report = temporary / "relocated.html"
        invoke("simulate", "--scenario", "shock", "--steps", "48", "--quantity", "1200",
               "--seed", "42", "--output", relocated_report, executable=relocated_binary)
        relocated = json.loads(relocated_report.with_suffix(".json").read_text(encoding="utf-8"))
        check(relocated == shock and relocated_report.is_file(),
              "relocated executable must render identical reports without source/assets")
        check(shock["runs"][0]["frames"][24]["fair"] < shock["runs"][0]["frames"][23]["fair"] - 30,
              "shock scenario must include a material downward shock")
        check(calm["meta"]["scenario"] == "calm" and rally["meta"]["scenario"] == "rally",
              "requested scenario metadata")

        huge, huge_output = simulate("huge", scenario="shock", steps=24, quantity=1000000, seed=42)
        check(all(not run["complete"] and run["unfilled"] > 0 for run in huge["runs"]),
              "unachievable target must explicitly report incomplete outcomes")
        check("incomplete fills" in huge_output.lower(), "incomplete outcomes must be explained")

        fees, _ = simulate("fees", scenario="shock", steps=48, quantity=1200, seed=42, fee_bps=7.5)
        for free, paid in zip(shock["runs"], fees["runs"]):
            check(free["frames"] == paid["frames"] and free["checksum"] == paid["checksum"],
                  "fee accounting must not change market path, execution, or state")
            check(paid["fee"] > 0 and free["fee"] == 0, "positive fee changes only fee accounting")

        csv_path = temporary / "experiment.csv"
        invoke("experiment", "--scenario", "shock", "--steps", "48", "--quantity", "1200",
               "--seed", "42", "--seeds", "3", "--fee-bps", "7.5", "--output", csv_path)
        with csv_path.open(newline="", encoding="utf-8") as file:
            rows = list(csv.DictReader(file))
        check(len(rows) == 9, "three policies for each of three experiment seeds")
        for seed in (42, 43, 44):
            subset = [row for row in rows if int(row["seed"]) == seed]
            check(len(subset) == 3 and {row["policy"] for row in subset} == POLICIES,
                  f"seed {seed}: paired policy coverage")
        for row in rows:
            filled, target = int(row["filled"]), int(row["target"])
            check(0 <= filled <= target and row["scenario"] == "shock", "experiment fill bounds/metadata")
            close(float(row["fill_rate"]), filled / target, "experiment fill rate")
            if filled:
                vwap = float(row["vwap_ticks"])
                close(float(row["slippage_bps"]), (vwap / 10000 - 1) * 10000, "experiment slippage")
                close(float(row["fee_ticks_units"]), vwap * filled * 7.5 / 10000, "experiment fees")
            if int(row["seed"]) == 42:
                paired = next(run for run in fees["runs"] if run["name"] == row["policy"])
                check(filled == paired["filled"] and row["checksum"] == paired["checksum"],
                      "experiment and replay use identical per-seed executions")
                close(float(row["vwap_ticks"]), paired["vwap"], "experiment/replay VWAP")

        # Boundary seed is valid for a one-seed experiment; the next seed is not.
        invoke("experiment", "--seed", "4294967295", "--seeds", "1", "--steps", "24",
               "--output", temporary / "last-seed.csv")
        invoke("experiment", "--seed", "4294967295", "--seeds", "2", "--steps", "24",
               "--output", temporary / "overflow.csv", expected=1)
        check(not (temporary / "overflow.csv").exists(), "invalid seed span must not create output")

        bad_values = [
            ["--unknown", "1"], ["--seed", "1", "--seed", "2"],
            ["--steps", "12x"], ["--steps", "0"], ["--quantity", "-1"],
            ["--quantity", "1000001"], ["--seed", "4294967296"],
            ["--fee-bps", "nan"], ["--fee-bps", "-1"], ["--fee-bps", "1001"],
            ["--fee-bps", "2oops"], ["--scenario", "unknown"], ["--workers", "4"],
        ]
        for index, bad in enumerate(bad_values):
            destination = temporary / f"rejected-{index}.html"
            result = invoke("simulate", *bad, "--output", destination, expected=1)
            check("Error:" in result.stderr, "invalid CLI input should explain the error")
            check(not destination.exists() and not destination.with_suffix(".json").exists(),
                  "rejected CLI input must not generate reports")
        invoke("simulate", "--steps", expected=1)
        invoke("unknown-command", expected=1)
        for suffix in (".json", ".csv", ""):
            destination = temporary / ("keep-existing" + suffix)
            destination.write_text("preserve this destination", encoding="utf-8")
            invoke("simulate", "--steps", "24", "--output", destination, expected=1)
            check(destination.read_text(encoding="utf-8") == "preserve this destination",
                  "wrong report extension must not overwrite the destination")

    print(f"[PASS] Report, scenario, fee, experiment, deterministic replay and CLI checks ({calls} process runs)")


if __name__ == "__main__":
    main()
