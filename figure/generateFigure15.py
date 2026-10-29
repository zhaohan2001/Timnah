#!/usr/bin/env python3
"""Plot measured sensitivity to the cross-tier migration multiplier."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

K_VALUES = (1.0, 1.1, 1.3, 1.5, 1.7, 2.0, 2.2, 2.5, 3.0, 4.0, 5.0, 8.0, 10.0, 15.0)
SERIES = ("ycsb_c_099", "ycsb_c_05", "twitter_storage")
STYLES = {
    "ycsb_c_099": {"marker": "o", "linestyle": "-", "color": "#E69F00",
                   "label": r"YCSB-C ($\theta=0.99$)"},
    "ycsb_c_05": {"marker": "s", "linestyle": "--", "color": "#009E73",
                  "label": r"YCSB-C ($\theta=0.5$)"},
    "twitter_storage": {"marker": "^", "linestyle": "-", "color": "#56B4E9",
                        "label": "Twitter-Storage"},
}
DEFAULT_LIMITS = (100.0, 460.0)

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


def validate_dataset(data: Any) -> dict[str, list[float]]:
    """Require one measured round 9 sample for every series and multiplier."""
    if not isinstance(data, dict):
        raise ValidationError("The JSON root must be an object.")
    for name, expected in (("schema_version", 1), ("num_threads", 30), ("num_bg", 1),
                           ("num_rounds", 10), ("num_load", 100_000_000),
                           ("num_run", 100_000_000), ("round", 9)):
        require_integer(data.get(name), expected, name)
    if data.get("metric") != "round9_throughput":
        raise ValidationError("The metric must be round9_throughput.")
    if data.get("complete") is not True:
        raise ValidationError("The sensitivity study is incomplete.")
    k_values = data.get("k_values")
    if not isinstance(k_values, list) or len(k_values) != len(K_VALUES):
        raise ValidationError("k_values must contain the complete multiplier grid.")
    for position, (value, expected) in enumerate(zip(k_values, K_VALUES)):
        number = require_positive(value, f"k_values[{position}]")
        if not math.isclose(number, expected, rel_tol=0.0, abs_tol=1e-12):
            raise ValidationError("k_values do not match the required ordered multiplier grid.")
    series, runs = data.get("series"), data.get("runs")
    if not isinstance(series, dict) or set(series) != set(SERIES):
        raise ValidationError("The dataset must contain exactly the requested series.")
    if not isinstance(runs, dict) or set(runs) != set(SERIES):
        raise ValidationError("A run-receipt array is required for every series.")
    curves: dict[str, list[float]] = {}
    for name in SERIES:
        rows, receipts = series[name], runs[name]
        if not isinstance(rows, list) or len(rows) != len(K_VALUES):
            raise ValidationError(f"{name}: one record per multiplier is required.")
        if not isinstance(receipts, list) or len(receipts) != len(K_VALUES):
            raise ValidationError(f"{name}: one run receipt per multiplier is required.")
        values = []
        for position, (row, receipt, expected) in enumerate(zip(rows, receipts, K_VALUES)):
            if not isinstance(row, dict):
                raise ValidationError(f"{name}, point {position}: the measurement must be an object.")
            if not isinstance(receipt, dict) or not receipt:
                raise ValidationError(f"{name}, point {position}: a nonempty run receipt is required.")
            require_integer(row.get("round"), 9, f"{name}, point {position}: round")
            k = require_positive(row.get("k"), f"{name}, point {position}: k")
            if not math.isclose(k, expected, rel_tol=0.0, abs_tol=1e-12):
                raise ValidationError(f"{name}: multipliers must be unique and in the required order.")
            values.append(require_positive(row.get("throughput_Mops"),
                                           f"{name}, k={expected}: throughput_Mops"))
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
            "outside the 100 to 460 Mops/s display range. Use --expand-y to show all values."
        )
    if expand_y:
        if smallest < low:
            low = max(0.0, math.floor(smallest * 0.9 / 50.0) * 50.0)
        if largest > high:
            high = math.ceil(largest * 1.05 / 50.0) * 50.0
    return low, high


def draw_figure(plt: Any, curves: dict[str, list[float]], limits: tuple[float, float]) -> Any:
    """Draw measured multiplier sweeps with consistent markers and typography."""
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
            list(K_VALUES), curves[name],
            marker=style["marker"],
            linestyle=style["linestyle"],
            color=style["color"],
            label=style["label"],
            linewidth=2.5,
            markersize=5.5,
        )
    ax.set_xlabel("Threshold Multiplier ($k$)")
    ax.set_ylabel("Throughput\n(Mops)")
    ax.set_xticks([1.0, 4.0, 8.0, 15.0])
    ax.set_yticks([100, 250, 400])
    ax.set_xlim(0.8, 15.5)
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
    result.add_argument("--data", type=Path, default=directory / "figure15-cross-tier-multiplier.json")
    result.add_argument("--output", type=Path, default=directory / "Figure 15.pdf")
    result.add_argument("--validate-only", action="store_true", help="Validate measurements without drawing a figure.")
    result.add_argument("--show", action="store_true", help="Show the figure after saving the PDF.")
    limits = result.add_mutually_exclusive_group()
    limits.add_argument("--expand-y", dest="expand_y", action="store_true",
                        help="Expand the display range only when measurements exceed it (default).")
    limits.add_argument("--original-y-limits", dest="expand_y", action="store_false",
                        help="Require all measurements to fit the 100 to 460 Mops/s display range.")
    result.set_defaults(expand_y=True)
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    curves = load_dataset(args.data)
    limits = axis_limits(curves, args.expand_y)
    print(f"Validated {len(SERIES) * len(K_VALUES)} measured round 9 records.")
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

