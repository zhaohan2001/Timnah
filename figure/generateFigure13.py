#!/usr/bin/env python3
"""Plot the measured Timnah Figure 13 factor analysis without invented values."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any


METHODS = ("EH", "Tiered EH", "+K/V Lock", "+Data Migration", "+Hot Lock")
WORKLOADS = ("a", "c", "ts", "w")
WORKLOAD_LABELS = ("YCSB A", "YCSB C", "Twitter-Storage", "Webmail")
COLORS = ("white", "lightgray", "skyblue", "lightgreen", "lightcoral")
HATCHES = (".", "x", "\\\\", "-", "/")
ORIGINAL_THROUGHPUT_MAX = 780.0
ORIGINAL_LATENCY_LIMITS_US = (0.05, 8000.0)


class ValidationError(ValueError):
    """The input does not contain the complete measured experiment."""


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
        raise ValidationError(f"{name} must be a positive, finite number.")
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise ValidationError(f"{name} must be a positive, finite number.")
    return result


def validate_dataset(data: Any) -> dict[str, dict[str, dict[str, Any]]]:
    """Require all 20 measured round 9 records and their collection receipts."""
    if not isinstance(data, dict):
        raise ValidationError("The JSON root must be an object.")
    for key, expected in (
        ("schema_version", 1),
        ("num_threads", 30),
        ("num_rounds", 10),
        ("num_load", 100_000_000),
        ("num_run", 100_000_000),
        ("round", 9),
    ):
        require_integer(data.get(key), expected, key)
    if data.get("metric") != "round9_throughput_and_sampled_latency":
        raise ValidationError("Unexpected metric; round 9 throughput and sampled latency are required.")
    if data.get("complete") is not True:
        raise ValidationError("The experiment is incomplete; all 20 runs are required.")
    workloads = data.get("workloads")
    if not isinstance(workloads, dict) or set(workloads) != set(WORKLOADS):
        raise ValidationError("workloads must contain exactly a, c, ts, and w.")
    runs = data.get("runs")
    if not isinstance(runs, dict) or set(runs) != set(WORKLOADS):
        raise ValidationError("runs must contain receipts for a, c, ts, and w.")
    for workload in WORKLOADS:
        records = workloads[workload]
        if not isinstance(records, dict) or set(records) != set(METHODS):
            raise ValidationError(f"{workload}: exactly the five canonical ablation methods are required.")
        receipts = runs[workload]
        if not isinstance(receipts, dict) or set(receipts) != set(METHODS):
            raise ValidationError(f"{workload}: a receipt for every method is required.")
        for method in METHODS:
            label = f"{workload}, {method}"
            row = records[method]
            if not isinstance(row, dict):
                raise ValidationError(f"{label}: the measurement must be an object.")
            if not isinstance(receipts[method], dict) or not receipts[method]:
                raise ValidationError(f"{label}: the run receipt must be a nonempty object.")
            if "round" in row:
                require_integer(row["round"], 9, f"{label}: round")
            require_positive(row.get("throughput_Mops"), f"{label}: throughput_Mops")
            p50 = require_positive(row.get("p50_latency_ns"), f"{label}: p50_latency_ns")
            p99 = require_positive(row.get("p99_latency_ns"), f"{label}: p99_latency_ns")
            if p99 < p50:
                raise ValidationError(f"{label}: P99 latency must be at least P50 latency.")
    return workloads


def load_dataset(path: Path) -> dict[str, dict[str, dict[str, Any]]]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle, object_pairs_hook=unique_object)
    return validate_dataset(data)


def axis_limits(
    workloads: dict[str, dict[str, dict[str, Any]]], expand_y: bool
) -> tuple[float, tuple[float, float]]:
    rows = [workloads[workload][method] for workload in WORKLOADS for method in METHODS]
    throughput_max = max(float(row["throughput_Mops"]) for row in rows)
    latency_min_us = min(float(row["p50_latency_ns"]) for row in rows) / 1000.0
    latency_max_us = max(float(row["p99_latency_ns"]) for row in rows) / 1000.0
    throughput_limit = ORIGINAL_THROUGHPUT_MAX
    latency_low, latency_high = ORIGINAL_LATENCY_LIMITS_US
    outside = []
    if throughput_max > throughput_limit:
        outside.append(f"throughput {throughput_max:.6f} Mops/s exceeds 780 Mops/s")
    if latency_min_us < latency_low:
        outside.append(f"P50 latency {latency_min_us:.6f} us is below 0.05 us")
    if latency_max_us > latency_high:
        outside.append(f"P99 latency {latency_max_us:.6f} us exceeds 8000 us")
    if outside and not expand_y:
        raise ValidationError(
            "Measured values exceed the original axis limits: " + "; ".join(outside)
            + ". Use --expand-y to show all measured values."
        )
    if expand_y:
        if throughput_max > throughput_limit:
            throughput_limit = float(math.ceil(throughput_max * 1.05 / 100.0) * 100)
        if latency_min_us < latency_low:
            latency_low = latency_min_us * 0.8
        if latency_max_us > latency_high:
            latency_high = latency_max_us * 1.25
    return throughput_limit, (latency_low, latency_high)


def draw_figure(
    plt: Any,
    np: Any,
    Patch: Any,
    Line2D: Any,
    workloads: dict[str, dict[str, dict[str, Any]]],
    limits: tuple[float, tuple[float, float]],
) -> Any:
    """Plot measured round 9 throughput and sampled latency by design."""
    plt.rcParams.update({
        "font.size": 22,
        "axes.labelsize": 24,
        "xtick.labelsize": 22,
        "ytick.labelsize": 24,
        "axes.titlesize": 26,
    })
    throughput = {
        method: [float(workloads[code][method]["throughput_Mops"]) for code in WORKLOADS]
        for method in METHODS
    }
    # The JSON stores nanoseconds. Convert to microseconds exactly once here.
    p50_latency = {
        method: [float(workloads[code][method]["p50_latency_ns"]) / 1000.0 for code in WORKLOADS]
        for method in METHODS
    }
    p99_latency = {
        method: [float(workloads[code][method]["p99_latency_ns"]) / 1000.0 for code in WORKLOADS]
        for method in METHODS
    }
    fig, ax1 = plt.subplots(figsize=(15, 3.8))
    ax2 = ax1.twinx()
    x = np.arange(len(WORKLOADS))
    total_width = 0.85
    bar_width = total_width / len(METHODS)
    for i, method in enumerate(METHODS):
        offset = (i - (len(METHODS) - 1) / 2) * bar_width
        ax1.bar(
            x + offset,
            throughput[method],
            width=bar_width,
            color=COLORS[i],
            hatch=HATCHES[i],
            edgecolor="black",
            linewidth=1.0,
            zorder=3,
        )
    for i in range(len(WORKLOADS)):
        x_offset = x[i] + np.linspace(
            -total_width / 2 + bar_width / 2, total_width / 2 - bar_width / 2, len(METHODS)
        )
        ax2.plot(
            x_offset,
            [p50_latency[method][i] for method in METHODS],
            color="black",
            marker="o",
            linestyle="-",
            linewidth=2.5,
            markersize=8,
            zorder=4,
        )
        ax2.plot(
            x_offset,
            [p99_latency[method][i] for method in METHODS],
            color="gray",
            marker="^",
            linestyle="--",
            linewidth=2.5,
            markersize=8,
            zorder=4,
        )
    throughput_limit, latency_limits = limits
    ax1.set_xlabel("Workload", fontsize=24, fontweight="bold")
    ax1.set_ylabel("Throughput\n(Mops/s)", fontsize=22, fontweight="bold")
    ax1.set_ylim(0, throughput_limit)
    ax1.set_yticks([0, 200, 400, 600])
    ax1.set_xticks(x)
    ax1.set_xticklabels(list(WORKLOAD_LABELS), fontsize=22, fontweight="bold")
    ax2.set_ylabel("Latency\n(us)", fontsize=22, fontweight="bold")
    ax2.set_yscale("log")
    ax2.set_ylim(*latency_limits)
    ax1.tick_params(axis="x", which="major", labelsize=22)
    ax1.tick_params(axis="y", which="major", labelsize=24)
    ax2.tick_params(axis="y", which="major", labelsize=24)
    ax1.grid(True, axis="y", linestyle="--", alpha=0.5, zorder=0)
    legend_elements = [
        Patch(facecolor=color, edgecolor="black", hatch=hatch, label=method)
        for method, color, hatch in zip(METHODS, COLORS, HATCHES)
    ] + [
        Line2D(
            [0], [0], color="black", marker="o", linestyle="-", linewidth=2.5,
            markersize=10, label="P50 Latency",
        ),
        Line2D(
            [0], [0], color="gray", marker="^", linestyle="--", linewidth=2.5,
            markersize=10, label="P99 Latency",
        ),
    ]
    ax1.legend(
        legend_elements,
        list(METHODS) + ["P50 Latency", "P99 Latency"],
        loc="upper center",
        bbox_to_anchor=(0.5, 0.95),
        ncol=4,
        frameon=True,
        framealpha=0.95,
        edgecolor="black",
        columnspacing=1.0,
        handletextpad=0.5,
        labelspacing=0.1,
        fontsize=22,
        borderaxespad=0.0,
    )
    plt.tight_layout(pad=0.5)
    return fig


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    directory = Path(__file__).resolve().parent
    parser.add_argument("--data", type=Path, default=directory / "figure13-factor-analysis.json")
    parser.add_argument("--output", type=Path, default=directory / "Figure 13.pdf")
    parser.add_argument("--validate-only", action="store_true", help="Validate all 20 records without plotting.")
    parser.add_argument("--show", action="store_true", help="Show the figure after saving the PDF.")
    parser.add_argument("--expand-y", action="store_true", help="Expand an axis only if measured values exceed the original limits.")
    args = parser.parse_args(argv)
    try:
        workloads = load_dataset(args.data)
        limits = axis_limits(workloads, args.expand_y)
        print("Validated 20 measured round 9 records: 5 methods, 4 workloads, and 30 threads.")
        print("Throughput: Mops/s. Sampled P50/P99 latency: nanoseconds in JSON, microseconds in the plot.")
        if args.validate_only:
            return 0
        if args.output.suffix.lower() != ".pdf":
            raise ValidationError("The output filename must use the .pdf extension.")
        import matplotlib

        if not args.show:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
        from matplotlib.lines import Line2D
        from matplotlib.patches import Patch

        fig = draw_figure(plt, np, Patch, Line2D, workloads, limits)
        fig.savefig(args.output, bbox_inches="tight", pad_inches=0.05)
        print(f"Saved PDF: {args.output.resolve()}")
        if args.show:
            plt.show()
        plt.close(fig)
        return 0
    except (OSError, ValueError, ImportError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
