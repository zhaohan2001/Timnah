#!/usr/bin/env python3
"""Collect Figure 14 capacity measurements over unchanged isolated index sources."""

import argparse
from datetime import datetime, timezone
import fcntl
import gzip
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import zlib


SCHEMA_VERSION = 1
METRIC = "round9_throughput_and_sampled_latency"
DATA_FILENAME = "figure14-dram-capacity.json"
VARIANTS = {
    "Timnah-A": {"index": "Timnah", "num_bg": 1, "fast_depth": 12, "capacity_MiB": 256},
    "Timnah-B": {"index": "Timnah", "num_bg": 1, "fast_depth": 11, "capacity_MiB": 128},
    "Timnah-C": {"index": "Timnah", "num_bg": 1, "fast_depth": 10, "capacity_MiB": 64},
    "CHash": {"index": "CHash", "num_bg": 0, "fast_depth": 0, "capacity_MiB": 256},
}
WORKLOADS = {"a": (0.5, 0.5, 0.0), "b": (0.95, 0.05, 0.0),
             "c": (1.0, 0.0, 0.0), "d": (0.95, 0.0, 0.05)}
SOURCE_SUFFIXES = {".h", ".hpp", ".cc", ".cpp", ".cmake", ".txt"}
ENV_PREFIXES = ("LD_", "OMP_", "GOMP_", "KMP_", "MALLOC_")


def english_text(value):
    return str(value).encode("ascii", "backslashreplace").decode("ascii")


class EnglishArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        super().error(english_text(message))


def now():
    return datetime.now(timezone.utc).isoformat()


def digest_bytes(value):
    return hashlib.sha256(value).hexdigest()


def digest_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def fingerprint(value):
    return digest_bytes(json.dumps(value, sort_keys=True, ensure_ascii=True,
                                  allow_nan=False).encode("ascii"))


def atomic_bytes(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def atomic_json(path, data):
    atomic_bytes(path, (json.dumps(data, indent=2, ensure_ascii=True,
                                  allow_nan=False) + "\n").encode("ascii"))


def fields(text):
    return dict(re.findall(r"([a-zA-Z0-9_]+)=([^\s]+)", text))


def production_versions(repository):
    """Fingerprint original sources, build configuration, binary and libraries."""
    ordinary = repository / "build/benchmark/ycsb/ycsb"
    if not ordinary.is_file():
        raise RuntimeError("The ordinary production ycsb executable is missing")
    paths = {ordinary.resolve()}
    for relative in ("utils", "hashindex", "benchmark"):
        for path in (repository / relative).rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                paths.add(path.resolve())
    for relative in ("CMakeLists.txt", "dependency.cmake", "build/CMakeCache.txt"):
        path = repository / relative
        if path.is_file():
            paths.add(path.resolve())
    for path in (repository / "build").rglob("*"):
        if path.is_file() and (".so" in path.name or path.name == "flags.make"):
            paths.add(path.resolve())
    linked = subprocess.run(["ldd", str(ordinary)], capture_output=True,
                            text=True, check=True)
    if "not found" in linked.stdout:
        raise RuntimeError("An ordinary ycsb runtime dependency is unresolved")
    for name in re.findall(r"(?:=>\s+)?(/[^\s]+)", linked.stdout):
        path = Path(name)
        if path.is_file():
            paths.add(path.resolve())
    return {str(path): digest_file(path) for path in sorted(paths)}


def environment():
    names = {name for name in os.environ if name.startswith(ENV_PREFIXES)}
    names.update(("PATH", "GLIBC_TUNABLES"))
    return {"variables": {name: os.environ.get(name) for name in sorted(names)},
            "parent_cpu_affinity": sorted(os.sched_getaffinity(0))
            if hasattr(os, "sched_getaffinity") else None}


def support_versions(support):
    suffixes = {".py", ".cc", ".cpp", ".h", ".hpp", ".inc", ".cmake"}
    result = {str(p.relative_to(support)): digest_file(p) for p in sorted(support.rglob("*"))
              if p.is_file() and (p.suffix in suffixes or p.name == "CMakeLists.txt")}
    if not {"prepare.py", "driver.cpp", "CMakeLists.txt", "percentile.hpp", "correctness.cpp"}.issubset(result):
        raise RuntimeError("The complete Figure 14 support directory is required")
    return result


def source_copy_valid(source, repository, support):
    try:
        m = json.loads((source / ".figure14-diagnostic-copy.json").read_text())
        if (m["schema"] != "figure14-diagnostic-source-v1" or m["repository"] != str(repository)
                or m["source_dir"] != str(source) or m["production_files_modified"] is not False
                or m["timnah_source_modified"] is not False or m["chash_source_modified"] is not False
                or m["generator_support_sha256"] != {"prepare.py": digest_file(support / "prepare.py")}
                or not isinstance(m["production_sources_sha256"], dict)
                or not isinstance(m["diagnostic_sources_sha256"], dict)
                or not m["production_sources_sha256"]
                or m["production_sources_sha256"] != m["diagnostic_sources_sha256"]):
            return False
        for group, root in (("production_sources_sha256", repository), ("diagnostic_sources_sha256", source)):
            for name, value in m[group].items():
                path = root / name
                if Path(name).is_absolute() or not path.resolve().is_relative_to(root) or digest_file(path) != value:
                    return False
        actual = {str(p.relative_to(source)) for p in source.rglob("*")
                  if p.is_file() and p.name != ".figure14-diagnostic-copy.json"}
        return actual == set(m["diagnostic_sources_sha256"])
    except (OSError, ValueError, KeyError, TypeError):
        return False


def runtime_versions(binary):
    linked = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, check=True)
    if "not found" in linked.stdout:
        raise RuntimeError("An isolated Figure 14 runtime dependency is unresolved")
    return {str(p): digest_file(p) for p in sorted({Path(n).resolve() for n in
            re.findall(r"(?:=>\s+)?(/[^\s]+)", linked.stdout) if Path(n).is_file()})}


def build_files(build):
    return {str(p.relative_to(build)): digest_file(p) for p in sorted(build.rglob("*"))
            if p.is_file() and p.name in {"flags.make", "link.txt", "CMakeCache.txt", "build.ninja", "rules.ninja"}}


def build_diagnostic(args):
    source, build, support = args.source_dir, args.build_dir, args.support_dir
    if not source_copy_valid(source, args.repository, support):
        if args.validate_only:
            return None
        sys.dont_write_bytecode = True
        spec = importlib.util.spec_from_file_location("figure14_prepare", support / "prepare.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.prepare(args.repository, source)
    if not source_copy_valid(source, args.repository, support):
        raise RuntimeError("The unchanged Figure 14 source copy failed validation")
    inputs = {"source_manifest": digest_file(source / ".figure14-diagnostic-copy.json"),
              "support": support_versions(support), "build_type": "Release",
              "source": str(source), "support_dir": str(support)}
    binary, cache = build / "figure14_probe", build / "figure14-build-manifest.json"
    try:
        old = json.loads(cache.read_text())
        if (old["inputs"] == inputs and old["binary_sha256"] == digest_file(binary)
                and old["correctness_binary_sha256"] == digest_file(build / "figure14_correctness")
                and old["build_files_sha256"] == build_files(build)
                and old["linked_libraries_sha256"] == runtime_versions(binary)):
            return old
    except (OSError, ValueError, KeyError, TypeError):
        pass
    if args.validate_only:
        return None
    build.mkdir(parents=True, exist_ok=True)
    commands = [["cmake", "-S", str(support), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                 "-DFIGURE14_REPOSITORY=" + str(args.repository), "-DFIGURE14_SOURCE_ROOT=" + str(source)],
                ["cmake", "--build", str(build), "--target", "figure14_probe", "figure14_correctness", "-j", str(args.jobs)]]
    for n, command in enumerate(commands):
        r = subprocess.run(command, capture_output=True)
        log = build / ("configure.log" if n == 0 else "compile.log")
        atomic_bytes(log, english_text((r.stdout + r.stderr).decode("utf-8", "replace")).encode("ascii"))
        if r.returncode:
            raise RuntimeError("Figure 14 isolated build failed; see " + str(log))
    if not source_copy_valid(source, args.repository, support) or inputs["support"] != support_versions(support):
        raise RuntimeError("Source or support code changed during the isolated build")
    value = {"inputs": inputs, "binary_sha256": digest_file(binary),
             "correctness_binary_sha256": digest_file(build / "figure14_correctness"),
             "linked_libraries_sha256": runtime_versions(binary), "build_files_sha256": build_files(build)}
    atomic_json(cache, value)
    return value


def require(condition, message):
    if not condition:
        raise ValueError(message)


def require_fields(row, expected, name):
    for k, v in expected.items():
        require(row.get(k) == str(v), name + ": incorrect " + k)


def parse_run(raw, config):
    def entries(marker):
        return [fields(line) for line in re.findall(r"^" + marker + r" (.+)$", raw, re.M)]
    variant, workload = config["variant"], config["workload"]
    design = VARIANTS[variant]
    configs, traces, loads, final = [entries(m) for m in
        ("Figure14Config", "Figure14Trace", "Figure14Load", "Figure14Complete")]
    require(all(len(rows) == 1 for rows in (configs, traces, loads, final)), "Missing or duplicate run markers")
    require_fields(configs[0], {"variant": variant, "index": design["index"], "workload": workload,
        "num_threads": 30, "num_load_threads": 30, "num_run_threads": 30, "num_rounds": 10,
        "num_bg": design["num_bg"], "num_load": config["num_load"], "num_run": config["num_run"],
        "dram_bucket_mib": 0 if variant == "CHash" else design["capacity_MiB"],
        "fast_depth": design["fast_depth"], "slow_depth": 14, "latency": "true", "latency_sample_rate": "0.01",
        "cpu_node": 0, "fast_node": 0, "slow_node": 2, "index_source": "unchanged_production_snapshot",
        "convergence_threshold": "0.002", "trace_generation_threads": 30, "chash_fast_pool_mib": 256 if variant == "CHash" else 0,
        "metric": "whole_round_attempted_operations/elapsed_seconds", "latency_unit": "ns",
        "d_mode": "ratio_only_replay_same_insert_keys"}, "Configuration")
    read_ratio, update_ratio, insert_ratio = WORKLOADS[workload]
    pattern = ("YCSB workload=" + workload + " read=" + format(read_ratio, "g") +
               " update=" + format(update_ratio, "g") + " insert=" + format(insert_ratio, "g") +
               " remove=0 negative=0 distribution=zipfian")
    require(pattern in raw, "The requested YCSB profile is missing")
    trace = traces[0]
    require_fields(trace, {"workload": workload, "operations": config["num_run"], "replay_same_trace": "true"}, "Trace")
    operations = {k: int(trace[k]) for k in ("search_operations", "update_operations", "insert_operations")}
    require(all(v >= 0 for v in operations.values()) and sum(operations.values()) == config["num_run"], "Invalid trace operation counts")
    for name, ratio in zip(operations, (read_ratio, update_ratio, insert_ratio)):
        require((operations[name] == 0) == (ratio == 0), "Trace operation types do not match the workload")
        if ratio > 0:
            require(abs(operations[name] / config["num_run"] - ratio) <= 0.05, "Trace ratio is inconsistent")
    inserts = operations["insert_operations"]
    require_fields(loads[0], {"operations": config["num_load"], "successful_operations": config["num_load"],
                            "failed_operations": 0, "size": config["num_load"]}, "Bulk load")
    rounds = entries("Figure14Round")
    require(len(rounds) == 10, "Exactly ten round records are required")
    measured = []
    for number, row in enumerate(rounds):
        expected_failures = inserts if number > 0 else 0
        require_fields(row, {"round": number, "operations": config["num_run"],
            "successful_operations": config["num_run"] - expected_failures,
            "failed_operations": expected_failures, "expected_duplicate_insert_rejections": expected_failures,
            "unexpected_failures": 0, "size": config["num_load"] + inserts, **operations}, "Round")
        elapsed, samples = int(row["elapsed_ns"]), int(row["latency_samples"])
        p50, p99, rate = int(row["p50_latency_ns"]), int(row["p99_latency_ns"]), float(row["throughput_Mops"])
        require(elapsed > 0 and 0 < samples <= config["num_run"] and 0 < p50 <= p99, "Invalid timing or sampled latency")
        require(math.isfinite(rate) and rate > 0 and math.isclose(rate, config["num_run"] * 1000 / elapsed, rel_tol=1e-12),
                "Throughput must use attempted operations and the measured whole-round time")
        measured.append({"round": number, "throughput_Mops": rate, "p50_latency_ns": p50,
            "p99_latency_ns": p99, "elapsed_ns": elapsed, "latency_samples": samples,
            "operations": config["num_run"], "successful_operations": config["num_run"] - expected_failures,
            "failed_operations": expected_failures, "expected_duplicate_insert_rejections": expected_failures,
            "size": config["num_load"] + inserts, **operations})
    native = entries("Figure14NativeStatistics")
    require([r.get("round") for r in native] == ["load"] + [str(n) for n in range(10)], "Native statistics rounds are missing")
    for number, row in enumerate(native):
        require_fields(row, {"size": config["num_load"] + (inserts if number else 0)}, "Native statistics")
        if variant != "CHash":
            require_fields(row, {"fast_node": 0, "slow_node": 2, "misses": 0}, "Native Timnah statistics")
        else:
            require_fields(row, {"dram_node": 0, "slow_node": 2}, "Native CHash statistics")
    memory = entries("Figure14Memory")
    require([r.get("phase") for r in memory] == ["initial", "loaded", "complete"], "Missing memory phase evidence")
    for row in memory:
        require_fields(row, {"fast_node": 0, "slow_node": 2}, "Memory placement")
        if variant == "CHash":
            require_fields(row, {"fast_pool_capacity_bytes": 256 << 20, "fast_reserved": 256 << 20,
                                "budget_scope": "existing_metadata_plus_KV_pool"}, "CHash memory")
            require(0 <= int(row["fast_used"]) <= 256 << 20, "CHash fast pool exceeded its budget")
        else:
            cap = design["capacity_MiB"] << 20
            require_fields(row, {"fast_bucket_bytes": cap, "fast_counter_bytes": cap // 4,
                "fast_segments": 1 << design["fast_depth"], "fast_depth": design["fast_depth"],
                "fast_bucket_capacity_bytes": cap, "fast_counter_capacity_bytes": cap // 4,
                "fast_bucket_reserved_bytes_inferred": cap, "fast_counter_reserved_bytes_inferred": 64 << 20,
                "budget_scope": "fast_bucket_bytes_only", "counters_and_metadata_included": "false"}, "Timnah memory")
    failed_total = inserts * 9
    require_fields(final[0], {"variant": variant, "workload": workload, "rounds": 10,
        "operations": config["num_run"] * 10, "successful_operations": config["num_run"] * 10 - failed_total,
        "failed_operations": failed_total, "expected_duplicate_insert_rejections": failed_total,
        "unexpected_failures": 0, "size": config["num_load"] + inserts, "verified_rounds": 10}, "Completion")
    return measured, {"trace": trace, "load": loads[0], "rounds": native, "memory": memory, "complete": final[0]}


def configuration(args, workload, variant):
    d = VARIANTS[variant]
    command = [str(args.build_dir / "figure14_probe"), "-index=" + d["index"], "-figure14_variant=" + variant,
        "-workload=" + workload, "-num_rounds=10", "-num_threads=30", "-num_load_threads=30", "-num_run_threads=30",
        "-num_bg=" + str(d["num_bg"]), "-num_load=" + str(args.num_load), "-num_run=" + str(args.num_run),
        "-rndSeed=1729", "-latency=true", "-latency_sample_rate=0.01", "-bind_core=true", "-verify=false",
        "-shift=false", "-shuffle=true", "-keySize=8", "-valueSize=8"]
    if variant == "CHash":
        command += ["-chash_initial_depth=16", "-chash_fast_mib=256", "-chash_slow_initial_mib=1024",
                    "-chash_slow_growth_mib=64", "-chash_dram_node=0", "-chash_slow_node=2"]
    else:
        command += ["-timnah_fast_depth=" + str(d["fast_depth"]), "-timnah_slow_depth=14", "-timnah_fast_node=0", "-timnah_slow_node=2"]
    return {"variant": variant, "label": variant, "workload": workload, "index": d["index"], "num_bg": d["num_bg"],
            "num_threads": 30, "num_rounds": 10, "num_load": args.num_load, "num_run": args.num_run,
            "capacity_MiB": d["capacity_MiB"], "fast_depth": d["fast_depth"], "command": command}




def cached(path, expected, config):
    try:
        record = json.loads(path.read_text())
        if record["fingerprint"] != expected or record["config"] != config or record["exit_code"] != 0:
            return None
        raw = gzip.decompress(Path(record["stdout_gzip"]).read_bytes())
        log = Path(record["log"]).read_bytes()
        if digest_bytes(raw) != record["stdout_sha256"] or digest_bytes(log) != record["log_sha256"]:
            return None
        if log != english_text(raw.decode("utf-8", "replace")).encode("ascii"):
            return None
        rounds, native = parse_run(raw.decode("utf-8", "replace"), config)
        if rounds != record["rounds"] or native != record["native"] or record["measurement"] != rounds[-1]:
            return None
        return record
    except (OSError, ValueError, KeyError, TypeError, EOFError, zlib.error):
        return None


def execute(args, config, path, expected):
    raw_path = path.with_suffix(".stdout.pending")
    started, begin = now(), time.monotonic()
    with raw_path.open("wb") as stream:
        process = subprocess.Popen(config["command"], cwd=args.repository / "build/benchmark/ycsb",
                                   stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while True:
                try:
                    code = process.wait(timeout=10)
                    break
                except subprocess.TimeoutExpired:
                    if args.timeout and time.monotonic() - begin >= args.timeout:
                        raise TimeoutError("The capacity benchmark exceeded the requested timeout")
        except BaseException:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            raise
    raw = raw_path.read_bytes()
    compressed, log = path.with_suffix(".stdout.gz"), path.with_suffix(".log")
    atomic_bytes(compressed, gzip.compress(raw, mtime=0))
    atomic_bytes(log, english_text(raw.decode("utf-8", "replace")).encode("ascii"))
    raw_path.unlink()
    if code:
        raise RuntimeError("Capacity benchmark exited with {}. See {}".format(code, log))
    rounds, native = parse_run(raw.decode("utf-8", "replace"), config)
    return {"fingerprint": expected, "config": config, "exit_code": code,
            "started_utc": started, "finished_utc": now(), "elapsed_seconds": time.monotonic() - begin,
            "stdout_gzip": str(compressed), "stdout_sha256": digest_bytes(raw), "log": str(log),
            "log_sha256": digest_file(log), "rounds": rounds, "measurement": rounds[-1], "native": native}


def assert_idle():
    for path in Path("/proc").glob("[0-9]*/comm"):
        try:
            name = path.read_text().strip()
            if name in ("ycsb", "ycsb.before", "figure11_probe", "figure12_probe") or name.startswith(("figure13_", "figure14_")):
                if (path.parent / "stat").read_text().split(") ", 1)[1][0] != "Z":
                    raise RuntimeError("Another benchmark is active: " + path.parent.name)
        except (OSError, PermissionError):
            pass


def frozen(args, original, env, build):
    if digest_file(Path(__file__).resolve()) != args.collector_sha256:
        raise RuntimeError("The collection script changed during collection")
    if production_versions(args.repository) != original or environment() != env:
        raise RuntimeError("Normal sources, executable, libraries or runtime environment changed")
    if (not source_copy_valid(args.source_dir, args.repository, args.support_dir)
            or digest_file(args.source_dir / ".figure14-diagnostic-copy.json") != build["inputs"]["source_manifest"]
            or support_versions(args.support_dir) != build["inputs"]["support"]
            or build_files(args.build_dir) != build["build_files_sha256"]
            or digest_file(args.build_dir / "figure14_probe") != build["binary_sha256"]
            or digest_file(args.build_dir / "figure14_correctness") != build["correctness_binary_sha256"]
            or runtime_versions(args.build_dir / "figure14_probe") != build["linked_libraries_sha256"]):
        raise RuntimeError("The isolated source, support, executable, build flags or runtime changed")


def publish(args, accepted, key):
    workloads = {w: {name: receipt["measurement"] for name, receipt in accepted[w].items()} for w in WORKLOADS}
    complete = all(set(workloads[w]) == set(VARIANTS) for w in WORKLOADS)
    output = {"schema_version": SCHEMA_VERSION, "metric": METRIC, "num_threads": 30, "num_rounds": 10,
        "num_load": args.num_load, "num_run": args.num_run, "round": 9, "complete": complete,
        "version_fingerprint": key, "updated_utc": now(), "workloads": workloads, "runs": accepted,
        "capacities_MiB": {name: d["capacity_MiB"] for name, d in VARIANTS.items()},
        "capacity_scope": {"Timnah-A": "fast_bucket_bytes_only", "Timnah-B": "fast_bucket_bytes_only",
            "Timnah-C": "fast_bucket_bytes_only", "CHash": "existing_metadata_plus_KV_pool"},
        "note": "Sixteen real measured runs: Timnah-A/B/C and CHash on current YCSB A/B/C/D. "
                "All index and trace sources are unchanged snapshots. Timnah fast_depth 12/11/10 gives "
                "256/128/64 MiB buckets; used counters are 64/32/16 MiB, reserved counter arenas are "
                "64 MiB each, and metadata is additional on node 0. CHash retains its existing 256 MiB "
                "metadata-plus-KV fast pool. This is not a matched total-DRAM-footprint comparison. "
                "CPU node 0, fast node 0, slow node 2. Timnah num_bg=1; CHash num_bg=0. Throughput uses "
                "attempted operations per whole-round wall time, as in normal ycsb. Current ratio-only "
                "D repeats insert keys: duplicate insertion rejections after round 0 are expected and "
                "recorded, not silent failures. Sampled P50/P99 are stored in ns using the existing "
                "percentile estimator; the plot converts to us. No interpolation or simulated values."}
    atomic_json(args.output_dir / DATA_FILENAME, output)


def parser():
    folder = Path(__file__).resolve().parent
    p = EnglishArgumentParser(description=__doc__)
    p.add_argument("--repository", type=Path, default=folder.parent)
    p.add_argument("--output-dir", type=Path, default=folder)
    p.add_argument("--support-dir", type=Path, default=folder / "figure14_support")
    p.add_argument("--source-dir", type=Path, default=folder / ".figure14-source")
    p.add_argument("--build-dir", type=Path, default=folder / ".figure14-build")
    p.add_argument("--workloads", nargs="+", choices=WORKLOADS, default=list(WORKLOADS))
    p.add_argument("--variants", nargs="+", choices=VARIANTS, default=list(VARIANTS))
    p.add_argument("--num-load", type=int, default=100000000)
    p.add_argument("--num-run", type=int, default=100000000)
    p.add_argument("--jobs", type=int, default=8)
    p.add_argument("--timeout", type=float, default=0)
    p.add_argument("--force", action="store_true")
    p.add_argument("--validate-only", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    if (args.num_load < 10000 or args.num_run < 10000 or args.jobs < 1
            or not math.isfinite(args.timeout) or args.timeout < 0):
        raise ValueError("Counts must be at least 10000, jobs positive, and timeout finite/nonnegative")
    if len(set(args.workloads)) != len(args.workloads) or len(set(args.variants)) != len(args.variants):
        raise ValueError("Workloads and variants must be unique")
    for name in ("repository", "output_dir", "support_dir", "source_dir", "build_dir"):
        setattr(args, name, getattr(args, name).resolve())
    for name in ("source_dir", "build_dir"):
        path = getattr(args, name)
        if path == args.repository or args.repository.is_relative_to(path):
            raise ValueError("Diagnostic directories cannot replace the repository")
        if any(path.is_relative_to(args.repository / item) for item in ("benchmark", "hashindex", "utils", "build")):
            raise ValueError("Diagnostic directories must be outside production directories")
        if path == args.support_dir or path.is_relative_to(args.support_dir) or args.support_dir.is_relative_to(path):
            raise ValueError("Source/build directories cannot overlap support files")
    if (args.source_dir == args.build_dir or args.source_dir.is_relative_to(args.build_dir)
            or args.build_dir.is_relative_to(args.source_dir)):
        raise ValueError("Isolated source and build directories must be separate")
    plan = [configuration(args, w, v) for w in args.workloads for v in args.variants]
    if args.dry_run:
        for c in plan:
            print(english_text(" ".join(c["command"])))
        return 0
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (Path(__file__).resolve().parent / ".collection.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("Another figure collector is active")
        assert_idle()
        original, env = production_versions(args.repository), environment()
        args.collector_sha256 = digest_file(Path(__file__).resolve())
        build = build_diagnostic(args)
        if build is None:
            print("No validated Figure 14 isolated build is cached.")
            return 2
        frozen(args, original, env, build)
        versions = {"production": original, "build": build, "collector_sha256": args.collector_sha256}
        key = fingerprint({"versions": versions, "environment": env, "num_load": args.num_load, "num_run": args.num_run})
        session = args.output_dir / "runs-figure14" / key[:16]
        session.mkdir(parents=True, exist_ok=True)
        accepted, missing = {name: {} for name in WORKLOADS}, []
        for c in plan:
            path = session / (c["workload"] + "-" + c["variant"] + ".json")
            expected = fingerprint({"session": key, "config": c})
            receipt = None if args.force else cached(path, expected, c)
            if receipt is None:
                missing.append((c, path, expected))
            else:
                accepted[c["workload"]][c["label"]] = receipt
        count = len(plan) - len(missing)
        print("Validated cached runs: {}/{}. Missing: {}.".format(count, len(plan), len(missing)), flush=True)
        if args.validate_only:
            return 0 if not missing else 2
        metadata = {"schema_version": 1, "status": "running", "started_utc": now(), "versions": versions,
                    "environment": env, "plan": plan, "completed": count, "expected": len(plan)}
        atomic_json(session / "session.json", metadata)
        publish(args, accepted, key)
        try:
            for c, path, expected in missing:
                assert_idle()
                frozen(args, original, env, build)
                print("[{}/{}] Running {} on workload {}: 30 threads, 10 rounds, background workers {}.".format(
                    count + 1, len(plan), c["label"], c["workload"], c["num_bg"]), flush=True)
                receipt = execute(args, c, path, expected)
                frozen(args, original, env, build)
                atomic_json(path, receipt)
                accepted[c["workload"]][c["label"]] = receipt
                count += 1
                publish(args, accepted, key)
                metadata["completed"] = count
                atomic_json(session / "session.json", metadata)
                r = receipt["measurement"]
                print("Validated {} / {} round 9: {:.6f} Mops/s, P50 {} ns, P99 {} ns.".format(
                    c["workload"], c["label"], r["throughput_Mops"], r["p50_latency_ns"], r["p99_latency_ns"]), flush=True)
        except BaseException as error:
            metadata.update(status="failed", finished_utc=now(), error=english_text(error))
            atomic_json(session / "session.json", metadata)
            raise
        metadata.update(status="complete", finished_utc=now())
        atomic_json(session / "session.json", metadata)
        print("Collection complete. Saved " + english_text(args.output_dir / DATA_FILENAME), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        print("Error: " + english_text(error), file=sys.stderr)
        sys.exit(1)


