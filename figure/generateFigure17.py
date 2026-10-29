#!/usr/bin/env python3
"""Plot Figure 17 from measured 16-byte-key YCSB round-9 throughput."""

import argparse
import json
import math
from pathlib import Path


THREAD_COUNTS = (1, 5, 10, 15, 20, 25, 30, 35, 40, 45, 48)
WORKLOADS = ("a", "b", "c", "d")
METHOD_ORDER = ("Timnah", "Dash", "CHash")
WORKLOAD_FILES = {workload: f"figure17-ycsb-{workload}.json" for workload in WORKLOADS}


def console_message(message):
    """Keep console text ASCII even when an input path contains other characters."""
    return str(message).encode("ascii", "backslashreplace").decode("ascii")


class EnglishArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        super().error(console_message(message))


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON member: {key}")
        result[key] = value
    return result


def invalid_constant(value):
    raise ValueError(f"Non-finite JSON number: {value}")


def load_workload(path, expected_workload):
    """Require every requested index, thread count and 16-byte configuration."""
    path = Path(path)
    if expected_workload not in WORKLOADS:
        raise ValueError("Unknown workload")
    try:
        with path.open("r", encoding="utf-8") as stream:
            document = json.load(stream, object_pairs_hook=unique_object,
                                 parse_constant=invalid_constant)
    except (OSError, ValueError) as error:
        raise ValueError(f"Cannot read {path}: {error}") from error
    if not isinstance(document, dict) or frozenset(document) != frozenset(METHOD_ORDER):
        raise ValueError(f"{path}: the JSON root must contain exactly Timnah, Dash and CHash")

    values = {}
    for method in METHOD_ORDER:
        records = document[method]
        if not isinstance(records, list):
            raise ValueError(f"{path}: {method} must contain a result array")
        indexed = {}
        for position, record in enumerate(records):
            location = f"{path}: {method} result {position}"
            if not isinstance(record, dict):
                raise ValueError(f"{location} must be an object")
            if record.get("index") != method:
                raise ValueError(f"{location} has an incorrect index name")
            if record.get("workload") != expected_workload:
                raise ValueError(f"{location} has an incorrect workload")
            expected = {"round": 9, "num_rounds": 10, "key_bytes": 16,
                        "value_bytes": 8, "num_bg": 1 if method == "Timnah" else 0}
            for name, value in expected.items():
                if type(record.get(name)) is not int or record[name] != value:
                    raise ValueError(f"{location} requires {name}={value}")
            thread_count = record.get("num_threads")
            if type(thread_count) is not int or thread_count not in THREAD_COUNTS:
                raise ValueError(f"{location} has an unexpected num_threads value")
            if thread_count in indexed:
                raise ValueError(f"{location} duplicates num_threads={thread_count}")
            throughput = record.get("throughput_Mops")
            if (type(throughput) not in (int, float)
                    or not math.isfinite(throughput) or throughput <= 0):
                raise ValueError(f"{location} requires finite, positive throughput_Mops")
            indexed[thread_count] = float(throughput)
        missing = sorted(set(THREAD_COUNTS) - set(indexed))
        if missing:
            raise ValueError(f"{path}: {method} is missing thread counts: "
                             + ", ".join(str(value) for value in missing))
        values[method] = [indexed[thread_count] for thread_count in THREAD_COUNTS]
    return values


def load_datasets(data_dir):
    """Read the four complete, figure-specific workload datasets."""
    directory = Path(data_dir)
    return {workload: load_workload(directory / WORKLOAD_FILES[workload], workload)
            for workload in WORKLOADS}


def create_figure(datasets, show=False):
    """Create four throughput panels with shared axes and fixed visual settings."""
    import matplotlib
    if not show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    styles = {
        "Timnah": {"color": "orange", "marker": "D", "label": "Timnah"},
        "Dash": {"color": "green", "marker": "^", "label": "Dash"},
        "CHash": {"color": "blue", "marker": "o", "label": "CHash"},
    }
    figure, axes = plt.subplots(1, 4, figsize=(20, 2.8), sharey=True)
    for axis, workload in zip(axes, WORKLOADS):
        title = f"YCSB {workload.upper()}"
        for method in METHOD_ORDER:
            style = styles[method]
            axis.plot(THREAD_COUNTS, datasets[workload][method],
                      color=style["color"], marker=style["marker"],
                      linestyle="-", linewidth=2, markersize=6,
                      label=style["label"])
        axis.set_title(title, fontsize=20, fontweight="bold")
        axis.set_xlabel("Threads", fontsize=20)
        axis.tick_params(axis="both", labelsize=16)
        axis.grid(True, linestyle="--", alpha=0.6)
        axis.set_xlim(1, 48)
        axis.set_xticks([1, 8, 16, 24, 32, 40, 48])
        if title == "YCSB A":
            axis.legend(fontsize=14, loc="upper left",
                        bbox_to_anchor=(0.01, 0.99), borderaxespad=0,
                        labelspacing=0.25, handlelength=1.5,
                        framealpha=0.95, edgecolor="black")
    axes[0].set_ylim(0, 600)
    axes[0].set_yticks([0, 200, 400, 600])
    axes[0].set_ylabel("Throughput (Mops)", fontsize=20)
    plt.tight_layout(pad=0.5)
    return figure


def parse_arguments(argv=None):
    directory = Path(__file__).resolve().parent
    parser = EnglishArgumentParser(prog="generateFigure17.py", description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=directory,
                        help="Directory containing figure17-ycsb-a.json through figure17-ycsb-d.json")
    parser.add_argument("--output", type=Path, default=directory / "Figure 17.pdf",
                        help="Output PDF path (default: Figure 17.pdf beside this script)")
    parser.add_argument("--show", action="store_true", help="Show the interactive plot")
    return parser.parse_args(argv)


def main(argv=None):
    arguments = parse_arguments(argv)
    figure = None
    try:
        datasets = load_datasets(arguments.data_dir)
        figure = create_figure(datasets, show=arguments.show)
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        figure.savefig(arguments.output, format="pdf", bbox_inches="tight", pad_inches=0.05)
        print(console_message(f"Saved Figure 17 to {arguments.output}"))
        if arguments.show:
            import matplotlib.pyplot as plt
            plt.show()
    except (ValueError, OSError, ImportError) as error:
        raise SystemExit(console_message(f"Error: {error}")) from error
    finally:
        if figure is not None:
            import matplotlib.pyplot as plt
            plt.close(figure)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
