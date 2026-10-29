#!/usr/bin/env python3
"""Plot measured DRAM Search hit rates using the original Figure 11 style."""

import argparse
import json
import math
from pathlib import Path
import sys


SCHEMA_VERSION = 1
METRIC = "search_dram_hits/search_attempts"
DATA_FILENAME = "figure11-dram-hit-rates.json"
METHOD_ORDER = ("Timnah", "Dash", "CHash", "EH+cache")
WORKLOAD_ORDER = ("c", "a", "ts", "w")
TITLES = {
    "c": "DRAM tier hit rate of YCSB-C",
    "a": "DRAM tier hit rate of YCSB-A",
    "ts": "DRAM tier hit rate of Twitter-Storage",
    "w": "DRAM tier hit rate of Webmail",
}
COLORS = {"Timnah": "orange", "Dash": "green", "CHash": "blue", "EH+cache": "red"}
MARKERS = {"Timnah": "D", "Dash": "^", "CHash": "o", "EH+cache": "s"}
RC_PARAMETERS = {
    "font.size": 34,
    "font.family": "DejaVu Sans",
    "font.weight": "normal",
    "axes.labelsize": 52,
    "axes.titlesize": 46,
    "xtick.labelsize": 52,
    "ytick.labelsize": 52,
    "legend.fontsize": 42,
    "lines.linewidth": 3.8,
    "lines.markersize": 11,
    "axes.linewidth": 1.6,
    "xtick.major.width": 1.6,
    "ytick.major.width": 1.6,
}


def english_text(value):
    """Keep all console output ASCII, including paths and exception messages."""
    return str(value).encode("ascii", "backslashreplace").decode("ascii")


class EnglishArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        super().error(english_text(message))


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON object key: " + english_text(key))
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError("Non-finite JSON constant: " + value)


def require_integer(record, name, minimum, location):
    value = record.get(name)
    if type(value) is not int or value < minimum:
        raise ValueError("{}: {} must be an integer >= {}".format(location, name, minimum))
    return value


def load_data(path):
    """Validate every measured stage; never fill, interpolate, or smooth data."""
    path = Path(path)
    try:
        with path.open("r", encoding="utf-8") as stream:
            document = json.load(stream, object_pairs_hook=unique_object,
                                 parse_constant=reject_constant)
    except (OSError, ValueError) as error:
        raise ValueError("Cannot read {}: {}".format(path, error)) from error
    if not isinstance(document, dict):
        raise ValueError("The Figure 11 JSON root must be an object")
    expected = {"schema_version": SCHEMA_VERSION, "metric": METRIC,
                "num_rounds": 10, "stages_per_round": 10, "num_threads": 30}
    for name, target in expected.items():
        actual = document.get(name)
        if actual != target or type(actual) is not type(target):
            raise ValueError("Unexpected Figure 11 setting: " + name)
    workloads = document.get("workloads")
    if not isinstance(workloads, dict) or set(workloads) != set(WORKLOAD_ORDER):
        raise ValueError("Figure 11 requires exactly workloads c, a, ts, and w")
    datasets = {}
    for workload in WORKLOAD_ORDER:
        methods = workloads[workload]
        if not isinstance(methods, dict) or set(methods) != set(METHOD_ORDER):
            raise ValueError("Workload {} requires exactly Timnah, Dash, CHash, and EH+cache".format(workload))
        datasets[workload] = {}
        for method in METHOD_ORDER:
            records = methods[method]
            if not isinstance(records, list) or len(records) != 100:
                raise ValueError("{}/{} requires exactly 100 measured stages".format(workload, method))
            indexed = {}
            for position, record in enumerate(records):
                location = "{}/{} record {}".format(workload, method, position)
                if not isinstance(record, dict):
                    raise ValueError(location + " must be an object")
                stage = require_integer(record, "stage", 1, location)
                if stage > 100 or stage in indexed:
                    raise ValueError(location + " has an out-of-range or duplicate stage")
                round_number = require_integer(record, "round", 0, location)
                stage_in_round = require_integer(record, "stage_in_round", 1, location)
                if round_number != (stage - 1) // 10 or stage_in_round != (stage - 1) % 10 + 1:
                    raise ValueError(location + " has inconsistent round/stage coordinates")
                operations = require_integer(record, "operations", 1, location)
                searches = require_integer(record, "search_attempts", 1, location)
                fast = require_integer(record, "dram_hits", 0, location)
                slow = require_integer(record, "slow_hits", 0, location)
                misses = require_integer(record, "read_misses", 0, location)
                if searches > operations or fast + slow + misses != searches:
                    raise ValueError(location + " has inconsistent Search counters")
                rate = record.get("dram_hit_rate")
                if (type(rate) not in (int, float) or not math.isfinite(rate)
                        or not 0 <= rate <= 1
                        or not math.isclose(rate, fast / searches, rel_tol=1e-12, abs_tol=1e-12)):
                    raise ValueError(location + " must have dram_hit_rate = dram_hits / search_attempts")
                indexed[stage] = float(rate)
            if set(indexed) != set(range(1, 101)):
                raise ValueError("{}/{} has missing measured stages".format(workload, method))
            datasets[workload][method] = [indexed[stage] for stage in range(1, 101)]
    return document, datasets


def axis_limits(workload, datasets, expand_y=False):
    """Preserve source limits by default; expanded limits are an explicit choice."""
    lower, upper = (0.1, 1.02) if workload in ("c", "a") else (0.1, 0.9)
    values = [value for method in METHOD_ORDER for value in datasets[workload][method]]
    if expand_y:
        if min(values) < lower:
            lower = 0.0
        if max(values) > upper:
            upper = 1.02
    return lower, upper


def display_warnings(datasets, expand_y=False):
    print("Figure 11 contains all 100 measured stages. The original paper-style x-axis displays stages 10-100; stages 1-9 remain in the JSON.")
    for workload in WORKLOAD_ORDER:
        lower, upper = axis_limits(workload, datasets, expand_y)
        for method in METHOD_ORDER:
            hidden = sum(value < lower or value > upper
                         for value in datasets[workload][method][9:])
            if hidden:
                print(english_text("Warning: {}/{} has {} displayed-stage values outside the original y-axis limits [{}, {}]. Data are unchanged. Use --expand-y to show the full measured range.".format(
                    workload, method, hidden, lower, upper)), file=sys.stderr)


def draw_one_axis(axis, datasets, workload, show_legend=False, show_ylabel=True,
                  expand_y=False):
    axis.grid(True, which="major", linestyle="--", alpha=0.6, color="gray", zorder=0)
    for method in METHOD_ORDER:
        axis.plot(range(1, 101), datasets[workload][method], marker=MARKERS[method],
                  markersize=4, color=COLORS[method], label=method, zorder=3)
    axis.set_xlabel("Stage", labelpad=14)
    axis.set_ylabel("Hit Rate" if show_ylabel else "", **({"labelpad": 18} if show_ylabel else {}))
    axis.set_title(TITLES[workload], fontweight="bold", pad=32)
    axis.set_xticks([10, 50, 90])
    axis.set_xticklabels(["10", "50", "90"], rotation=0)
    limits = axis_limits(workload, datasets, expand_y)
    if limits[1] > 1.0:
        axis.set_yticks([0.2, 0.6, 1.0])
        axis.set_yticklabels(["0.2", "0.6", "1.0"])
    else:
        axis.set_yticks([0.2, 0.5, 0.8])
        axis.set_yticklabels(["0.2", "0.5", "0.8"])
    axis.set_xlim(10, 100)
    axis.set_ylim(*limits)
    if show_legend:
        axis.legend(frameon=True, framealpha=0.85, edgecolor="black",
                    loc="center right", bbox_to_anchor=(0.98, 0.43), ncol=2,
                    handlelength=1.5, borderaxespad=0.1, labelspacing=0.1,
                    columnspacing=0.8, handletextpad=0.4)
    axis.spines["top"].set_visible(False)
    axis.spines["right"].set_visible(False)


def create_figure(datasets, show=False, expand_y=False):
    import matplotlib
    if not show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update(RC_PARAMETERS)
    figure, axes = plt.subplots(1, 4, figsize=(60, 6.5))
    for position, workload in enumerate(WORKLOAD_ORDER):
        draw_one_axis(axes[position], datasets, workload,
                      show_legend=position == 0, show_ylabel=position == 0,
                      expand_y=expand_y)
    plt.tight_layout(pad=1.0, w_pad=3.0)
    return figure


def arguments(argv=None):
    directory = Path(__file__).resolve().parent
    parser = EnglishArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=directory / DATA_FILENAME,
                        help="Measured Figure 11 JSON path")
    parser.add_argument("--output", type=Path, default=directory / "Figure 11.pdf",
                        help="Output PDF path (default: Figure 11.pdf beside this script)")
    limits = parser.add_mutually_exclusive_group()
    limits.add_argument("--expand-y", dest="expand_y", action="store_true",
                        help="Expand y-axis bounds when needed to show all measured hit rates (default)")
    limits.add_argument("--original-y-limits", dest="expand_y", action="store_false",
                        help="Use the source script's y-axis bounds, with warnings for hidden values")
    parser.set_defaults(expand_y=True)
    parser.add_argument("--show", action="store_true", help="Show the interactive figure")
    parser.add_argument("--validate-only", action="store_true",
                        help="Validate every measured stage without importing Matplotlib or writing a figure")
    return parser.parse_args(argv)


def main(argv=None):
    options = arguments(argv)
    _, datasets = load_data(options.data)
    print(english_text("Validated 16 measured series and 1,600 stage records from {}.".format(options.data)))
    if options.validate_only:
        return 0
    display_warnings(datasets, expand_y=options.expand_y)
    figure = create_figure(datasets, show=options.show, expand_y=options.expand_y)
    options.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(options.output, format="pdf", bbox_inches="tight", pad_inches=0.05)
    print(english_text("Saved Figure 11 to {}.".format(options.output)))
    import matplotlib.pyplot as plt
    if options.show:
        plt.show()
    plt.close(figure)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("Plot generation interrupted.", file=sys.stderr)
        raise SystemExit(130)
    except Exception as error:
        print("Error: " + english_text(error), file=sys.stderr)
        raise SystemExit(1)
