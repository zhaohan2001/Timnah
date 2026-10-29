#!/usr/bin/env python3
"""Plot the measured Figure 12 lifecycle throughput without interpolation."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any


INDEX_NAMES = ("Timnah", "Dash", "CHash")
NUM_LOAD = 100_000_000
NUM_RUN = 100_000_000
OPERATIONS_PER_STAGE = 10_000_000
STAGE_COUNT = 120
ORIGINAL_Y_MAX = 950.0


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


def require_finite(value: Any, name: str, *, positive: bool = False) -> float:
    if type(value) not in (int, float):
        raise ValidationError(f"{name} must be a finite number.")
    result = float(value)
    if not math.isfinite(result) or (positive and result <= 0):
        qualifier = "positive, finite" if positive else "finite"
        raise ValidationError(f"{name} must be a {qualifier} number.")
    return result


def expected_stage(stage: int) -> tuple[str, int, int, int]:
    """Return phase, lifecycle round, phase round, and stage within round."""
    if stage <= 10:
        return "bulk_loading", 0, 0, stage
    if stage <= 110:
        phase_round, stage_offset = divmod(stage - 11, 10)
        return "executing", phase_round + 1, phase_round, stage_offset + 1
    return "mass_deletion", 11, 0, stage - 110


def validate_dataset(data: Any) -> dict[str, list[dict[str, Any]]]:
    if not isinstance(data, dict):
        raise ValidationError("The JSON root must be an object.")
    for key, expected in (
        ("schema_version", 1),
        ("num_threads", 30),
        ("num_rounds", 10),
        ("stages_per_round", 10),
        ("num_load", NUM_LOAD),
        ("num_run", NUM_RUN),
    ):
        require_integer(data.get(key), expected, key)
    if data.get("metric") != "completed_operations/elapsed_seconds":
        raise ValidationError("Unexpected throughput metric.")
    if data.get("workload") != "c":
        raise ValidationError("Figure 12 requires YCSB C (workload c).")
    if data.get("complete") is not True:
        raise ValidationError("The experiment is incomplete; all three runs are required.")
    indices = data.get("indices")
    if not isinstance(indices, dict) or set(indices) != set(INDEX_NAMES):
        raise ValidationError("indices must contain exactly Timnah, Dash, and CHash.")
    runs = data.get("runs")
    if not isinstance(runs, dict) or set(runs) != set(INDEX_NAMES):
        raise ValidationError("runs must contain a receipt for each of the three indices.")
    for index in INDEX_NAMES:
        if not isinstance(runs[index], dict) or not runs[index]:
            raise ValidationError(f"{index}: the run receipt must be a nonempty object.")
        records = indices[index]
        if not isinstance(records, list) or len(records) != STAGE_COUNT:
            raise ValidationError(f"{index}: exactly 120 measured Stage records are required.")
        for stage, record in enumerate(records, start=1):
            label = f"{index}, Stage {stage}"
            if not isinstance(record, dict):
                raise ValidationError(f"{label}: the Stage record must be an object.")
            require_integer(record.get("stage"), stage, f"{label}: stage")
            phase, lifecycle_round, phase_round, stage_in_round = expected_stage(stage)
            if record.get("phase") != phase:
                raise ValidationError(f"{label}: phase must be {phase}.")
            for key, expected in (
                ("lifecycle_round", lifecycle_round),
                ("phase_round", phase_round),
                ("stage_in_round", stage_in_round),
                ("operations", OPERATIONS_PER_STAGE),
                ("successful_operations", OPERATIONS_PER_STAGE),
                ("failed_operations", 0),
            ):
                require_integer(record.get(key), expected, f"{label}: {key}")
            elapsed = require_finite(
                record.get("elapsed_seconds"), f"{label}: elapsed_seconds", positive=True
            )
            throughput = require_finite(
                record.get("throughput_Mops"), f"{label}: throughput_Mops", positive=True
            )
            calculated = OPERATIONS_PER_STAGE / elapsed / 1_000_000
            if not math.isclose(throughput, calculated, rel_tol=1e-8, abs_tol=5e-7):
                raise ValidationError(
                    f"{label}: throughput_Mops does not match operations / elapsed_seconds / 1e6."
                )
    return indices


def load_dataset(path: Path) -> dict[str, list[dict[str, Any]]]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle, object_pairs_hook=unique_object)
    return validate_dataset(data)


def axis_maximum(indices: dict[str, list[dict[str, Any]]], expand_y: bool) -> float:
    maximum = max(float(row["throughput_Mops"]) for rows in indices.values() for row in rows)
    if maximum <= ORIGINAL_Y_MAX:
        return ORIGINAL_Y_MAX
    if not expand_y:
        raise ValidationError(
            f"Measured throughput {maximum:.6f} Mops exceeds the original y-axis maximum "
            "of 950 Mops. Use --expand-y to show every measured point."
        )
    return float(math.ceil(maximum * 1.05 / 100.0) * 100)


def draw_figure(plt: Any, indices: dict[str, list[dict[str, Any]]], y_max: float) -> Any:
    """Plot measured stage throughput at each stage right-edge position."""
    fig, ax = plt.subplots(figsize=(12, 3.0))
    styles = {
        "Timnah": {"color": "#E69F00", "linewidth": 3, "linestyle": "-"},
        "Dash": {"color": "#009E73", "linewidth": 2.5, "linestyle": "--"},
        "CHash": {"color": "#0072B2", "linewidth": 2.5, "linestyle": "-."},
    }
    for index in INDEX_NAMES:
        rows = indices[index]
        ax.plot(
            [row["stage"] for row in rows],
            [row["throughput_Mops"] for row in rows],
            label=index,
            **styles[index],
        )

    ax.axvspan(0, 10, color="gray", alpha=0.1)
    ax.axvspan(10, 110, color="blue", alpha=0.05)
    ax.axvspan(110, 120, color="red", alpha=0.05)
    ax.axvline(x=10, color="black", linestyle=":", linewidth=1.5)
    ax.axvline(x=110, color="black", linestyle=":", linewidth=1.5)
    ax.set_xlim(0, 120)
    ax.set_ylim(0, y_max)
    ax.set_yticks([0, 200, 400, 600, 800])

    text_y = 800
    ax.text(1, text_y, "Bulk Loading", ha="left", va="center", fontsize=22, fontweight="bold")
    ax.text(60, text_y, "Executing", ha="center", va="center", fontsize=26, fontweight="bold")
    ax.text(119, text_y, "Mass Deletion", ha="right", va="center", fontsize=22, fontweight="bold")
    ax.set_xlabel("Stage", fontsize=24, fontweight="bold")
    ax.set_ylabel("Throughput\n(Mops)", fontsize=20, fontweight="bold")
    ax.tick_params(axis="both", which="major", labelsize=20)
    ax.grid(True, which="both", linestyle="--", alpha=0.5)
    ax.legend(
        loc="center",
        bbox_to_anchor=(0.55, 0.38),
        ncol=3,
        fontsize=18,
        framealpha=0.95,
        edgecolor="black",
        columnspacing=0.8,
        handletextpad=0.4,
    )
    plt.tight_layout(pad=0.5)
    return fig


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    directory = Path(__file__).resolve().parent
    parser.add_argument("--data", type=Path, default=directory / "figure12-lifecycle-throughput.json")
    parser.add_argument("--output", type=Path, default=directory / "Figure 12.pdf")
    parser.add_argument("--validate-only", action="store_true", help="Validate the complete dataset without plotting.")
    parser.add_argument("--show", action="store_true", help="Show the figure after saving the PDF.")
    parser.add_argument("--expand-y", action="store_true", help="Expand the y-axis only if a measured point exceeds 950 Mops.")
    args = parser.parse_args(argv)
    try:
        indices = load_dataset(args.data)
        y_max = axis_maximum(indices, args.expand_y)
        print("Validated 3 runs and 360 measured Stages (10 loading, 100 executing, 10 deletion per index).")
        print("Metric: completed operations / elapsed seconds, reported in Mops/s.")
        if args.validate_only:
            return 0
        if args.output.suffix.lower() != ".pdf":
            raise ValidationError("The output filename must use the .pdf extension.")
        import matplotlib

        if not args.show:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        fig = draw_figure(plt, indices, y_max)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(args.output, format="pdf", bbox_inches="tight", pad_inches=0.05, dpi=300)
        print(f"PDF file saved: {args.output.resolve()}")
        print("All 120 measured points per index are plotted; no interpolation or synthetic points are used.")
        if y_max != ORIGINAL_Y_MAX:
            print(f"The y-axis maximum was expanded to {y_max:g} Mops.")
        if args.show:
            plt.show()
        plt.close(fig)
        return 0
    except (OSError, ValueError, TypeError, KeyError, ImportError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
