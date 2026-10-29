#!/usr/bin/env python3
"""Plot measured sensitivity to the hotness sampling rate."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

SAMPLING_RATES = (0.1, 0.2, 0.3, 0.5, 0.8, 1.0)
SERIES = ("ycsb_c_099", "ycsb_c_05", "twitter_storage")
STYLES = {
    "ycsb_c_099": {"marker": "o", "linestyle": "-", "color": "#E69F00",
                   "label": r"YCSB-C ($\theta=0.99$)"},
    "ycsb_c_05": {"marker": "s", "linestyle": "--", "color": "#009E73",
                  "label": r"YCSB-C ($\theta=0.5$)"},
    "twitter_storage": {"marker": "^", "linestyle": "-", "color": "#56B4E9",
                        "label": "Twitter-Storage"},
}
DEFAULT_LIMITS = (150.0, 460.0)
PROFILES = {
    "ycsb_c_099": ("c", 0.99),
    "ycsb_c_05": ("c", 0.5),
    "twitter_storage": ("ts", None),
}

class ValidationError(ValueError):
    """The input is not a complete measured sensitivity study."""


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def require_integer(value: Any, expected: int, name: str) -> None:
    if type(value) is not int or value != expected:
        raise ValidationError(f"{name} must be the integer {expected}.")


def require_positive(value: Any, name: str) -> float:
    if type(value) not in (int, float):
        raise ValidationError(f"{name} must be a positive finite number.")
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise ValidationError(f"{name} must be a positive finite number.")
    return number


def validate_receipt(receipt: dict[str, Any], name: str, sampling_rate: float,
                     throughput: float) -> None:
    """Keep plotted values tied to the completed experiment receipts."""
    if receipt.get("validated") is not True or type(receipt.get("exit_code")) is not int or receipt["exit_code"] != 0:
        raise ValidationError(f"{name}: the run receipt must record a validated successful run.")
    config = receipt.get("config")
    if not isinstance(config, dict):
        raise ValidationError(f"{name}: a run configuration is required.")
    for key, expected in (("num_threads", 30), ("num_bg", 1), ("num_rounds", 10),
                          ("num_load", 100_000_000), ("num_run", 100_000_000)):
        require_integer(config.get(key), expected, f"{name}: config {key}")
    workload, skewness = PROFILES[name]
    if config.get("series_id") != name or config.get("workload") != workload or config.get("skewness") != skewness:
        raise ValidationError(f"{name}: the run profile differs from the plotted series.")
    rate = require_positive(config.get("sampling_rate"), f"{name}: configured sampling_rate")
    if not math.isclose(rate, sampling_rate, rel_tol=0.0, abs_tol=1e-12):
        raise ValidationError(f"{name}: the run sampling rate differs from its plotted value.")
    command = config.get("command")
    if not isinstance(command, list) or not command or any(not isinstance(arg, str) for arg in command):
        raise ValidationError(f"{name}: the explicit run command is required.")
    flags = [arg for arg in command if arg.startswith("-timnah_sample_rate=")]
    if len(flags) != 1:
        raise ValidationError(f"{name}: exactly one hotness sampling flag is required.")
    try:
        command_rate = require_positive(float(flags[0].split("=", 1)[1]), f"{name}: command sampling_rate")
    except ValueError as error:
        raise ValidationError(f"{name}: invalid sampling rate in the explicit command.") from error
    if not math.isclose(command_rate, sampling_rate, rel_tol=0.0, abs_tol=1e-12):
        raise ValidationError(f"{name}: the command sampling rate differs from its plotted value.")
    for flag in ("timnah_cross_multiplier", "timnah_intra_multiplier"):
        values = [arg for arg in command if arg.startswith("-" + flag + "=")]
        try:
            valid = len(values) == 1 and float(values[0].split("=", 1)[1]) == 2.0
        except ValueError:
            valid = False
        if not valid:
            raise ValidationError(f"{name}: {flag} must remain fixed at 2.")
    rounds = receipt.get("rounds")
    if not isinstance(rounds, list) or len(rounds) != 10:
        raise ValidationError(f"{name}: all ten round records are required.")
    for index, record in enumerate(rounds):
        if not isinstance(record, dict):
            raise ValidationError(f"{name}: each round record must be an object.")
        require_integer(record.get("round"), index, f"{name}: receipt round")
        require_positive(record.get("throughput_Mops"), f"{name}: receipt throughput_Mops")
    if not math.isclose(float(rounds[9]["throughput_Mops"]), throughput, rel_tol=0.0, abs_tol=1e-12):
        raise ValidationError(f"{name}: the plotted throughput differs from the measured round 9 value.")


def validate_dataset(data: Any) -> dict[str, list[float]]:
    """Require one measured round 9 sample for every series and sampling rate."""
    if not isinstance(data, dict):
        raise ValidationError("The JSON root must be an object.")
    for name, expected in (("schema_version", 1), ("num_threads", 30), ("num_bg", 1),
                           ("num_rounds", 10), ("num_load", 100_000_000),
                           ("num_run", 100_000_000), ("round", 9)):
        require_integer(data.get(name), expected, name)
    if data.get("metric") != "round9_throughput":
        raise ValidationError("The metric must be round9_throughput.")
    if data.get("index") != "Timnah":
        raise ValidationError("The index must be Timnah.")
    if data.get("complete") is not True:
        raise ValidationError("The sensitivity study is incomplete.")
    sampling_rates = data.get("sampling_rates")
    if not isinstance(sampling_rates, list) or len(sampling_rates) != len(SAMPLING_RATES):
        raise ValidationError("sampling_rates must contain the complete sampling rate grid.")
    for position, (value, expected) in enumerate(zip(sampling_rates, SAMPLING_RATES)):
        number = require_positive(value, f"sampling_rates[{position}]")
        if not math.isclose(number, expected, rel_tol=0.0, abs_tol=1e-12):
            raise ValidationError("sampling_rates do not match the required ordered sampling rate grid.")
    series, runs = data.get("series"), data.get("runs")
    if not isinstance(series, dict) or set(series) != set(SERIES):
        raise ValidationError("The dataset must contain exactly the requested series.")
    if not isinstance(runs, dict) or set(runs) != set(SERIES):
        raise ValidationError("A run-receipt array is required for every series.")
    curves: dict[str, list[float]] = {}
    for name in SERIES:
        rows, receipts = series[name], runs[name]
        if not isinstance(rows, list) or len(rows) != len(SAMPLING_RATES):
            raise ValidationError(f"{name}: one record per sampling rate is required.")
        if not isinstance(receipts, list) or len(receipts) != len(SAMPLING_RATES):
            raise ValidationError(f"{name}: one run receipt per sampling rate is required.")
        values = []
        for position, (row, receipt, expected) in enumerate(zip(rows, receipts, SAMPLING_RATES)):
            if not isinstance(row, dict):
                raise ValidationError(f"{name}, point {position}: the measurement must be an object.")
            if not isinstance(receipt, dict) or not receipt:
                raise ValidationError(f"{name}, point {position}: a nonempty run receipt is required.")
            require_integer(row.get("round"), 9, f"{name}, point {position}: round")
            sampling_rate = require_positive(row.get("sampling_rate"), f"{name}, point {position}: sampling_rate")
            if not math.isclose(sampling_rate, expected, rel_tol=0.0, abs_tol=1e-12):
                raise ValidationError(f"{name}: sampling rates must be unique and in the required order.")
            throughput = require_positive(row.get("throughput_Mops"),
                                          f"{name}, sampling_rate={expected}: throughput_Mops")
            validate_receipt(receipt, name, expected, throughput)
            values.append(throughput)
        curves[name] = values
    return curves


def load_dataset(path: Path) -> dict[str, list[float]]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle, object_pairs_hook=unique_object)
    return validate_dataset(data)


def axis_limits(curves: dict[str, list[float]], expand_y: bool) -> tuple[float, float]:
    values = [value for name in SERIES for value in curves[name]]
    low, high = DEFAULT_LIMITS
    smallest, largest = min(values), max(values)
    if not expand_y and (smallest < low or largest > high):
        raise ValidationError(
            f"Measured throughput spans {smallest:.6f} to {largest:.6f} Mops/s, "
            "outside the 150 to 460 Mops/s display range. Use --expand-y to show all values."
        )
    if expand_y:
        if smallest < low:
            low = max(0.0, math.floor(smallest * 0.9 / 50.0) * 50.0)
        if largest > high:
            high = math.ceil(largest * 1.05 / 50.0) * 50.0
    return low, high


def draw_figure(plt: Any, curves: dict[str, list[float]], limits: tuple[float, float]) -> Any:
    """Draw measured sampling rate sweeps with consistent markers and typography."""
    plt.rcParams.update({
        "font.size": 14,
        "font.weight": "bold",
        "axes.labelsize": 16,
        "axes.titlesize": 16,
        "axes.labelweight": "bold",
        "xtick.labelsize": 14,
        "ytick.labelsize": 14,
        "legend.fontsize": 11,
    })
    fig, ax = plt.subplots(figsize=(3.2, 2.55))
    for name in SERIES:
        style = STYLES[name]
        ax.plot(
            list(SAMPLING_RATES), curves[name],
            marker=style["marker"],
            linestyle=style["linestyle"],
            color=style["color"],
            label=style["label"],
            linewidth=2.5,
            markersize=5.5,
        )
    ax.set_xlabel("Sampling Rate")
    ax.set_ylabel("Throughput\n(Mops)")
    ax.set_xticks([0.1, 0.3, 0.5, 0.8, 1.0])
    ax.set_yticks([150, 250, 350, 450])
    ax.set_xlim(0.08, 1.02)
    ax.set_ylim(*limits)
    ax.grid(True, linestyle="--", alpha=0.6, linewidth=0.8)
    ax.legend(
        loc="lower center",
        bbox_to_anchor=(0.5, 1.015),
        ncol=1,
        frameon=True,
        framealpha=1.0,
        edgecolor="black",
        labelspacing=0.15,
        handlelength=1.15,
        handletextpad=0.30,
        borderpad=0.25,
        borderaxespad=0.0,
    )
    fig.subplots_adjust(left=0.24, right=0.98, bottom=0.24, top=0.67)
    return fig


def english_text(value: Any) -> str:
    return str(value).encode("ascii", "backslashreplace").decode("ascii")


class EnglishArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        super().error(english_text(message))


def parser() -> argparse.ArgumentParser:
    directory = Path(__file__).resolve().parent
    result = EnglishArgumentParser(description=__doc__)
    result.add_argument("--data", type=Path, default=directory / "figure16-sampling-rate.json")
    result.add_argument("--output", type=Path, default=directory / "Figure 16.pdf")
    result.add_argument("--validate-only", action="store_true", help="Validate measurements without drawing a figure.")
    result.add_argument("--show", action="store_true", help="Show the figure after saving the PDF.")
    limits = result.add_mutually_exclusive_group()
    limits.add_argument("--expand-y", dest="expand_y", action="store_true",
                        help="Expand the display range only when measurements exceed it (default).")
    limits.add_argument("--original-y-limits", dest="expand_y", action="store_false",
                        help="Require all measurements to fit the 150 to 460 Mops/s display range.")
    result.set_defaults(expand_y=True)
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    curves = load_dataset(args.data)
    limits = axis_limits(curves, args.expand_y)
    print(f"Validated {len(SERIES) * len(SAMPLING_RATES)} measured round 9 records.")
    if limits != DEFAULT_LIMITS:
        print(f"Display y-axis expanded to {limits[0]:g} through {limits[1]:g} Mops/s to include all measured values.")
    if args.validate_only:
        return 0
    import matplotlib
    if not args.show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figure = draw_figure(plt, curves, limits)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output, format="pdf", bbox_inches="tight", pad_inches=0.02)
    print("Saved PDF: " + english_text(args.output))
    if args.show:
        plt.show()
    plt.close(figure)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, ImportError) as error:
        print("Error: " + english_text(error), file=sys.stderr)
        raise SystemExit(1)

