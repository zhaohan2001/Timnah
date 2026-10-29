#!/usr/bin/env python3
"""Plot Figure 10 from measured round 9 throughput in four workload JSON files."""

import argparse
import json
import math
from pathlib import Path


THREAD_COUNTS = (1, 5, 10, 15, 20, 25, 30, 35, 40, 45, 48)
WORKLOADS = ("ts", "tt", "tc", "w")
WORKLOAD_FILES = {"ts": "storage.json", "tt": "transient.json",
                  "tc": "compute.json", "w": "WebMail.json"}
TITLES = {"ts": "Storage", "tt": "Transient", "tc": "Compute", "w": "Webmail"}
METHOD_ORDER = ("Timnah", "Dash", "CHash", "EH+cache")
REQUIRED_METHODS = frozenset(METHOD_ORDER)


def console_message(message):
    """Keep console messages ASCII even when an input path contains other characters."""
    return str(message).encode("ascii", "backslashreplace").decode("ascii")


class EnglishArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        super().error(console_message(message))


def load_workload(path, expected_workload=None):
    """Read one workload, requiring measured results at every requested thread count."""
    path = Path(path)
    try:
        with path.open("r", encoding="utf-8") as stream:
            document = json.load(stream)
    except (OSError, ValueError) as error:
        raise ValueError(f"Cannot read {path}: {error}") from error
    if not isinstance(document, dict):
        raise ValueError(f"{path}: the JSON root must map index names to result arrays")
    methods = frozenset(document)
    if not REQUIRED_METHODS.issubset(methods):
        missing = ", ".join(sorted(REQUIRED_METHODS - methods))
        raise ValueError(f"{path}: missing required indexes: {missing}")
    if not methods.issubset(METHOD_ORDER):
        unknown = ", ".join(sorted(methods - frozenset(METHOD_ORDER)))
        raise ValueError(f"{path}: unknown index names: {unknown}")

    values = {}
    for method in METHOD_ORDER:
        if method not in document:
            continue
        records = document[method]
        if not isinstance(records, list):
            raise ValueError(f"{path}: {method} must contain a result array")
        indexed = {}
        for position, record in enumerate(records):
            location = f"{path}: {method} result {position}"
            if not isinstance(record, dict):
                raise ValueError(f"{location} must be an object")
            thread_count = record.get("num_threads")
            if type(thread_count) is not int or thread_count not in THREAD_COUNTS:
                raise ValueError(f"{location} has an unexpected num_threads value")
            if thread_count in indexed:
                raise ValueError(f"{location} duplicates num_threads={thread_count}")
            if "round" in record and (type(record["round"]) is not int or record["round"] != 9):
                raise ValueError(f"{location} must contain round 9 results")
            if "num_rounds" in record:
                num_rounds = record["num_rounds"]
                if type(num_rounds) is not int or num_rounds < 10:
                    raise ValueError(f"{location} requires num_rounds >= 10 for round 9")
            if expected_workload is not None and "workload" in record:
                workload = record["workload"]
                if (not isinstance(workload, str)
                        or workload.lower() != expected_workload):
                    raise ValueError(f"{location} has a workload that does not match its JSON filename")
            throughput = record.get("throughput_Mops")
            if (type(throughput) not in (int, float)
                    or not math.isfinite(throughput) or throughput <= 0):
                raise ValueError(f"{location} requires finite, positive throughput_Mops")
            indexed[thread_count] = float(throughput)
        missing_threads = sorted(set(THREAD_COUNTS) - set(indexed))
        if missing_threads:
            missing = ", ".join(str(value) for value in missing_threads)
            raise ValueError(f"{path}: {method} is missing thread counts: {missing}")
        values[method] = [indexed[thread_count] for thread_count in THREAD_COUNTS]
    return values


def load_datasets(data_dir):
    """Require the same measured index set in all four workloads."""
    data_dir = Path(data_dir)
    datasets = {
        workload: load_workload(data_dir / WORKLOAD_FILES[workload], expected_workload=workload)
        for workload in WORKLOADS
    }
    expected = frozenset(datasets[WORKLOADS[0]])
    for workload in WORKLOADS[1:]:
        if frozenset(datasets[workload]) != expected:
            raise ValueError("All four workload JSON files must contain the same index names")
    return datasets


def create_figure(datasets, show=False):
    """Create four workload panels from measured throughput datasets."""
    import matplotlib
    if not show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    styles = {
        "Timnah": {"color": "orange", "marker": "D", "label": "Timnah"},
        "Dash": {"color": "green", "marker": "^", "label": "Dash"},
        "CHash": {"color": "blue", "marker": "o", "label": "CHash"},
        "EH+cache": {"color": "red", "marker": "s", "label": "EH+cache"},
    }
    figure, axes = plt.subplots(1, 4, figsize=(20, 2.8), sharey=True)
    for axis, workload in zip(axes, WORKLOADS):
        title = TITLES[workload]
        for method in METHOD_ORDER:
            if method in datasets[workload]:
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
        if title == "Storage":
            axis.legend(fontsize=14, loc="upper left",
                        bbox_to_anchor=(0.01, 0.99), borderaxespad=0,
                        labelspacing=0.25, handlelength=1.5,
                        framealpha=0.95, edgecolor="black")
    axes[0].set_ylim(0, 600)
    axes[0].set_yticks([0, 200, 400, 600])
    axes[0].set_ylabel("Throughput (Mops)", fontsize=20)
    axes[0].tick_params(axis="both", labelsize=16)
    plt.tight_layout(pad=0.5)
    return figure


def parse_arguments(argv=None):
    directory = Path(__file__).resolve().parent
    parser = EnglishArgumentParser(prog="generateFigure10.py", description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=directory,
                        help="Directory containing storage.json, transient.json, compute.json and WebMail.json")
    parser.add_argument("--output", type=Path, default=directory / "Figure 10.pdf",
                        help="Output PDF path (default: Figure 10.pdf beside this script)")
    parser.add_argument("--png", type=Path,
                        help="Also save a PNG preview at this path")
    parser.add_argument("--show", action="store_true", help="Show the interactive plot")
    return parser.parse_args(argv)


def main(argv=None):
    arguments = parse_arguments(argv)
    try:
        datasets = load_datasets(arguments.data_dir)
        figure = create_figure(datasets, show=arguments.show)
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        figure.savefig(arguments.output, format="pdf", bbox_inches="tight", pad_inches=0.05)
        print(console_message(f"Saved Figure 10 to {arguments.output}"))
        if arguments.png is not None:
            arguments.png.parent.mkdir(parents=True, exist_ok=True)
            figure.savefig(arguments.png, bbox_inches="tight", pad_inches=0.05)
            print(console_message(f"Saved Figure 10 preview to {arguments.png}"))
        import matplotlib.pyplot as plt
        if arguments.show:
            plt.show()
        plt.close(figure)
    except (ValueError, OSError, ImportError) as error:
        raise SystemExit(console_message(f"Error: {error}")) from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
