#!/usr/bin/env python3
"""Collect measured Timnah factor analysis with isolated ablation binaries."""

import real_trace_support
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
DATA_FILENAME = "figure13-factor-analysis.json"
VARIANTS = {
    "eh": {"label": "EH", "num_bg": 0, "tiered": False, "kv_lock": False, "data_migration": False, "hot_lock": False},
    "tiered": {"label": "Tiered EH", "num_bg": 0, "tiered": True, "kv_lock": False, "data_migration": False, "hot_lock": False},
    "kv": {"label": "+K/V Lock", "num_bg": 0, "tiered": True, "kv_lock": True, "data_migration": False, "hot_lock": False},
    "migration": {"label": "+Data Migration", "num_bg": 1, "tiered": True, "kv_lock": True, "data_migration": True, "hot_lock": False},
    "hot": {"label": "+Hot Lock", "num_bg": 1, "tiered": True, "kv_lock": True, "data_migration": True, "hot_lock": True},
}
WORKLOADS = {"a": (0.5, 0.5), "c": (1.0, 0.0), "ts": (0.99, 0.01), "w": (0.18, 0.82)}
EPOCH_OPS = 10000000
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
    """Hash every support source, including patch fragments and correctness tools."""
    paths = [path for path in support.rglob("*") if path.is_file() and
             (path.suffix in {".py", ".cc", ".cpp", ".h", ".hpp", ".inc", ".cmake"}
              or path.name == "CMakeLists.txt")]
    required = {"prepare.py", "driver.cpp", "CMakeLists.txt", "flat_methods.inc", "correctness.cpp"}
    result = {str(path.relative_to(support)): digest_file(path) for path in sorted(paths)}
    if not required.issubset(result):
        raise RuntimeError("The complete isolated Figure 13 support directory is required")
    return result


def generator_versions(support):
    return {name: digest_file(support / name) for name in ("prepare.py", "flat_methods.inc")}


def source_copy_valid(source, repository, support):
    try:
        manifest = json.loads((source / ".figure13-diagnostic-copy.json").read_text())
        if (manifest["schema"] != "figure13-diagnostic-source-v1" or
                manifest["repository"] != str(repository) or manifest["source_dir"] != str(source) or
                manifest.get("production_files_modified") is not False or
                manifest.get("full_hot_source_modified") is not False):
            return False
        # A source copy can be internally consistent yet originate from an
        # obsolete transformation. Recreate it whenever the patch generator
        # or included flat-EH methods change, before considering build caches.
        if manifest.get("generator_support_sha256") != generator_versions(support):
            return False
        for group, root in (("production_sources_sha256", repository),
                            ("diagnostic_sources_sha256", source)):
            if not isinstance(manifest[group], dict) or not manifest[group]:
                return False
            for name, value in manifest[group].items():
                path = root / name
                if Path(name).is_absolute() or not path.resolve().is_relative_to(root):
                    return False
                if digest_file(path) != value:
                    return False
        # The full Hot Lock endpoint must compile an unchanged Timnah snapshot.
        for name, value in manifest["production_sources_sha256"].items():
            if name.startswith("hashindex/timnah/") and manifest["diagnostic_sources_sha256"].get(name) != value:
                return False
        actual = {str(path.relative_to(source)) for path in source.rglob("*")
                  if path.is_file() and path.name != ".figure13-diagnostic-copy.json"}
        return actual == set(manifest["diagnostic_sources_sha256"])
    except (OSError, ValueError, KeyError, TypeError):
        return False


def runtime_versions(binary):
    linked = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, check=True)
    if "not found" in linked.stdout:
        raise RuntimeError("An isolated Figure 13 runtime dependency is unresolved")
    paths = {Path(name).resolve() for name in re.findall(r"(?:=>\s+)?(/[^\s]+)", linked.stdout)
             if Path(name).is_file()}
    return {str(path): digest_file(path) for path in sorted(paths)}


def build_files(build):
    return {str(path.relative_to(build)): digest_file(path)
            for path in sorted(build.rglob("*")) if path.is_file() and
            path.name in ("flags.make", "link.txt", "CMakeCache.txt", "build.ninja", "rules.ninja")}


def build_diagnostic(args):
    source, build, support = args.source_dir, args.build_dir, args.support_dir
    if not source_copy_valid(source, args.repository, support):
        if args.validate_only:
            return None
        sys.dont_write_bytecode = True
        spec = importlib.util.spec_from_file_location("figure13_prepare", support / "prepare.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.prepare(args.repository, source)
    if not source_copy_valid(source, args.repository, support):
        raise RuntimeError("The isolated ablation source copy did not pass validation")
    inputs = {"source_manifest": digest_file(source / ".figure13-diagnostic-copy.json"),
              "support": support_versions(support), "build_type": "Release",
              "source": str(source), "support_dir": str(support)}
    cache = build / "figure13-build-manifest.json"
    try:
        old = json.loads(cache.read_text())
        if (old["inputs"] == inputs and set(old["binaries"]) == set(VARIANTS) and
                old["build_files_sha256"] == build_files(build) and
                all(digest_file(name) == value for name, value in old["linked_libraries_sha256"].items()) and
                all(digest_file(build / ("figure13_" + name)) == row["sha256"]
                    for name, row in old["binaries"].items())):
            return old
    except (OSError, ValueError, KeyError, TypeError):
        pass
    if args.validate_only:
        return None
    build.mkdir(parents=True, exist_ok=True)
    commands = [
        ["cmake", "-S", str(support), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
         "-DFIGURE13_REPOSITORY=" + str(args.repository), "-DFIGURE13_SOURCE_ROOT=" + str(source)],
        ["cmake", "--build", str(build), "--target"] + ["figure13_" + name for name in VARIANTS]
        + ["-j", str(args.jobs)],
    ]
    for number, command in enumerate(commands):
        result = subprocess.run(command, capture_output=True)
        path = build / ("configure.log" if number == 0 else "compile.log")
        atomic_bytes(path, english_text((result.stdout + result.stderr).decode("utf-8", "replace")).encode("ascii"))
        if result.returncode:
            raise RuntimeError("Isolated Figure 13 build failed; see " + str(path))
    if (not source_copy_valid(source, args.repository, support) or support_versions(support) != inputs["support"] or
            digest_file(source / ".figure13-diagnostic-copy.json") != inputs["source_manifest"]):
        raise RuntimeError("An ablation source or support file changed during the build")
    binaries, dependencies = {}, {}
    for name in VARIANTS:
        binary = build / ("figure13_" + name)
        linked = runtime_versions(binary)
        for path in linked:
            if Path(path).is_relative_to(args.repository / "build"):
                raise RuntimeError("Ablation binaries must not load production index libraries: " + path)
        binaries[name] = {"path": str(binary), "sha256": digest_file(binary)}
        dependencies.update(linked)
    manifest = {"inputs": inputs, "created_utc": now(), "binaries": binaries,
                "linked_libraries_sha256": dependencies, "build_files_sha256": build_files(build)}
    atomic_json(cache, manifest)
    return manifest


def require_fields(actual, expected, message):
    if any(actual.get(name) != str(value) for name, value in expected.items()):
        raise ValueError(message)


def parse_run(output, config):
    """Validate real all-round counters before accepting the final round metric."""
    def one(prefix):
        rows = re.findall(r"^" + re.escape(prefix) + r" (.+)$", output, re.M)
        if len(rows) != 1:
            raise ValueError("Expected exactly one " + prefix + " record")
        return fields(rows[0])
    replay = None
    if config['workload'] in ('ts','w'):
        replay = real_trace_support.validate(output,config['workload'],10,config['num_run'],config['num_load'])
        config = dict(config,num_load=replay['preload_keys'])
    variant, workload = config["variant"], config["workload"]
    design = VARIANTS[variant]
    header = one("Figure13Config")
    expected = {"variant_id": variant, "workload": workload, "num_threads": 30,
                "num_load_threads": 30, "num_run_threads": 30, "num_rounds": 10,
                "num_bg": design["num_bg"], "num_load": config["num_load"], "num_run": config["num_run"],
                "latency": "true", "latency_sample_rate": "0.01", "cpu_node": 0,
                "fast_node": 0, "slow_node": 2, "latency_unit": "ns",
                "full_hot_source": "unchanged_production_snapshot",
                "metric": "whole_round_successful_operations/elapsed_seconds",
                "quantile_method": "floor_probability_times_samples_minus_one"}
    expected.update({name: int(design[name]) for name in ("tiered", "kv_lock", "data_migration", "hot_lock")})
    require_fields(header, expected, "Ablation feature switches or benchmark configuration differ")
    if replay is None:
        ratio_rows=re.findall(r'^YCSB workload='+workload+r' (.+)$',output,re.M)
        if len(ratio_rows)!=1:raise ValueError('Missing YCSB header')
        ratios=fields(ratio_rows[0]);read_ratio,update_ratio=WORKLOADS[workload]
        for name,value in [('read',read_ratio),('update',update_ratio),('insert',0),('remove',0),('negative',0)]:
            if float(ratios[name])!=value:raise ValueError('Invalid YCSB ratios')
        if ratios.get('distribution')!='zipfian':raise ValueError('Invalid YCSB distribution')
        profile=None
    else:
        ratios=replay['ratios'];profile=replay['profile'];read_ratio=float(replay['ratios']['read']);update_ratio=1-read_ratio
    load = one("Figure13Load")
    require_fields(load, {"operations": config["num_load"], "successful_operations": config["num_load"],
                         "failed_operations": 0, "size": config["num_load"]},
                   "Not all preload records were inserted successfully")
    rounds = []
    integer_fields = ("round", "operations", "successful_operations", "failed_operations", "elapsed_ns",
                      "p50_latency_ns", "p99_latency_ns", "latency_samples", "search_operations", "update_operations", "size")
    for line in re.findall(r"^Figure13Round (.+)$", output, re.M):
        item = fields(line)
        row = {name: int(item[name]) for name in integer_fields}
        row["throughput_Mops"] = float(item["throughput_Mops"])
        rounds.append(row)
    if len(rounds) != 10:
        raise ValueError("Each ablation requires exactly ten real-round measurements")
    for number, row in enumerate(rounds):
        operations = config["num_run"]
        if (row["round"] != number or row["operations"] != operations or
                row["successful_operations"] != operations or row["failed_operations"] != 0 or
                row["size"] != config["num_load"] or
                row["search_operations"] + row["update_operations"] != operations):
            raise ValueError("Round coordinates, operation success or cardinality are inconsistent")
        rate = row["throughput_Mops"]
        if (not math.isfinite(rate) or rate <= 0 or row["elapsed_ns"] <= 0 or
                not math.isclose(rate, operations * 1000 / row["elapsed_ns"], rel_tol=1e-8, abs_tol=5e-9)):
            raise ValueError("Throughput differs from successful operations divided by elapsed time")
        if (row["p50_latency_ns"] <= 0 or row["p99_latency_ns"] < row["p50_latency_ns"] or
                not 0 < row["latency_samples"] <= operations):
            raise ValueError("Sampled latency percentiles or sample count are invalid")
        tolerance = max(30, 8 * math.sqrt(operations * read_ratio * (1 - read_ratio)))
        if replay is None and abs(row["search_operations"] - operations * read_ratio) > tolerance:
            raise ValueError("Observed operations differ from the configured workload ratios")
        if replay is not None and (row['search_operations']!=int(replay['audits'][number]['reads']) or row['update_operations']!=int(replay['audits'][number]['updates'])):
            raise ValueError('Observed trace operations disagree with successful-operation counts')
        if workload == "c" and row["search_operations"] != operations:
            raise ValueError("YCSB C must perform only successful searches")
        # Native Dice<256> repeats a fixed Bernoulli-generated bit pattern;
        # its realized frequency need not equal the requested probability.
        # Preserve this sampler and report actual samples without treating
        # successive rolls as independent binomial observations.
    native = []
    for coordinate, text in re.findall(r"^Figure13NativeStatistics round=(\S+) (.+)$", output, re.M):
        if not text.startswith("Timnah "):
            raise ValueError("The native statistics identify an unexpected index implementation")
        statistics = fields(text)
        require_fields(statistics, {"size": config["num_load"], "fast_node": 0, "slow_node": 2},
                       "Native cardinality or NUMA node configuration changed")
        if not design["data_migration"]:
            require_fields(statistics, {"samples": 0, "reorders": 0, "promotions": 0,
                                      "demotions": 0, "decay_events": 0, "tracking": 0},
                           "A maintenance-disabled ablation performed tracking or migration")
        native.append({"round": coordinate, "text": text, "fields": statistics})
    if [row["round"] for row in native] != ["load"] + [str(i) for i in range(10)]:
        raise ValueError("Exactly one native statistics call per real round is required")
    timeline = re.findall(r"^(Figure13Load|Figure13Round|Figure13NativeStatistics) (.+)$", output, re.M)
    if [kind for kind, _ in timeline] != ["Figure13Load", "Figure13NativeStatistics"] + [
            kind for _ in range(10) for kind in ("Figure13Round", "Figure13NativeStatistics")]:
        raise ValueError("Native statistics were not emitted once at each real-round boundary")
    memory = []
    for text in re.findall(r"^Figure13Memory (.+)$", output, re.M):
        row = fields(text)
        placement = ("single_directory_first_4096_segments_fast_remainder_slow" if variant == "eh"
                     else "separate_fixed_fast_and_growable_slow_tables")
        require_fields(row, {"fast_node": 0, "slow_node": 2, "metadata_node": 0,
                            "fast_bucket_capacity_bytes": 268435456,
                            "fast_counter_capacity_bytes": 67108864,
                            "budget_scope": "match_current_Timnah_bucket_and_counter_capacities",
                            "reported_payload_scope": "bucket_and_counter_bytes_only",
                            "metadata_and_arena_reservation_included": "false",
                            "placement_policy": placement}, "Ablation memory policy changed")
        sizes = {name: int(row[name]) for name in ("fast_bucket_bytes", "fast_counter_bytes", "slow_bucket_bytes",
                                                    "slow_counter_bytes", "fast_segments", "slow_segments")}
        if any(value < 0 for value in sizes.values()) or sizes["fast_bucket_bytes"] > 268435456 or sizes["fast_counter_bytes"] > 67108864:
            raise ValueError("Ablation fast-tier memory exceeds the controlled capacities")
        if (sizes["fast_bucket_bytes"] != sizes["fast_segments"] * 65536 or
                sizes["slow_bucket_bytes"] != sizes["slow_segments"] * 65536 or
                sizes["fast_counter_bytes"] != sizes["fast_segments"] * 16384 or
                sizes["slow_counter_bytes"] != sizes["slow_segments"] * 16384):
            raise ValueError("Segment, bucket and counter allocations are inconsistent")
        memory.append(row)
    if [row["phase"] for row in memory] != ["initial", "loaded", "complete"]:
        raise ValueError("Expected initial, loaded and complete memory receipts")
    if design["data_migration"] and int(native[-1]["fields"]["samples"]) <= 0:
        raise ValueError("The migration-enabled ablation did not sample any heat")
    complete = one("Figure13Complete")
    require_fields(complete, {"variant_id": variant, "workload": workload, "rounds": 10,
                             "operations": config["num_run"] * 10,
                             "successful_operations": config["num_run"] * 10,
                             "failed_operations": 0, "size": config["num_load"], "verified_rounds": 10},
                   "The final ablation completion or cardinality verification failed")
    return rounds, {"config": header, "ratios": ratios, "profile": profile,
                    "load": load, "statistics": native, "memory": memory, "complete": complete}


def configuration(args, workload, variant):
    design = VARIANTS[variant]
    command = [str(args.build_dir / ("figure13_" + variant)), "-index=Timnah",
               "-workload=" + workload, "-num_rounds=10", "-num_threads=30",
               "-num_load_threads=30", "-num_run_threads=30", "-num_bg=" + str(design["num_bg"]),
               "-num_load=" + str(args.num_load), "-num_run=" + str(args.num_run),
               "-rndSeed=1729", "-latency=true", "-latency_sample_rate=0.01", "-bind_core=true",
               "-verify=false", "-shift=false", "-shuffle=true", "-keySize=8", "-valueSize=8",
               "-real_epoch_ops=" + str(EPOCH_OPS),
               "-real_drift_stride=" + str(max(args.num_load // 100, 1))]
    return {"variant": variant, "label": design["label"], "workload": workload,
            "num_bg": design["num_bg"], "num_threads": 30, "num_rounds": 10,
            "num_load": args.num_load, "num_run": args.num_run, "real_epoch_ops": EPOCH_OPS,
            "real_drift_stride": max(args.num_load // 100, 1), "command": command}


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
                        raise TimeoutError("The ablation benchmark exceeded the requested timeout")
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
        raise RuntimeError("Ablation benchmark exited with {}. See {}".format(code, log))
    rounds, native = parse_run(raw.decode("utf-8", "replace"), config)
    return {"fingerprint": expected, "config": config, "exit_code": code,
            "started_utc": started, "finished_utc": now(), "elapsed_seconds": time.monotonic() - begin,
            "stdout_gzip": str(compressed), "stdout_sha256": digest_bytes(raw), "log": str(log),
            "log_sha256": digest_file(log), "rounds": rounds, "measurement": rounds[-1], "native": native}


def assert_idle():
    for path in Path("/proc").glob("[0-9]*/comm"):
        try:
            name = path.read_text().strip()
            if name in ("ycsb", "ycsb.before", "figure11_probe", "figure12_probe") or name.startswith("figure13_"):
                if (path.parent / "stat").read_text().split(") ", 1)[1][0] != "Z":
                    raise RuntimeError("Another benchmark is active: " + path.parent.name)
        except (OSError, PermissionError):
            pass


def frozen(args, original, env, build):
    if digest_file(Path(__file__).resolve()) != args.collector_sha256:
        raise RuntimeError("The collection script changed during collection")
    if production_versions(args.repository) != original or environment() != env:
        raise RuntimeError("Production sources, binary, libraries or runtime environment changed")
    if (not source_copy_valid(args.source_dir, args.repository, args.support_dir) or
            digest_file(args.source_dir / ".figure13-diagnostic-copy.json") != build["inputs"]["source_manifest"] or
            support_versions(args.support_dir) != build["inputs"]["support"] or
            build_files(args.build_dir) != build["build_files_sha256"]):
        raise RuntimeError("The isolated source, support code or build flags changed")
    for variant, record in build["binaries"].items():
        if digest_file(args.build_dir / ("figure13_" + variant)) != record["sha256"]:
            raise RuntimeError("An ablation binary changed during collection")
    for name, value in build["linked_libraries_sha256"].items():
        if digest_file(name) != value:
            raise RuntimeError("An ablation runtime library changed during collection")


def publish(args, accepted, key, manifest):
    workloads = {name: {} for name in WORKLOADS}
    for workload, rows in accepted.items():
        workloads[workload] = {label: record["measurement"] for label, record in rows.items()}
    complete = all(set(workloads[name]) == {value["label"] for value in VARIANTS.values()} for name in WORKLOADS)
    output = {"schema_version": SCHEMA_VERSION, "metric": METRIC, "num_threads": 30, "num_rounds": 10,
              "num_load": args.num_load, "num_run": args.num_run, "round": 9,
              "complete": complete, "version_fingerprint": key, "updated_utc": now(),
              "workloads": workloads, "runs": accepted,
              "mapping_assumptions": manifest["mapping_assumptions"],
              "note": "Twenty independent measured runs: five incremental Timnah-derived designs under "
                      "YCSB A, YCSB C, Twitter-Storage and Webmail. Each record is real round 9. "
                      "Throughput is successful operations per complete-round wall time in Mops/s; "
                      "P50/P99 are ordinary 0.01 sampled operation latencies in nanoseconds, using the "
                      "existing percentile estimator. No interpolation or constructed performance values. "
                      "The copied full Hot Lock implementation is byte-identical to current Timnah. "
                      "Production ycsb, index sources and libraries are unchanged. "
                      "All variants match current Timnah capacities: 256 MiB fast buckets plus 64 MiB "
                      "fast counters, metadata separately on node 0; this is not a 256 MiB total-index budget. "
                      "Current 0.002 static convergence and continuous dynamic-profile adaptation are preserved."}
    atomic_json(args.output_dir / DATA_FILENAME, output)


def parser():
    folder = Path(__file__).resolve().parent
    p = EnglishArgumentParser(description=__doc__)
    p.add_argument("--repository", type=Path, default=folder.parent)
    p.add_argument("--output-dir", type=Path, default=folder)
    p.add_argument("--support-dir", type=Path, default=folder / "figure13_support")
    p.add_argument("--source-dir", type=Path, default=folder / ".figure13-source")
    p.add_argument("--build-dir", type=Path, default=folder / ".figure13-build")
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
    if (args.num_load < 10000 or args.num_run < 10000 or args.num_load % 10 or args.num_run % 10 or
            args.jobs < 1 or not math.isfinite(args.timeout) or args.timeout < 0):
        raise ValueError("Counts must be >= 10000 and divisible by ten; jobs must be positive")
    if len(args.workloads) != len(set(args.workloads)) or len(args.variants) != len(set(args.variants)):
        raise ValueError("Workloads and variants must be unique")
    for name in ("repository", "output_dir", "support_dir", "source_dir", "build_dir"):
        setattr(args, name, getattr(args, name).resolve())
    for name in ("source_dir", "build_dir"):
        path = getattr(args, name)
        if path == args.repository or args.repository.is_relative_to(path):
            raise ValueError("Diagnostic directories cannot replace the repository")
        if any(path.is_relative_to(args.repository / item) for item in ("benchmark", "hashindex", "utils", "build")):
            raise ValueError("Diagnostic directories must be outside production directories")
    if args.source_dir == args.build_dir or args.source_dir.is_relative_to(args.build_dir) or args.build_dir.is_relative_to(args.source_dir):
        raise ValueError("Isolated source and build directories must be separate")
    plan = [configuration(args, workload, variant) for workload in args.workloads for variant in args.variants]
    if args.dry_run:
        for config in plan:
            print(english_text(" ".join(config["command"])))
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
            print("No validated Figure 13 ablation build is cached.")
            return 2
        frozen(args, original, env, build)
        versions = {"real_trace_data": real_trace_support.datasets(args.repository), "production": original, "build": build,
                    "collector_sha256": args.collector_sha256}
        key = fingerprint({"versions": versions, "environment": env,
                           "num_load": args.num_load, "num_run": args.num_run})
        session = args.output_dir / "runs-figure13" / key[:16]
        session.mkdir(parents=True, exist_ok=True)
        accepted = {name: {} for name in WORKLOADS}
        missing = []
        for config in plan:
            path = session / (config["workload"] + "-" + config["variant"] + ".json")
            expected = fingerprint({"session": key, "config": config})
            record = None if args.force else cached(path, expected, config)
            if record is None:
                missing.append((config, path, expected))
            else:
                accepted[config["workload"]][config["label"]] = record
        count = len(plan) - len(missing)
        print("Validated cached ablation runs: {}/{}. Missing: {}.".format(count, len(plan), len(missing)), flush=True)
        if args.validate_only:
            return 0 if not missing else 2
        metadata = {"schema_version": 1, "status": "running", "started_utc": now(),
                    "versions": versions, "environment": env, "plan": plan,
                    "completed": count, "expected": len(plan)}
        atomic_json(session / "session.json", metadata)
        source_manifest = json.loads((args.source_dir / ".figure13-diagnostic-copy.json").read_text())
        publish(args, accepted, key, source_manifest)
        try:
            for config, path, expected in missing:
                assert_idle()
                frozen(args, original, env, build)
                print("[{}/{}] Running {} on workload {}: 30 threads, 10 rounds, background workers {}.".format(
                    count + 1, len(plan), config["label"], config["workload"], config["num_bg"]), flush=True)
                record = execute(args, config, path, expected)
                frozen(args, original, env, build)
                atomic_json(path, record)
                accepted[config["workload"]][config["label"]] = record
                count += 1
                publish(args, accepted, key, source_manifest)
                metadata["completed"] = count
                atomic_json(session / "session.json", metadata)
                measured = record["measurement"]
                print("Validated {} / {} round 9: {:.6f} Mops/s, P50 {} ns, P99 {} ns.".format(
                    config["workload"], config["label"], measured["throughput_Mops"],
                    measured["p50_latency_ns"], measured["p99_latency_ns"]), flush=True)
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
