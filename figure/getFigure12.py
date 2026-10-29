#!/usr/bin/env python3
"""Collect real lifecycle throughput using unchanged production index libraries."""

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
METRIC = "completed_operations/elapsed_seconds"
DATA_FILENAME = "figure12-lifecycle-throughput.json"
METHODS = {"Timnah": 1, "Dash": 0, "CHash": 0}
WORKLOAD = "c"
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
    return {name: digest_file(support / name)
            for name in ("prepare.py", "driver.cpp", "CMakeLists.txt")}


def source_copy_valid(source, repository):
    try:
        manifest = json.loads((source / ".figure12-diagnostic-copy.json").read_text())
        if (manifest["schema"] != "figure12-diagnostic-source-v1" or
                manifest["repository"] != str(repository) or
                manifest["source_dir"] != str(source)):
            return False
        for name, value in manifest["production_sources_sha256"].items():
            if digest_file(repository / name) != value:
                return False
        for name, value in manifest["diagnostic_sources_sha256"].items():
            if digest_file(source / name) != value:
                return False
        return True
    except (OSError, ValueError, KeyError, TypeError):
        return False


def build_diagnostic(args):
    source, build, support = args.source_dir, args.build_dir, args.support_dir
    if not source_copy_valid(source, args.repository):
        if args.validate_only:
            return None
        sys.dont_write_bytecode = True
        spec = importlib.util.spec_from_file_location("figure12_prepare", support / "prepare.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.prepare(args.repository, source)
    inputs = {"source_manifest": digest_file(source / ".figure12-diagnostic-copy.json"),
              "support": support_versions(support), "build_type": "Release",
              "source": str(source), "support_dir": str(support)}
    binary = build / "figure12_probe"
    cache = build / "figure12-build-manifest.json"
    try:
        old = json.loads(cache.read_text())
        if old["inputs"] == inputs and digest_file(binary) == old["binary_sha256"]:
            if all(digest_file(build / name) == value
                   for name, value in old["build_files_sha256"].items()) and all(
                    digest_file(name) == value for name, value in old["linked_libraries_sha256"].items()):
                return old
    except (OSError, ValueError, KeyError, TypeError):
        pass
    if args.validate_only:
        return None
    build.mkdir(parents=True, exist_ok=True)
    commands = [
        ["cmake", "-S", str(support), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
         "-DFIGURE12_REPOSITORY=" + str(args.repository),
         "-DFIGURE12_SOURCE_ROOT=" + str(source)],
        ["cmake", "--build", str(build), "--target", "figure12_probe", "-j", str(args.jobs)],
    ]
    for number, command in enumerate(commands):
        result = subprocess.run(command, capture_output=True)
        path = build / ("configure.log" if number == 0 else "compile.log")
        atomic_bytes(path, english_text((result.stdout + result.stderr).decode(
            "utf-8", "replace")).encode("ascii"))
        if result.returncode:
            raise RuntimeError("Isolated lifecycle build failed; see " + str(path))
    if not source_copy_valid(source, args.repository):
        raise RuntimeError("Source files changed during the isolated build")
    linkage = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, check=True)
    if "not found" in linkage.stdout:
        raise RuntimeError("A lifecycle runtime dependency is unresolved")
    linked = {str(Path(name).resolve()) for name in re.findall(r"(/[^\s]+)", linkage.stdout)
              if Path(name).is_file()}
    for relative in ("build/hashindex/libhashindex.so", "build/utils/libutils.so",
                     "build/hashindex/timnah/libtimnah.so", "build/hashindex/dash/libdash.so",
                     "build/hashindex/chash/libchash.so"):
        if str((args.repository / relative).resolve()) not in linked:
            raise RuntimeError("The lifecycle driver must use the existing production library: " + relative)
    manifest = {"inputs": inputs, "binary": str(binary), "binary_sha256": digest_file(binary),
                "created_utc": now(), "linked_libraries_sha256":
                {name: digest_file(name) for name in sorted(linked)},
                "build_files_sha256": {str(path.relative_to(build)): digest_file(path)
                    for path in build.rglob("*") if path.is_file() and
                    path.name in ("flags.make", "link.txt", "CMakeCache.txt")}}
    atomic_json(cache, manifest)
    return manifest


def parse_run(output, config):
    def one(prefix):
        rows = re.findall(r"^" + re.escape(prefix) + r" (.+)$", output, re.M)
        if len(rows) != 1:
            raise ValueError("Expected exactly one " + prefix + " line")
        return fields(rows[0])
    header = one("Figure12Config")
    expected = {"index": config["index"], "workload": "c", "num_threads": "30",
                "num_load_threads": "30", "num_run_threads": "30", "lifecycle_rounds": "12",
                "num_bg": str(config["num_bg"]), "num_rounds": "10", "stages_per_round": "10",
                "num_load": str(config["num_load"]), "num_run": str(config["num_run"]),
                "metric": METRIC, "latency": "true", "latency_sample_rate": "0.01",
                "cpu_node": "0", "key_bytes": "8", "value_bytes": "8",
                "index_implementation": "unchanged_production_shared_libraries"}
    if any(header.get(name) != target for name, target in expected.items()):
        raise ValueError("Lifecycle settings differ from the requested configuration")
    markers = re.findall(r"^YCSB workload=c (.+)$", output, re.M)
    if len(markers) != 1:
        raise ValueError("The YCSB C configuration is missing")
    ratio = fields(markers[0])
    if any(float(ratio[name]) != value for name, value in
           (("read", 1), ("update", 0), ("insert", 0), ("remove", 0), ("negative", 0))):
        raise ValueError("The workload is not read-only YCSB C")
    records = []
    integer_fields = ("stage", "lifecycle_round", "phase_round", "stage_in_round",
                      "operations", "successful_operations", "failed_operations", "elapsed_ns")
    for line in re.findall(r"^Figure12Stage (.+)$", output, re.M):
        values = fields(line)
        row = {key: int(values[key]) for key in integer_fields}
        row.update(phase=values["phase"], elapsed_seconds=float(values["elapsed_seconds"]),
                   throughput_Mops=float(values["throughput_Mops"]))
        records.append(row)
    if len(records) != 120:
        raise ValueError("Each index requires exactly 120 measured lifecycle stages")
    for stage, row in enumerate(records, 1):
        logical = (stage - 1) // 10
        phase = "bulk_loading" if logical == 0 else "mass_deletion" if logical == 11 else "executing"
        count = (config["num_run"] if phase == "executing" else config["num_load"]) // 10
        if (row["stage"] != stage or row["phase"] != phase or row["lifecycle_round"] != logical or
                row["phase_round"] != (logical - 1 if phase == "executing" else 0) or
                row["stage_in_round"] != (stage - 1) % 10 + 1 or row["operations"] != count or
                row["successful_operations"] != count or row["failed_operations"] != 0):
            raise ValueError("An operation failed or the Stage coordinates/counters are inconsistent")
        elapsed, rate = row["elapsed_seconds"], row["throughput_Mops"]
        if (not math.isfinite(elapsed) or elapsed <= 0 or not math.isfinite(rate) or rate <= 0 or
                row["elapsed_ns"] <= 0 or abs(elapsed - row["elapsed_ns"] / 1e9) > 1e-12 or
                not math.isclose(rate, count / elapsed / 1e6, rel_tol=1e-8, abs_tol=5e-7)):
            raise ValueError("Lifecycle throughput does not agree with successful operations and elapsed time")
    # Phase statistics must follow exactly ten measured stages, once per true round.
    timeline = re.findall(r"^(Figure12Stage|Figure12Phase) (.+)$", output, re.M)
    if len(timeline) != 132:
        raise ValueError("Expected 120 Stage and 12 real-round Phase records")
    phase_records = []
    for logical in range(12):
        block = timeline[logical * 11:(logical + 1) * 11]
        if [item[0] for item in block] != ["Figure12Stage"] * 10 + ["Figure12Phase"]:
            raise ValueError("Native statistics were not called only at real-round boundaries")
        row = fields(block[-1][1])
        phase = "bulk_loading" if logical == 0 else "mass_deletion" if logical == 11 else "executing"
        count = config["num_run"] if phase == "executing" else config["num_load"]
        size = 0 if logical == 11 else config["num_load"]
        targets = {"phase": phase, "lifecycle_round": str(logical),
                   "phase_round": str(logical - 1 if phase == "executing" else 0),
                   "attempted_operations": str(count), "successful_operations": str(count),
                   "failed_operations": "0", "size": str(size), "expected_size": str(size),
                   "cardinality_verified": "true"}
        if any(row.get(name) != value for name, value in targets.items()):
            raise ValueError("Native phase cardinality or aggregate operation counts are inconsistent")
        phase_records.append(row)
    native = re.findall(r"^Figure12NativeStatistics phase=(\S+) phase_round=(\d+) (.+)$", output, re.M)
    if len(native) != 12:
        raise ValueError("Exactly one native statistics record per real phase is required")
    for row, (phase, phase_round, statistics) in zip(phase_records, native):
        if (phase != row["phase"] or phase_round != row["phase_round"] or
                statistics.split()[0] != config["index"] or
                fields(statistics).get("size") != row["size"]):
            raise ValueError("The native index identity, phase or size differs")
    verification = one("Figure12Verification")
    if any(verification.get(name) != value for name, value in
           {"checked_keys": str(config["num_load"]), "remaining_keys": "0",
            "expected_remaining_keys": "0", "outside_timing": "true"}.items()):
        raise ValueError("Not all original preload keys were verified absent after deletion")
    complete = one("Figure12Complete")
    total = config["num_load"] * 2 + config["num_run"] * 10
    targets = {"stages": "120", "operations": str(total), "metric": METRIC,
               "final_size": "0", "verified_absent": str(config["num_load"]),
               "lifecycle_rounds": "12", "search_rounds": "10", "remaining_keys": "0",
               "loaded_keys": str(config["num_load"]), "deleted_keys": str(config["num_load"]),
               "searched_keys": str(config["num_run"] * 10),
               "successful_operations": str(total), "failed_operations": "0"}
    if any(complete.get(name) != value for name, value in targets.items()):
        raise ValueError("The lifecycle is incomplete or full mass deletion was not verified")
    return records, {"phases": phase_records, "statistics": [list(row) for row in native],
                     "verification": verification, "complete": complete}


def configuration(args, index):
    binary = args.build_dir / "figure12_probe"
    command = [str(binary), "-index=" + index, "-workload=c", "-num_rounds=10",
               "-num_threads=30", "-num_load_threads=30", "-num_run_threads=30",
               "-num_bg=" + str(METHODS[index]), "-num_load=" + str(args.num_load),
               "-num_run=" + str(args.num_run), "-rndSeed=1729", "-latency=true",
               "-latency_sample_rate=0.01", "-bind_core=true", "-verify=false",
               "-shift=false", "-shuffle=true", "-keySize=8", "-valueSize=8"]
    return {"index": index, "workload": "c", "num_bg": METHODS[index], "num_threads": 30,
            "num_rounds": 10, "num_load": args.num_load, "num_run": args.num_run,
            "command": command}


def cached(path, expected, config):
    try:
        record = json.loads(path.read_text())
        if record["fingerprint"] != expected or record["config"] != config or record["exit_code"] != 0:
            return None
        raw = gzip.decompress(Path(record["stdout_gzip"]).read_bytes())
        log = Path(record["log"]).read_bytes()
        if digest_bytes(raw) != record["stdout_sha256"] or digest_bytes(log) != record["log_sha256"]:
            return None
        parsed, native = parse_run(raw.decode("utf-8", "replace"), config)
        if parsed != record["stages"] or native != record["native"]:
            return None
        return record
    except (OSError, ValueError, KeyError, TypeError, EOFError, zlib.error):
        return None


def execute(args, config, path, expected):
    # Raw native diagnostics are retained byte-for-byte; visible logs are ASCII.
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
                        raise TimeoutError("The lifecycle benchmark exceeded the requested timeout")
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
        raise RuntimeError("Lifecycle benchmark exited with {}. See {}".format(code, log))
    stages, native = parse_run(raw.decode("utf-8", "replace"), config)
    return {"fingerprint": expected, "config": config, "exit_code": code,
            "started_utc": started, "finished_utc": now(), "elapsed_seconds": time.monotonic()-begin,
            "stdout_gzip": str(compressed), "stdout_sha256": digest_bytes(raw), "log": str(log),
            "log_sha256": digest_file(log), "stages": stages, "native": native}


def assert_idle():
    for path in Path("/proc").glob("[0-9]*/comm"):
        try:
            if path.read_text().strip() in ("ycsb", "ycsb.before", "figure11_probe", "figure12_probe"):
                if (path.parent / "stat").read_text().split(") ", 1)[1][0] != "Z":
                    raise RuntimeError("Another benchmark is active: " + path.parent.name)
        except (OSError, PermissionError):
            pass


def publish(args, accepted, key):
    output = {"schema_version": SCHEMA_VERSION, "metric": METRIC, "workload": "c",
              "num_threads": 30, "num_rounds": 10, "stages_per_round": 10,
              "num_load": args.num_load, "num_run": args.num_run,
              "complete": set(accepted) == set(METHODS), "version_fingerprint": key,
              "updated_utc": now(), "indices": {name: row["stages"] for name, row in accepted.items()},
              "runs": accepted,
              "note": "Actual right-endpoint stages 1-120: load 1-10, execute 11-110, delete 111-120. "
                      "The same index instance is used throughout. Each Stage rate is successful operations "
                      "divided by its elapsed time. Production index libraries are unchanged. "
                      "Trace generation, log output, native round statistics and final absence validation "
                      "are outside the Stage timers. Search latency sampling remains enabled at 0.01."}
    atomic_json(args.output_dir / DATA_FILENAME, output)


def parser():
    folder = Path(__file__).resolve().parent
    p = EnglishArgumentParser(description=__doc__)
    p.add_argument("--repository", type=Path, default=folder.parent)
    p.add_argument("--output-dir", type=Path, default=folder)
    p.add_argument("--support-dir", type=Path, default=folder / "figure12_support")
    p.add_argument("--source-dir", type=Path, default=folder / ".figure12-source")
    p.add_argument("--build-dir", type=Path, default=folder / ".figure12-build")
    p.add_argument("--indices", nargs="+", choices=METHODS, default=list(METHODS))
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
    if len(args.indices) != len(set(args.indices)):
        raise ValueError("Indices must be unique")
    for name in ("repository", "output_dir", "support_dir", "source_dir", "build_dir"):
        setattr(args, name, getattr(args, name).resolve())
    for name in ("source_dir", "build_dir"):
        path = getattr(args, name)
        if path == args.repository or args.repository.is_relative_to(path):
            raise ValueError("Diagnostic directories cannot replace the repository")
        if any(path.is_relative_to(args.repository / item) for item in ("benchmark", "hashindex", "utils", "build")):
            raise ValueError("Diagnostic directories must be outside production directories")
    plan = [configuration(args, name) for name in args.indices]
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
        build = build_diagnostic(args)
        if build is None:
            print("No validated lifecycle diagnostic build is cached.")
            return 2
        if production_versions(args.repository) != original:
            raise RuntimeError("Production files changed during the isolated build")
        for name, sha in build["linked_libraries_sha256"].items():
            if digest_file(name) != sha:
                raise RuntimeError("A lifecycle runtime dependency changed; rebuild the diagnostic executable")
        versions = {"production": original, "build": build,
                    "collector_sha256": digest_file(Path(__file__).resolve())}
        key = fingerprint({"versions": versions, "environment": env,
                           "num_load": args.num_load, "num_run": args.num_run})
        session = args.output_dir / "runs-figure12" / key[:16]
        session.mkdir(parents=True, exist_ok=True)
        accepted, missing = {}, []
        for config in plan:
            path = session / (config["index"] + ".json")
            expected = fingerprint({"session": key, "config": config})
            record = None if args.force else cached(path, expected, config)
            if record is None:
                missing.append((config, path, expected))
            else:
                accepted[config["index"]] = record
        print("Validated cached lifecycle runs: {}/{}. Missing: {}.".format(len(accepted), len(plan), len(missing)), flush=True)
        if args.validate_only:
            return 0 if not missing else 2
        metadata = {"schema_version": 1, "status": "running", "started_utc": now(),
                    "versions": versions, "environment": env, "plan": plan,
                    "completed": len(accepted), "expected": len(plan)}
        atomic_json(session / "session.json", metadata)
        publish(args, accepted, key)
        for config, path, expected in missing:
            assert_idle()
            if (production_versions(args.repository) != original or environment() != env or
                    digest_file(args.build_dir / "figure12_probe") != build["binary_sha256"]):
                raise RuntimeError("Measured source, binary, library or environment changed")
            print("[{}/{}] Running {}: 30 threads, 10 YCSB C rounds, bulk loading and mass deletion.".format(
                len(accepted)+1, len(plan), config["index"]), flush=True)
            record = execute(args, config, path, expected)
            if production_versions(args.repository) != original or environment() != env:
                raise RuntimeError("Production version or environment changed during collection")
            atomic_json(path, record)
            accepted[config["index"]] = record
            publish(args, accepted, key)
            metadata["completed"] = len(accepted)
            atomic_json(session / "session.json", metadata)
            print("Validated {}: 120 stages; all inserted keys were deleted and verified absent.".format(config["index"]), flush=True)
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


