#!/usr/bin/env python3
"""Collect measured round-9 throughput for Figure 10. No synthetic points."""

import real_trace_support
import argparse
import fcntl
import gzip
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
import zlib
from datetime import datetime, timezone

THREAD_COUNTS = (1, 5, 10, 15, 20, 25, 30, 35, 40, 45, 48)
METHODS = {"Timnah": ("Timnah", 1), "Dash": ("Dash", 0),
           "CHash": ("CHash", 0), "EH+cache": ("EH+cache", 0)}
WORKLOADS = ("ts", "tt", "tc", "w")
WORKLOAD_FILES = {"ts": "storage.json", "tt": "transient.json",
                  "tc": "compute.json", "w": "WebMail.json"}
PROFILE_NAMES = {"ts": "Twitter-Storage", "tt": "Twitter-Transient",
                 "tc": "Twitter-Compute", "w": "Webmail"}
EPOCH_OPS = 10000000
SCHEMA_VERSION = 2
ENV_KEYS = ("LD_LIBRARY_PATH", "LD_PRELOAD", "OMP_NUM_THREADS", "OMP_PLACES",
            "OMP_PROC_BIND", "GOMP_CPU_AFFINITY", "OMP_WAIT_POLICY",
            "KMP_AFFINITY", "KMP_BLOCKTIME", "MALLOC_CONF", "GLIBC_TUNABLES")
ENV_PREFIXES = ("LD_", "OMP_", "GOMP_", "KMP_", "MALLOC_")
ROUND = re.compile(r"^Round (\d+)\s*$", re.MULTILINE)
THROUGHPUT = re.compile(r"^(?:Run throughput|\u8fd0\u884c\u541e\u5410): "
                        r"([0-9]+(?:\.[0-9]+)?) Mops/s$", re.MULTILINE)
STATS = re.compile(r"^(?:Run statistics|\u6267\u884c\u9636\u6bb5\u7edf\u8ba1\u6570\u636e): (.*)$", re.MULTILINE)
LOAD_STATS = re.compile(r"^(?:Load statistics|\u8f7d\u5165\u9636\u6bb5\u7edf\u8ba1\u6570\u636e): (.*)$", re.MULTILINE)


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def english_text(value):
    return str(value).encode("ascii", errors="backslashreplace").decode("ascii")


class EnglishArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        super().error(english_text(message))


def digest_bytes(value):
    return hashlib.sha256(value).hexdigest()


def digest_file(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def atomic_bytes(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("wb") as stream:
        stream.write(value)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def atomic_json(path, value):
    atomic_bytes(path, (json.dumps(value, indent=2, ensure_ascii=True,
                                 allow_nan=False) + "\n").encode("ascii"))


def fields(text):
    return dict(re.findall(r"([a-zA-Z0-9_]+)=([^\s]+)", text))


def index_size(stats):
    for name in ("size", "eh_size"):
        if name in stats:
            return int(stats[name])
    raise ValueError("Index size is missing from statistics")


def check_index_identity(statistics, expected_index):
    if expected_index is None:
        return
    # The public paper label is EH+cache; its existing runtime statistics use EHCache.
    expected = "EHCache" if expected_index == "EH+cache" else expected_index
    actual = statistics.split(maxsplit=1)
    if not actual or actual[0] != expected:
        raise ValueError("The benchmark index identity does not match the request")


def parse_run(output, workload, num_rounds=10, num_load=100000000,
              expected_index=None, num_run=100000000):
    """Accept round 9 only after validating all rounds and dynamic trace audits."""
    if workload not in WORKLOADS or num_rounds < 10:
        raise ValueError("A supported workload and at least 10 rounds are required")
    replay = real_trace_support.validate(output,workload,num_rounds,num_run,num_load)
    num_load = replay['preload_keys']
    traces = replay['audits']
    load = list(LOAD_STATS.finditer(output))
    if len(load) != 1 or index_size(fields(load[0][1])) != num_load:
        raise ValueError("The authoritative index did not load the expected record count")
    check_index_identity(load[0][1], expected_index)
    markers = list(ROUND.finditer(output))
    if [int(marker[1]) for marker in markers] != list(range(num_rounds)):
        raise ValueError("The benchmark must contain every round exactly once")
    result = []
    previous_stats = fields(load[0][1])
    for position, marker in enumerate(markers):
        end = markers[position + 1].start() if position + 1 < len(markers) else len(output)
        block = output[marker.end():end]
        values = list(THROUGHPUT.finditer(block))
        statistics = list(STATS.finditer(block))
        if len(values) != 1 or len(statistics) != 1:
            raise ValueError("Each round needs one throughput and one complete statistics line")
        check_index_identity(statistics[0][1], expected_index)
        throughput = float(values[0][1])
        if not math.isfinite(throughput) or throughput <= 0:
            raise ValueError("Throughput must be a finite positive measured value")
        stats = fields(statistics[0][1])
        if index_size(stats) != num_load:
            raise ValueError("The index record count changed in a read/overwrite workload")
        if "misses" in stats and int(stats["misses"]) != 0:
            raise ValueError("The benchmark reported missing reads")
        if expected_index == "Timnah":
            if (stats.get("tracking") != "1" or stats.get("converged") != "0" or
                    stats.get("rates_frozen") != "0"):
                raise ValueError("Timnah must retain continuous dynamic-workload adaptation")
            counted_reads = sum(int(stats[k]) - int(previous_stats[k])
                                for k in ("fast_hits", "slow_hits"))
            if counted_reads != int(traces[position]["reads"]):
                raise ValueError("Timnah Search counters do not match the generated trace")
        result.append(dict(round=position, throughput_Mops=throughput,
                           stats=stats, trace=traces[position]))
        previous_stats = stats
    return result


def normalize_output(output):
    """Keep saved text logs in English; preserve original bytes in gzip."""
    replacements = {
        "\u6d4b\u8bd5\u54c8\u5e0c\u8868": "Index",
        "\u751f\u6210 trace \u4e2d": "Generating trace",
        "\u521d\u59cb\u5316 trace \u5b8c\u6210: \u5171\u5360\u636e": "Trace initialized: allocated",
        "\u521d\u59cb\u5316 trace \u7528\u65f6": "Trace initialization time",
        "\u521b\u5efa\u54c8\u5e0c\u8868": "Create index",
        "\u8f7d\u5165\u9636\u6bb5\u7edf\u8ba1\u6570\u636e": "Load statistics",
        "\u6267\u884c\u9636\u6bb5\u7edf\u8ba1\u6570\u636e": "Run statistics",
        "\u8f7d\u5165\u9a8c\u8bc1\u5931\u8d25": "Load verification failed",
        "\u8f7d\u5165\u9a8c\u8bc1\u4e2d": "Verifying load",
        "\u8f7d\u5165\u7528\u65f6": "Load time",
        "\u8f7d\u5165\u541e\u5410": "Load throughput",
        "\u8f7d\u5165\u4e2d": "Loading",
        "\u8fd0\u884c\u7528\u65f6": "Run time",
        "\u8fd0\u884c\u541e\u5410": "Run throughput",
        "\u8fd0\u884c\u4e2d": "Running",
        "\u5e73\u5747\u5ef6\u8fdf": "Mean latency (legacy accumulator)",
        "\u5c3e\u5ef6\u8fdf": "Tail latency",
        "\u9a8c\u8bc1\u7528\u65f6": "Verification time",
        "\u9a8c\u8bc1\u901a\u8fc7": "Verification passed",
        "\u7528\u65f6": "time",
    }
    for old, new in replacements.items():
        output = output.replace(old, new)
    return output.encode("ascii", errors="backslashreplace").decode("ascii")


def versions(binary, repository):
    """Fingerprint the executable, resolved runtime libraries, and source."""
    binary = binary.resolve(strict=True)
    linked = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, check=True)
    if "not found" in linked.stdout:
        raise RuntimeError("An executable dependency could not be resolved")
    paths = {binary, Path(__file__).resolve()}
    for line in linked.stdout.splitlines():
        for name in re.findall(r"(?:=>\s+)?(/[^\s]+)", line):
            path = Path(name)
            if path.is_file():
                paths.add(path.resolve())
    for directory in ("hashindex", "utils", "benchmark"):
        source = repository / directory
        if source.is_dir():
            paths.update(path.resolve() for path in source.rglob("*")
                         if path.is_file() and path.suffix in (".h", ".hpp", ".cc", ".cpp", ".txt", ".cmake"))
    for name in ("CMakeLists.txt", "dependency.cmake", "build/CMakeCache.txt"):
        if (repository / name).is_file():
            paths.add((repository / name).resolve())
    result = {str(path): digest_file(path) for path in sorted(paths)}
    result["real_trace_data"] = real_trace_support.datasets(repository)
    return result


def environment():
    names = set(ENV_KEYS)
    names.update(name for name in os.environ if name.startswith(ENV_PREFIXES))
    return {"variables": {name: os.environ.get(name) for name in sorted(names)},
            "parent_cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None}


def observed_profile_name(workload):
    if workload == "w":
        return PROFILE_NAMES[workload]
    repository = Path(__file__).resolve().parent.parent
    directory = Path(os.environ.get("TIMNAH_TRACE_DIR", str(repository / "benchmark/twitter/data")))
    metadata = json.loads((directory / (workload + ".json")).read_text())
    declared = int(metadata.get("cluster", 0))
    match = re.search(r"cluster([0-9]+)", metadata.get("source_url", ""))
    source_cluster = int(match.group(1)) if match else declared
    if source_cluster <= 0 or declared and declared != source_cluster:
        raise ValueError("Invalid or conflicting observed trace cluster identity")
    return "{} (cluster {})".format(PROFILE_NAMES[workload], source_cluster)


def configuration(args, workload, method, threads):
    cli_index, background = METHODS[method]
    command = [str(args.binary), "-index=" + cli_index, "-workload=" + workload,
               "-num_rounds=" + str(args.num_rounds), "-num_threads=" + str(threads),
               "-num_load_threads=" + str(threads), "-num_run_threads=" + str(threads),
               "-num_bg=" + str(background), "-num_load=" + str(args.num_load),
               "-num_run=" + str(args.num_run), "-rndSeed=1729",
               "-real_epoch_ops=" + str(EPOCH_OPS),
               "-real_drift_stride=" + str(max(args.num_load // 100, 1)),
               "-keySize=8", "-valueSize=8",
               "-bind_core=true", "-latency=true", "-latency_sample_rate=0.01",
               "-verify=false", "-shift=false", "-shuffle=true"]
    return dict(workload=workload, method=method, num_threads=threads,
                num_bg=background, num_rounds=args.num_rounds, round=9,
                num_load=args.num_load, num_run=args.num_run, command=command,
                profile_name=observed_profile_name(workload), trace_model="observed_trace")


def fingerprint(config, manifest, env):
    return digest_bytes(json.dumps(dict(config=config, versions=manifest, environment=env),
                                   sort_keys=True, ensure_ascii=True).encode("ascii"))


def load_cached(path, expected, num_load, expected_config=None):
    """Trust neither a successful-looking log nor an unmatched old receipt."""
    try:
        receipt = json.loads(Path(path).read_text(encoding="ascii"))
        if receipt.get("schema_version") != SCHEMA_VERSION or receipt["fingerprint"] != expected or receipt["exit_code"] != 0 or not receipt["validated"]:
            return None
        if expected_config is not None and receipt["config"] != expected_config:
            return None
        log = Path(receipt["log_path"])
        raw = Path(receipt["raw_path"])
        if digest_file(log) != receipt["log_sha256"] or digest_file(raw) != receipt["raw_sha256"]:
            return None
        config = receipt["config"]
        original = gzip.decompress(raw.read_bytes())
        if digest_bytes(original) != receipt["stdout_sha256"]:
            return None
        expected_index = METHODS[config["method"]][0] if "method" in config else None
        parsed = parse_run(original.decode("utf-8", errors="replace"), config["workload"],
                           config["num_rounds"], num_load, expected_index, config["num_run"])
        if parsed != receipt["rounds"] or parsed[9]["throughput_Mops"] != receipt["throughput_Mops"]:
            return None
        return receipt
    except (OSError, ValueError, KeyError, TypeError, EOFError, zlib.error):
        return None


def active_benchmarks():
    if not Path("/proc").is_dir():
        return []
    found = []
    for path in Path("/proc").glob("[0-9]*/cmdline"):
        try:
            command = path.read_bytes().split(b"\0")[0]
            if command and Path(os.fsdecode(command)).name.startswith("ycsb"):
                found.append(int(path.parent.name))
        except (OSError, ProcessLookupError):
            pass
    return found


def stop_process(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def execute(args, config, receipt_path, expected):
    running = active_benchmarks()
    if running:
        raise RuntimeError("Another ycsb process is active: " + ",".join(map(str, running)))
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    raw_part = receipt_path.with_suffix(".stdout.part")
    log_path = receipt_path.with_suffix(".log")
    raw_path = receipt_path.with_suffix(".stdout.gz")
    started = utc_now()
    timer = time.monotonic()
    process = None
    interrupted = False
    try:
        with raw_part.open("wb") as output:
            process = subprocess.Popen(config["command"], cwd=args.binary.parent,
                                       stdout=output, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            last_heartbeat = timer
            while process.poll() is None:
                elapsed = time.monotonic() - timer
                if args.timeout and elapsed > args.timeout:
                    stop_process(process)
                    raise RuntimeError("Benchmark exceeded its configured timeout")
                if time.monotonic() - last_heartbeat >= 60:
                    print("Still running: workload={} index={} threads={} elapsed={:.0f}s".format(
                        config["workload"].upper(), config["method"], config["num_threads"], elapsed), flush=True)
                    last_heartbeat = time.monotonic()
                time.sleep(0.5)
    except BaseException:
        interrupted = True
        if process is not None:
            stop_process(process)
        raise
    finally:
        if raw_part.exists():
            raw = raw_part.read_bytes()
            atomic_bytes(raw_path, gzip.compress(raw, mtime=0))
            atomic_bytes(log_path, normalize_output(raw.decode("utf-8", errors="replace")).encode("ascii"))
            raw_part.unlink()
    if interrupted or process.returncode:
        raise RuntimeError("Benchmark failed with exit code {}. See {}".format(process.returncode, log_path))
    parsed = parse_run(raw.decode("utf-8", errors="replace"), config["workload"],
                       config["num_rounds"], args.num_load, METHODS[config["method"]][0], config["num_run"])
    receipt = dict(schema_version=SCHEMA_VERSION, fingerprint=expected, config=config,
                   started_utc=started, finished_utc=utc_now(), seconds=time.monotonic()-timer,
                   exit_code=process.returncode, validated=True, rounds=parsed,
                   throughput_Mops=parsed[9]["throughput_Mops"],
                   log_path=str(log_path), log_sha256=digest_file(log_path),
                   raw_path=str(raw_path), raw_sha256=digest_file(raw_path),
                   stdout_sha256=digest_bytes(raw))
    return receipt


def publish(args, accepted):
    """Write measured points only. Incomplete files cannot pass plot validation."""
    for workload in args.workloads:
        data = {method: [] for method in args.indices}
        for method in args.indices:
            for threads in args.threads:
                receipt = accepted.get((workload, method, threads))
                if receipt is None:
                    continue
                config = receipt["config"]
                data[method].append(dict(num_threads=threads,
                    throughput_Mops=receipt["throughput_Mops"], round=9,
                    num_rounds=args.num_rounds, num_bg=config["num_bg"], workload=workload,
                    receipt=str(Path(receipt["log_path"]).with_suffix(".json").relative_to(args.output_dir)),
                    fingerprint=receipt["fingerprint"]))
        atomic_json(args.output_dir / WORKLOAD_FILES[workload], data)


def parser():
    root = Path(__file__).resolve().parent
    value = EnglishArgumentParser(description=__doc__)
    value.add_argument("--binary", type=Path, default=root.parent / "build/benchmark/ycsb/ycsb")
    value.add_argument("--output-dir", type=Path, default=root)
    value.add_argument("--workloads", nargs="+", choices=WORKLOADS, default=list(WORKLOADS))
    value.add_argument("--threads", nargs="+", type=int, default=list(THREAD_COUNTS))
    value.add_argument("--indices", nargs="+", choices=METHODS, default=list(METHODS))
    value.add_argument("--num-rounds", type=int, default=10)
    value.add_argument("--num-load", type=int, default=100000000)
    value.add_argument("--num-run", type=int, default=100000000)
    value.add_argument("--timeout", type=float, default=0, help="Per-process timeout in seconds; 0 disables it")
    value.add_argument("--force", action="store_true", help="Rerun even validated cached points")
    value.add_argument("--dry-run", action="store_true", help="Print the experiment plan without running ycsb")
    value.add_argument("--validate-only", action="store_true", help="Check cache coverage without running ycsb")
    return value


def main(argv=None):
    args = parser().parse_args(argv)
    if (args.num_rounds < 10 or args.num_load <= 0 or args.num_run <= 0
            or not math.isfinite(args.timeout) or args.timeout < 0):
        raise ValueError("Use at least 10 rounds and positive record/operation counts")
    if not args.threads or any(t not in THREAD_COUNTS for t in args.threads) or len(set(args.threads)) != len(args.threads):
        raise ValueError("Threads must be unique values from the Figure 10 grid")
    if len(set(args.workloads)) != len(args.workloads) or len(set(args.indices)) != len(args.indices):
        raise ValueError("Workloads and indices must be unique")
    args.binary = args.binary.resolve(strict=True)
    args.output_dir = args.output_dir.resolve()
    plan = [configuration(args, w, m, t) for w in args.workloads for m in args.indices for t in args.threads]
    if args.dry_run:
        for position, config in enumerate(plan, 1):
            print(english_text("[{}/{}] {}".format(position, len(plan), " ".join(config["command"]))))
        return 0
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / ".collection.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("Another figure collector is using this output directory")
        repository = Path(__file__).resolve().parent.parent
        manifest = versions(args.binary, repository)
        env = environment()
        session = dict(schema_version=SCHEMA_VERSION, started_utc=utc_now(), versions=manifest,
                       environment=env, plan=plan,
                       note="Observed Twitter cluster identities recorded in dataset manifests and FIU Webmail records, adapted to 8-byte index reads and overwrites. Manifests identify source scope. The preload includes disjoint background keys; finite-trace wrapping is explicit. Preparation and preload are excluded from throughput.")
        collection_settings = dict(num_rounds=args.num_rounds, num_load=args.num_load,
                                   num_run=args.num_run)
        session["settings"] = collection_settings
        session_key = digest_bytes(json.dumps(dict(versions=manifest, environment=env,
                                                  settings=collection_settings),
                                              sort_keys=True).encode("ascii"))
        session_dir = args.output_dir / "runs-figure10" / session_key[:16]
        accepted = {}
        missing = []
        for config in plan:
            method_slug = "EHCache" if config["method"] == "EH+cache" else config["method"]
            receipt_path = session_dir / ("{}-{}-t{}.json".format(config["workload"], method_slug, config["num_threads"]))
            expected = fingerprint(config, manifest, env)
            receipt = None if args.force else load_cached(receipt_path, expected, args.num_load, config)
            key = (config["workload"], config["method"], config["num_threads"])
            if receipt is None:
                missing.append((config, receipt_path, expected))
            else:
                accepted[key] = receipt
        print("Validated cached points: {}/{}. Missing points: {}.".format(len(accepted), len(plan), len(missing)), flush=True)
        if versions(args.binary, repository) != manifest or environment() != env:
            raise RuntimeError("The measured version or environment changed while validating the cache")
        if args.validate_only:
            return 0 if not missing else 2
        atomic_json(session_dir / "session.json", session)
        publish(args, accepted)
        for config, receipt_path, expected in missing:
            if versions(args.binary, repository) != manifest or environment() != env:
                raise RuntimeError("The executable, source, dependencies, or environment changed; stopping collection")
            print("[{}/{}] Running workload={} index={} threads={} rounds={} background={}".format(
                len(accepted)+1, len(plan), config["workload"].upper(), config["method"],
                config["num_threads"], args.num_rounds, config["num_bg"]), flush=True)
            receipt = execute(args, config, receipt_path, expected)
            if versions(args.binary, repository) != manifest or environment() != env:
                raise RuntimeError("The measured version changed during the benchmark; result rejected")
            atomic_json(receipt_path, receipt)
            key = (config["workload"], config["method"], config["num_threads"])
            accepted[key] = receipt
            publish(args, accepted)
            print("Measured round 9: {:.3f} Mops/s. Completed {}/{}.".format(
                receipt["throughput_Mops"], len(accepted), len(plan)), flush=True)
        if versions(args.binary, repository) != manifest or environment() != env:
            raise RuntimeError("The measured version or environment changed before collection completed")
        session.update(finished_utc=utc_now(), completed=len(accepted), expected=len(plan))
        atomic_json(session_dir / "session.json", session)
        print(english_text("Collection complete. Saved workload JSON files in {}.".format(args.output_dir)), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("Collection interrupted. Completed measurements remain available for resume.", file=sys.stderr)
        sys.exit(130)
    except Exception as error:
        message = english_text(error)
        print("Error: " + message, file=sys.stderr)
        sys.exit(1)
