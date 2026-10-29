#!/usr/bin/env python3
"""Collect measured round-9 throughput for 16-byte-key YCSB Figure 17."""

import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

THREAD_COUNTS = (1, 5, 10, 15, 20, 25, 30, 35, 40, 45, 48)
METHODS = {"Timnah": 1, "Dash": 0, "CHash": 0}
WORKLOADS = {"a": (0.5, 0.5, 0.0), "b": (0.95, 0.05, 0.0),
             "c": (1.0, 0.0, 0.0), "d": (0.95, 0.0, 0.05)}
SCHEMA_VERSION = 1
SOURCE_SUFFIXES = {".h", ".hpp", ".cc", ".cpp", ".cmake", ".txt"}
ENV_PREFIXES = ("LD_", "OMP_", "GOMP_", "KMP_", "MALLOC_")


def english(value):
    return str(value).encode("ascii", "backslashreplace").decode("ascii")


class EnglishArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        super().error(english(message))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def digest_file(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def digest_value(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=True,
                                    allow_nan=False).encode("ascii")).hexdigest()


def atomic_bytes(path, content):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name(path.name + ".pending")
    with pending.open("wb") as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(pending, path)


def atomic_json(path, content):
    atomic_bytes(path, (json.dumps(content, indent=2, ensure_ascii=True,
                                  allow_nan=False) + "\n").encode("ascii"))


def fields(text):
    return dict(re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", text))


def one_line(output, prefix):
    found = re.findall(r"^" + re.escape(prefix) + r"([^\n]+)$", output, re.M)
    require(len(found) == 1, "Expected exactly one " + prefix.strip())
    return found[0]


def equal_fields(actual, expected, context):
    for name, value in expected.items():
        require(actual.get(name) == str(value), context + ": incorrect " + name)


def native_stat(text, prefix, index):
    matches = list(re.finditer(r"^" + re.escape(prefix) + r"([^\n]+)$", text, re.M))
    require(len(matches) == 1, "Expected exactly one " + prefix.strip())
    line = matches[0][1]
    name, separator, remainder = line.partition(" ")
    require(name == index and separator, "The native index identity differs")
    continuation = text[matches[0].end():].split("\n\n", 1)[0]
    return fields(remainder + continuation)


def parse_run(output, config):
    """Reject scalar, incomplete, failed, or differently configured measurements."""
    require(isinstance(output, str) and output.isascii(), "Benchmark output must be English ASCII")
    index, workload = config["index"], config["workload"]
    require(index in METHODS and workload in WORKLOADS, "Unknown index or workload")
    require(config["num_rounds"] == 10 and config["key_bytes"] == 16
            and config["value_bytes"] == 8, "Figure 17 requires 10 rounds and 16-byte keys")
    header = one_line(output, "YCSB workload=")
    name, separator, remainder = header.partition(" ")
    require(name == workload and separator, "The workload header differs")
    ratios = WORKLOADS[workload]
    header_fields = fields(remainder)
    for key, value in zip(("read", "update", "insert", "remove", "negative"), ratios + (0.0, 0.0)):
        require(key in header_fields and float(header_fields[key]) == value,
                "The workload operation ratios differ")
    equal_fields(header_fields, {"distribution": "zipfian"}, "Workload")
    mode = fields(one_line(output, "Variable-key mode "))
    equal_fields(mode, {"key_bytes": 16, "value_bytes": 8, "pointer_bytes": 8,
                       "fingerprint_bits": 16, "cpu_node": 0,
                       "cpu_binding_enabled": "true", "paper_fast_initial_mib": 512,
                       "paper_slow_initial_mib": 2048,
                       "counters_and_metadata_accounted_separately": "true"}, "Variable mode")
    trace = fields(one_line(output, "Variable trace "))
    equal_fields(trace, {"key_bytes": 16, "minimum_key_bytes": 16, "value_bytes": 8,
                        "preload": config["num_load"], "operations_per_round": config["num_run"],
                        "seed": 1729, "shared_trace": "true", "generation_in_throughput": "false",
                        "removes": 0, "negatives": 0}, "Variable trace")
    counts = {name: int(trace[name]) for name in ("reads", "updates", "inserts", "removes", "negatives")}
    require(all(value >= 0 for value in counts.values()) and sum(counts.values()) == config["num_run"],
            "Trace operation counts do not sum to the requested count")
    for key, fraction in zip(("reads", "updates", "inserts"), ratios):
        if fraction == 0:
            require(counts[key] == 0, "The trace contains an operation excluded by the workload")
    if workload == "c":
        require(counts["reads"] == config["num_run"], "Read-only trace includes another operation")
    storage = fields(one_line(output, "Variable-key storage "))
    equal_fields(storage, {"key_payload_node": 2, "trace_storage_excluded_from_index_budget": "true"}, "Key storage")
    initial = native_stat(output, "Variable initial statistics: ", index)
    equal_fields(initial, {"size": 0}, "Initial index")
    if index == "Timnah":
        equal_fields(initial, {"fast_bucket_bytes": 512 << 20, "slow_bucket_bytes": 2 << 30,
                               "fast_node": 0, "slow_node": 2}, "Initial Timnah tiers")
    else:
        equal_fields(initial, {"fast_reserved": 512 << 20, "slow_reserved": 2 << 30,
                               "dram_node": 0, "slow_node": 2, "key_payload_node": 2}, "Initial index tiers")
    load = fields(one_line(output, "Variable load "))
    equal_fields(load, {"operations": config["num_load"], "failed_operations": 0,
                       "key_bytes": 16, "key_payload_node": 2}, "Bulk load")
    loaded = native_stat(output, "Load statistics: ", index)
    equal_fields(loaded, {"size": config["num_load"]}, "Loaded index")
    if "misses" in loaded:
        equal_fields(loaded, {"misses": 0}, "Loaded index")
    markers = list(re.finditer(r"^Round (\d+)$", output, re.M))
    require([int(marker[1]) for marker in markers] == list(range(10)),
            "Every requested round must occur exactly once")
    require(output.count("Variable-key benchmark completed successfully.") == 1,
            "The successful completion marker is missing")
    rounds = []
    expected_size = config["num_load"] + counts["inserts"]
    for number, marker in enumerate(markers):
        end = markers[number + 1].start() if number < 9 else len(output)
        block = output[marker.end():end]
        rate = float(one_line(block, "Run throughput: ").removesuffix(" Mops/s"))
        require(math.isfinite(rate) and rate > 0, "Throughput must be finite and positive")
        summary = fields(one_line(block, "Variable round summary: "))
        duplicates = counts["inserts"] if number else 0
        equal_fields(summary, {"round": number, "operations": config["num_run"],
                               "successful_operations": config["num_run"] - duplicates,
                               "failed_operations": 0,
                               "expected_duplicate_insert_rejections": duplicates,
                               "expected_negative_reads": 0, "key_bytes": 16}, "Round completion")
        completed = sum(int(summary[name]) for name in ("successful_operations", "failed_operations",
                        "expected_duplicate_insert_rejections", "expected_negative_reads"))
        require(completed == config["num_run"], "Round outcomes do not account for every operation")
        elapsed = int(summary["elapsed_ns"])
        require(elapsed > 0 and abs(rate - config["num_run"] * 1000.0 / elapsed) <= 0.000501,
                "Throughput disagrees with the exact operation timer")
        printed = one_line(block, "Run time: ")
        match = re.fullmatch(r"([0-9]+\.[0-9]{6}) s", printed)
        require(match is not None and abs(float(match[1]) * 1e9 - elapsed) <= 500.01,
                "Printed run timing disagrees with the exact timer")
        stat = native_stat(block, "Run statistics: ", index)
        equal_fields(stat, {"size": expected_size}, "Round index size")
        if "misses" in stat:
            equal_fields(stat, {"misses": 0}, "Round index")
        if index == "Timnah":
            equal_fields(stat, {"fast_node": 0, "slow_node": 2}, "Round Timnah tiers")
        else:
            equal_fields(stat, {"dram_node": 0, "slow_node": 2, "key_payload_node": 2}, "Round index tiers")
        require(int(summary["key_arena_bytes"]) >= int(load["key_arena_bytes"]),
                "Key payload accounting decreased unexpectedly")
        rounds.append({"round": number, "throughput_Mops": rate,
                       "summary": summary, "statistics": stat})
    return {"rounds": rounds, "throughput_Mops": rounds[9]["throughput_Mops"],
            "mode": mode, "trace": trace, "initial_statistics": initial,
            "load": load, "load_statistics": loaded}


def environment():
    names = {name for name in os.environ if name.startswith(ENV_PREFIXES)}
    names.update(("PATH", "GLIBC_TUNABLES", "LD_LIBRARY_PATH", "LD_PRELOAD"))
    return {"variables": {name: os.environ.get(name) for name in sorted(names)},
            "parent_cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
            "node0_cpulist": Path("/sys/devices/system/node/node0/cpulist").read_text().strip()}


def versions(binary, repository):
    """Anchor both dispatch targets, actual variable dependencies, and compiled inputs."""
    sidecar = binary.with_name("ycsb.dispatch.json")
    dispatch = json.loads(sidecar.read_text(encoding="ascii"))
    require(dispatch.get("schema_version") == 1, "Unknown executable dispatch manifest")
    scalar = Path(dispatch["scalar_binary"]).resolve(strict=True)
    variable = Path(dispatch["variable_binary"]).resolve(strict=True)
    alias = Path(dispatch["scalar_alias"])
    require(not alias.is_symlink() and alias.is_file() and os.path.samefile(alias, scalar),
            "The scalar fingerprint alias is inconsistent")
    require(digest_file(binary) == dispatch["dispatcher_sha256"], "The dispatcher changed")
    require(digest_file(scalar) == dispatch["scalar_sha256"] == dispatch["scalar_alias_sha256"],
            "The scalar executable changed")
    require(digest_file(variable) == dispatch["variable_sha256"], "The variable executable changed")
    paths = {binary, sidecar, scalar, variable, alias.resolve(), Path(__file__).resolve()}
    libraries = {}
    for executable in (binary, scalar, variable):
        linked = subprocess.run(["ldd", str(executable)], capture_output=True, text=True, check=True).stdout
        require("not found" not in linked, "An executable dependency cannot be resolved")
        libraries[str(executable)] = re.sub(r"\(0x[0-9A-Fa-f]+\)", "(address)", linked)
        for name in re.findall(r"(/[^\s]+)", linked):
            candidate = Path(name)
            if candidate.is_file():
                paths.add(candidate.resolve())
    for directory in ("hashindex", "utils", "benchmark"):
        source = repository / directory
        if source.is_dir():
            paths.update(path.resolve() for path in source.rglob("*")
                         if path.is_file() and path.suffix in SOURCE_SUFFIXES)
    for name in ("CMakeLists.txt", "dependency.cmake", "build/CMakeCache.txt",
                 "benchmark/variable_keys/build.py", "build/variable_keys/CMakeCache.txt"):
        candidate = repository / name
        if candidate.is_file():
            paths.add(candidate.resolve())
    return {"paths_sha256": {str(path): digest_file(path) for path in sorted(paths)},
            "dispatch": dispatch, "dependencies": libraries,
            "actual_variable_executable": str(variable)}


def configuration(args, workload, index, threads):
    command = [str(args.binary), "-v", "-keySize=16", "-valueSize=8", "-variable_min_key_size=0",
               "-index=" + index, "-workload=" + workload, "-num_rounds=10",
               "-num_threads=" + str(threads), "-num_load_threads=" + str(threads),
               "-num_run_threads=" + str(threads), "-num_bg=" + str(METHODS[index]),
               "-num_load=" + str(args.num_load), "-num_run=" + str(args.num_run),
               "-skewness=0.99", "-rndSeed=1729", "-bind_core=true", "-latency=true",
               "-latency_sample_rate=0.01", "-verify=false", "-shift=false", "-shuffle=true"]
    return {"workload": workload, "index": index, "num_threads": threads,
            "num_bg": METHODS[index], "num_rounds": 10, "round": 9,
            "num_load": args.num_load, "num_run": args.num_run,
            "key_bytes": 16, "value_bytes": 8, "command": command}


def fingerprint(config, manifest, env):
    return digest_value({"schema_version": SCHEMA_VERSION, "config": config,
                         "versions": manifest, "environment": env})


def receipt_path(session_dir, config):
    return session_dir / "{}-{}-t{}.json".format(config["workload"], config["index"], config["num_threads"])


def load_cached(path, expected, config):
    try:
        receipt = json.loads(Path(path).read_text(encoding="ascii"))
        require(receipt.get("schema_version") == SCHEMA_VERSION
                and receipt["fingerprint"] == expected and receipt["config"] == config
                and receipt["exit_code"] == 0 and receipt["validated"] is True,
                "The cached receipt does not match this run")
        log = Path(receipt["log_path"])
        require(log.resolve() == Path(path).with_suffix(".log").resolve(), "The cached log path differs")
        require(digest_file(log) == receipt["log_sha256"], "The cached log hash differs")
        parsed = parse_run(log.read_text(encoding="ascii"), config)
        require(parsed == receipt["measurement"], "The cached parse differs")
        return receipt
    except (OSError, ValueError, KeyError, TypeError, UnicodeError):
        return None


def active_benchmarks():
    result = []
    for path in Path("/proc").glob("[0-9]*/comm"):
        try:
            name = path.read_text().strip()
            active = name in ("ycsb", "ycsb8", "ycsb_variable", "ycsb.before", "figure11_probe", "figure12_probe")
            active = active or name.startswith(("figure13_", "figure14_", "figure15_", "figure16_", "figure17_"))
            if active and (path.parent / "stat").read_text().split(") ", 1)[1][0] != "Z":
                result.append(int(path.parent.name))
        except (OSError, ProcessLookupError):
            pass
    return result


def stop_process(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def execute(args, config, path, expected, manifest, env):
    running = active_benchmarks()
    require(not running, "Another benchmark is active: " + ",".join(map(str, running)))
    path.parent.mkdir(parents=True, exist_ok=True)
    part = path.with_suffix(".stdout.part")
    log = path.with_suffix(".log")
    started, timer = utc_now(), time.monotonic()
    process = None
    observed_affinity = None
    observed_executable = None
    node0 = env["node0_cpulist"]
    actual = Path(manifest["actual_variable_executable"])
    try:
        with part.open("wb") as stream:
            process = subprocess.Popen(config["command"], cwd=args.binary.parent,
                                       stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            heartbeat = timer
            while True:
                try:
                    status = Path("/proc/{}/status".format(process.pid)).read_text()
                    match = re.search(r"^Cpus_allowed_list:\s*(.+)$", status, re.M)
                    if match and match[1] == node0:
                        observed_affinity = match[1]
                    exe = os.readlink("/proc/{}/exe".format(process.pid))
                    if Path(exe).resolve() == actual:
                        observed_executable = exe
                except (OSError, ProcessLookupError):
                    pass
                try:
                    code = process.wait(timeout=0.1)
                    break
                except subprocess.TimeoutExpired:
                    elapsed = time.monotonic() - timer
                    if args.timeout and elapsed > args.timeout:
                        raise TimeoutError("The benchmark exceeded its configured timeout")
                    if time.monotonic() - heartbeat >= 30:
                        print("Still running: workload={} index={} threads={} elapsed={:.0f}s".format(
                            config["workload"].upper(), config["index"], config["num_threads"], elapsed), flush=True)
                        heartbeat = time.monotonic()
    except BaseException:
        if process is not None:
            stop_process(process)
        raise
    finally:
        if part.exists():
            os.replace(part, log)
    require(code == 0, "Benchmark failed with exit code {}. See {}".format(code, log))
    require(observed_affinity == node0, "Node 0 CPU binding was not observed")
    require(observed_executable is not None, "The variable-key executable was not observed")
    parsed = parse_run(log.read_text(encoding="ascii"), config)
    return {"schema_version": SCHEMA_VERSION, "fingerprint": expected, "config": config,
            "started_utc": started, "finished_utc": utc_now(), "seconds": time.monotonic() - timer,
            "exit_code": code, "validated": True, "measurement": parsed,
            "log_path": str(log), "log_sha256": digest_file(log),
            "observed_cpu_affinity": observed_affinity, "observed_executable": observed_executable}


def full_plan(args):
    return [configuration(args, workload, index, threads)
            for workload in WORKLOADS for index in METHODS for threads in THREAD_COUNTS]


def current_cached(args, session_dir, manifest, env):
    accepted = {}
    for config in full_plan(args):
        receipt = load_cached(receipt_path(session_dir, config), fingerprint(config, manifest, env), config)
        if receipt is not None:
            accepted[(config["workload"], config["index"], config["num_threads"])] = receipt
    return accepted


def publish(args, accepted):
    """Publish a workload only when all 33 points have validated current receipts."""
    published = []
    for workload in WORKLOADS:
        keys = [(workload, index, threads) for index in METHODS for threads in THREAD_COUNTS]
        if not all(key in accepted for key in keys):
            continue
        document = {index: [] for index in METHODS}
        for index in METHODS:
            for threads in THREAD_COUNTS:
                receipt = accepted[(workload, index, threads)]
                config = receipt["config"]
                document[index].append({"index": index, "workload": workload,
                    "num_threads": threads, "num_rounds": 10, "round": 9,
                    "num_bg": config["num_bg"], "num_load": args.num_load, "num_run": args.num_run,
                    "key_bytes": 16, "value_bytes": 8,
                    "throughput_Mops": receipt["measurement"]["throughput_Mops"],
                    "fingerprint": receipt["fingerprint"],
                    "log_path": receipt["log_path"], "log_sha256": receipt["log_sha256"]})
        atomic_json(args.output_dir / ("figure17-ycsb-" + workload + ".json"), document)
        published.append(workload)
    return published


def parser():
    directory = Path(__file__).resolve().parent
    result = EnglishArgumentParser(description=__doc__)
    result.add_argument("--repository", type=Path, default=directory.parent)
    result.add_argument("--binary", type=Path, help="Installed ycsb dispatcher; defaults to REPOSITORY/build/benchmark/ycsb/ycsb")
    result.add_argument("--output-dir", type=Path, default=directory)
    result.add_argument("--workloads", nargs="+", choices=WORKLOADS, default=list(WORKLOADS))
    result.add_argument("--indices", nargs="+", choices=METHODS, default=list(METHODS))
    result.add_argument("--threads", nargs="+", type=int, choices=THREAD_COUNTS, default=list(THREAD_COUNTS))
    result.add_argument("--num-rounds", type=int, choices=(10,), default=10)
    result.add_argument("--num-load", type=int, default=100000000)
    result.add_argument("--num-run", type=int, default=100000000)
    result.add_argument("--timeout", type=float, default=0, help="Per-process timeout in seconds; 0 disables it")
    result.add_argument("--force", action="store_true", help="Rerun selected points even when their caches validate")
    result.add_argument("--dry-run", action="store_true", help="Print commands only; do not run ycsb or write files")
    result.add_argument("--validate-only", action="store_true", help="Validate selected cached points without running ycsb or publishing files")
    return result


def main(argv=None):
    args = parser().parse_args(argv)
    require(args.num_load > 0 and args.num_run > 0, "Record and operation counts must be positive")
    require(math.isfinite(args.timeout) and args.timeout >= 0, "Timeout must be finite and nonnegative")
    for values in (args.workloads, args.indices, args.threads):
        require(len(values) == len(set(values)), "Workloads, indices, and thread counts must be unique")
    require(not (args.force and args.validate_only), "--force cannot be combined with --validate-only")
    require(not (args.dry_run and args.validate_only), "--dry-run cannot be combined with --validate-only")
    args.repository = args.repository.resolve()
    args.binary = (args.binary or args.repository / "build/benchmark/ycsb/ycsb").resolve()
    args.output_dir = args.output_dir.resolve()
    plan = [configuration(args, workload, index, threads)
            for workload in args.workloads for index in args.indices for threads in args.threads]
    if args.dry_run:
        print("Figure 17: {} planned runs; 16-byte keys; round 9; 10 rounds per run.".format(len(plan)))
        for number, config in enumerate(plan, 1):
            print(english("[{}/{}] {}".format(number, len(plan), " ".join(config["command"]))))
        print(english("Output files: " + ", ".join(str(args.output_dir / ("figure17-ycsb-" + w + ".json")) for w in WORKLOADS)))
        return 0
    require(args.binary.is_file() and os.access(args.binary, os.X_OK), "The installed ycsb executable is unavailable")
    lock_path = args.repository / "figure/.collection.lock"
    with lock_path.open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("Another collector or build is using this project") from error
        manifest, env = versions(args.binary, args.repository), environment()
        session_key = digest_value({"schema_version": SCHEMA_VERSION, "versions": manifest,
                                   "environment": env, "settings": {"num_load": args.num_load, "num_run": args.num_run,
                                   "num_rounds": 10, "key_bytes": 16, "value_bytes": 8}})
        session_dir = args.output_dir / "runs-figure17" / session_key[:24]
        accepted = current_cached(args, session_dir, manifest, env)
        missing = []
        for config in plan:
            key = (config["workload"], config["index"], config["num_threads"])
            if args.force or key not in accepted:
                missing.append(config)
        print("Validated selected cached points: {}/{}. Missing points: {}.".format(
            len(plan) - len(missing), len(plan), len(missing)), flush=True)

        def unchanged():
            require(versions(args.binary, args.repository) == manifest and environment() == env,
                    "The executable, source, dependencies, or environment changed; collection stopped")

        unchanged()
        if args.validate_only:
            return 0 if not missing else 2
        session = {"schema_version": SCHEMA_VERSION, "started_utc": utc_now(), "session_key": session_key,
                   "versions": manifest, "environment": env, "plan": plan,
                   "note": "D uses a shared ratio-only trace; repeated insertions after round 0 are expected duplicate rejections. Throughput counts attempted operations."}
        atomic_json(session_dir / "session.json", session)
        publish(args, accepted)
        for number, config in enumerate(missing, 1):
            unchanged()
            print("[{}/{}] Running workload={} index={} threads={} rounds=10 background={}".format(
                number, len(missing), config["workload"].upper(), config["index"], config["num_threads"], config["num_bg"]), flush=True)
            path = receipt_path(session_dir, config)
            receipt = execute(args, config, path, fingerprint(config, manifest, env), manifest, env)
            unchanged()
            atomic_json(path, receipt)
            accepted[(config["workload"], config["index"], config["num_threads"])] = receipt
            published = publish(args, accepted)
            print("Measured round 9: {:.3f} Mops/s. Complete workload files: {}.".format(
                receipt["measurement"]["throughput_Mops"], len(published)), flush=True)
        unchanged()
        published = publish(args, accepted)
        session.update(finished_utc=utc_now(), selected_points=len(plan), validated_total_points=len(accepted),
                       published_workloads=published)
        atomic_json(session_dir / "session.json", session)
        print("Collection complete. Validated points: {}/132. Published workload files: {}/4.".format(len(accepted), len(published)))
        if len(published) < 4:
            print("Incomplete workload files were not replaced. Rerun without selection flags to complete the figure.")
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("Collection interrupted. Completed measurements remain available for resume.", file=sys.stderr)
        sys.exit(130)
    except Exception as error:
        print("Error: " + english(error), file=sys.stderr)
        sys.exit(1)
