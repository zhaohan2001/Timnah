#!/usr/bin/env python3
"""Collect real Stage-level DRAM Search hit rates with an isolated build."""

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
METRIC = "search_dram_hits/search_attempts"
DATA_FILENAME = "figure11-dram-hit-rates.json"
METHODS = {"Timnah": 1, "Dash": 0, "CHash": 0, "EH+cache": 0}
WORKLOADS = {"c": (1.0, 0.0), "a": (0.5, 0.5),
             "ts": (0.99, 0.01), "w": (0.18, 0.82)}
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


def source_copy_valid(source, repository, support):
    try:
        manifest = json.loads((source / ".figure11-diagnostic-copy.json").read_text())
        if (manifest["schema"] != "figure11-diagnostic-source-v1" or
                manifest["repository"] != str(repository) or
                manifest["source_dir"] != str(source) or
                manifest["observer_sha256"] != digest_file(support / "observer.hpp")):
            return False
        for relative, expected in manifest["production_sources_sha256"].items():
            if digest_file(repository / relative) != expected:
                return False
        for relative, expected in manifest["diagnostic_sources_sha256"].items():
            if digest_file(source / relative) != expected:
                return False
        return True
    except (OSError, ValueError, KeyError, TypeError):
        return False


def support_versions(support):
    return {name: digest_file(support / name)
            for name in ("prepare.py", "observer.hpp", "driver.cpp", "CMakeLists.txt")}


def build_diagnostic(args):
    source, build, support = args.source_dir, args.build_dir, args.support_dir
    if not source_copy_valid(source, args.repository, support):
        if args.validate_only:
            return None
        sys.dont_write_bytecode = True
        spec = importlib.util.spec_from_file_location("figure11_prepare", support / "prepare.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.prepare(args.repository, source)
    inputs = {"source_manifest": digest_file(source / ".figure11-diagnostic-copy.json"),
              "support": support_versions(support),
              "build_type": "Release", "source": str(source), "support_dir": str(support)}
    binary = build / "figure11_probe"
    cache = build / "figure11-build-manifest.json"
    try:
        previous = json.loads(cache.read_text())
        if previous["inputs"] == inputs and digest_file(binary) == previous["binary_sha256"]:
            return previous
    except (OSError, ValueError, KeyError, TypeError):
        pass
    if args.validate_only:
        return None
    build.mkdir(parents=True, exist_ok=True)
    commands = [
        ["cmake", "-S", str(support), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
         "-DFIGURE11_REPOSITORY=" + str(args.repository),
         "-DFIGURE11_SOURCE_ROOT=" + str(source)],
        ["cmake", "--build", str(build), "--target", "figure11_probe", "-j", str(args.jobs)],
    ]
    for position, command in enumerate(commands):
        completed = subprocess.run(command, capture_output=True)
        log = build / ("configure.log" if position == 0 else "compile.log")
        atomic_bytes(log, english_text((completed.stdout + completed.stderr).decode(
            "utf-8", "replace")).encode("ascii"))
        if completed.returncode:
            raise RuntimeError("Isolated diagnostic build failed; see " + str(log))
    if not source_copy_valid(source, args.repository, support):
        raise RuntimeError("Sources changed while building the isolated diagnostic executable")
    manifest = {"inputs": inputs, "binary": str(binary),
                "binary_sha256": digest_file(binary), "created_utc": now()}
    atomic_json(cache, manifest)
    return manifest


def parse_run(output, config):
    """Reject incomplete, duplicate, inconsistent or misconfigured stages."""
    replay = None
    if config['workload'] in ('ts','w'):
        replay = real_trace_support.validate(output,config['workload'],10,config['num_run'],config['num_load'])
        config = dict(config, num_load=replay['preload_keys'])
    def one(prefix):
        lines = re.findall(r"^" + re.escape(prefix) + r" (.+)$", output, re.MULTILINE)
        if len(lines) != 1:
            raise ValueError("Expected exactly one " + prefix + " line")
        return fields(lines[0])

    header = one("Figure11Config")
    targets = {"index": config["index"], "workload": config["workload"],
               "num_threads": "30", "num_bg": str(config["num_bg"]),
               "num_rounds": "10", "stages_per_round": "10",
               "num_load": str(config["num_load"]), "num_run": str(config["num_run"]),
               "metric": METRIC, "diagnostic_only": "true"}
    if any(header.get(name) != target for name, target in targets.items()):
        raise ValueError("The diagnostic configuration does not match the requested run")
    load = one("Figure11Load")
    if int(load["records"]) != config["num_load"] or int(load["failed_inserts"]):
        raise ValueError("The preload is incomplete")
    load_line = re.findall(r"^Figure11Load .+statistics=(.+)$", output, re.MULTILINE)[0]
    identity = "EHCache" if config["index"] == "EH+cache" else config["index"]
    if load_line.split()[0] != identity:
        raise ValueError("The native statistics report a different index")
    operation_header = re.findall(r"^(?:YCSB|Real) workload=(\S+) (.+)$", output, re.MULTILINE)
    if len(operation_header) != 1 or operation_header[0][0] != config["workload"]:
        raise ValueError("The workload header is missing or inconsistent")
    ratios = fields(operation_header[0][1])
    if replay is None:
        read, update = WORKLOADS[config['workload']]
        if any(float(ratios[name]) != expected for name,expected in [('read',read),('update',update),('insert',0),('remove',0),('negative',0)]):
            raise ValueError('Unexpected YCSB operation ratios')
    markers = re.findall(r"^Round (\d+)\s*$", output, re.MULTILINE)
    if list(map(int, markers)) != list(range(10)):
        raise ValueError("Every actual round must occur exactly once")
    stage_lines = re.findall(r"^Figure11Stage (.+)$", output, re.MULTILINE)
    if len(stage_lines) != 100:
        raise ValueError("Each run requires exactly 100 measured stages")
    stages = []
    integer_names = ("stage", "round", "stage_in_round", "operations", "search_attempts",
                     "dram_hits", "slow_hits", "read_misses", "update_attempts", "update_failures")
    for number, line in enumerate(stage_lines, 1):
        item = fields(line)
        record = {name: int(item[name]) for name in integer_names}
        record["dram_hit_rate"] = float(item["dram_hit_rate"])
        if (record["stage"] != number or record["round"] != (number-1)//10 or
                record["stage_in_round"] != (number-1)%10+1 or
                record["operations"] != config["num_run"]//10):
            raise ValueError("A Stage has invalid coordinates or operation count")
        if (any(record[name] < 0 for name in integer_names) or
                record["search_attempts"] <= 0 or record["read_misses"] or record["update_failures"] or
                record["dram_hits"] + record["slow_hits"] + record["read_misses"] != record["search_attempts"] or
                record["search_attempts"] + record["update_attempts"] != record["operations"]):
            raise ValueError("A Stage has inconsistent Search or Update counters")
        rate = record["dram_hit_rate"]
        if (not math.isfinite(rate) or not 0 <= rate <= 1 or
                not math.isclose(rate, record["dram_hits"]/record["search_attempts"],
                                 rel_tol=1e-12, abs_tol=1e-12)):
            raise ValueError("A Stage hit rate disagrees with its exact counters")
        stages.append(record)
    round_lines = re.findall(r"^Figure11Round round=(\d+) statistics=(.+)$", output, re.MULTILINE)
    if [int(number) for number, _ in round_lines] != list(range(10)):
        raise ValueError("Native statistics must be gathered once per actual round")
    rounds = []
    for number, statistics in round_lines:
        native = fields(statistics)
        if statistics.split()[0] != identity:
            raise ValueError("Native round statistics have the wrong index identity")
        if int(native.get("size", native.get("eh_size", -1))) != config["num_load"]:
            raise ValueError("Index cardinality changed during a read/overwrite workload")
        if "observed_rounds" in native and int(native["observed_rounds"]) > int(number)+1:
            raise ValueError("Timnah convergence was evaluated more than once per real round")
        rounds.append({"round": int(number), "statistics": native})
    if replay is not None:
        for number,trace in enumerate(replay['audits']):
            block=stages[number*10:(number+1)*10]
            if int(trace['reads'])!=sum(x['search_attempts'] for x in block) or int(trace['updates'])!=sum(x['update_attempts'] for x in block):
                raise ValueError('Observed trace counts disagree with measured stages')
    complete = one("Figure11Complete")
    if (complete.get("metric") != METRIC or int(complete["stages"]) != 100 or
            int(complete["operations"]) != config["num_run"]*10):
        raise ValueError("The diagnostic process did not finish all requested operations")
    return {"stages": stages, "rounds": rounds, "load": load, "complete": complete}


def configuration(args, workload, index):
    command = [str(args.build_dir / "figure11_probe"), "-index=" + index,
               "-workload=" + workload, "-num_rounds=10", "-num_threads=30",
               "-num_load_threads=30", "-num_run_threads=30", "-num_bg=" + str(METHODS[index]),
               "-num_load=" + str(args.num_load), "-num_run=" + str(args.num_run),
               "-rndSeed=1729", "-real_epoch_ops=" + str(EPOCH_OPS),
               "-real_drift_stride=" + str(max(args.num_load//100, 1)),
               "-keySize=8", "-valueSize=8", "-bind_core=true", "-latency=false",
               "-verify=false", "-shift=false", "-shuffle=true"]
    return {"index": index, "workload": workload, "num_bg": METHODS[index],
            "num_threads": 30, "num_rounds": 10, "stages_per_round": 10,
            "num_load": args.num_load, "num_run": args.num_run, "command": command}


def active_benchmarks():
    result = []
    for path in Path("/proc").glob("[0-9]*/cmdline"):
        try:
            executable = path.read_bytes().split(b"\0")[0]
            name = Path(os.fsdecode(executable)).name
            if name.startswith("ycsb") or name == "figure11_probe":
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


def execute(args, config, path, expected):
    running = active_benchmarks()
    if running:
        raise RuntimeError("Another benchmark is active: " + ",".join(map(str, running)))
    path.parent.mkdir(parents=True, exist_ok=True)
    raw_part = path.with_suffix(".stdout.part")
    raw_path, log_path = path.with_suffix(".stdout.gz"), path.with_suffix(".log")
    start, timer, process = now(), time.monotonic(), None
    try:
        with raw_part.open("wb") as output:
            process = subprocess.Popen(config["command"], cwd=args.repository,
                                       stdout=output, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            heartbeat = timer
            while process.poll() is None:
                current = time.monotonic()
                if args.timeout and current-timer > args.timeout:
                    raise RuntimeError("The diagnostic run exceeded its timeout")
                if current-heartbeat >= 60:
                    print("Still running: workload={} index={} elapsed={:.0f}s".format(
                        config["workload"], config["index"], current-timer), flush=True)
                    heartbeat = current
                time.sleep(0.5)
    except BaseException:
        if process is not None:
            stop_process(process)
        raise
    finally:
        if raw_part.exists():
            raw = raw_part.read_bytes()
            atomic_bytes(raw_path, gzip.compress(raw, mtime=0))
            atomic_bytes(log_path, english_text(raw.decode("utf-8", "replace")).encode("ascii"))
            raw_part.unlink()
    if process.returncode:
        raise RuntimeError("Diagnostic process exited with code {}; see {}".format(
            process.returncode, log_path))
    parsed = parse_run(raw.decode("utf-8", "replace"), config)
    return {"schema_version": SCHEMA_VERSION, "fingerprint": expected, "config": config,
            "started_utc": start, "finished_utc": now(), "seconds": time.monotonic()-timer,
            "exit_code": 0, "validated": True, **parsed,
            "raw_path": str(raw_path), "raw_sha256": digest_file(raw_path),
            "stdout_sha256": digest_bytes(raw), "log_path": str(log_path),
            "log_sha256": digest_file(log_path)}


def load_cached(path, expected, config):
    try:
        receipt = json.loads(path.read_text())
        if (receipt["schema_version"] != SCHEMA_VERSION or receipt["fingerprint"] != expected or
                receipt["config"] != config or receipt["exit_code"] or receipt["validated"] is not True):
            return None
        if (digest_file(receipt["raw_path"]) != receipt["raw_sha256"] or
                digest_file(receipt["log_path"]) != receipt["log_sha256"]):
            return None
        raw = gzip.decompress(Path(receipt["raw_path"]).read_bytes())
        if digest_bytes(raw) != receipt["stdout_sha256"]:
            return None
        parsed = parse_run(raw.decode("utf-8", "replace"), config)
        if any(parsed[name] != receipt[name] for name in parsed):
            return None
        return receipt
    except (OSError, ValueError, KeyError, TypeError, EOFError, zlib.error):
        return None


def publish(args, accepted, version_key):
    workloads = {workload: {} for workload in args.workloads}
    runs = {workload: {} for workload in args.workloads}
    for (workload, index), receipt in accepted.items():
        workloads[workload][index] = receipt["stages"]
        runs[workload][index] = receipt
    document = {"schema_version": SCHEMA_VERSION, "metric": METRIC,
                "num_rounds": 10, "stages_per_round": 10, "num_threads": 30,
                "num_load": args.num_load, "num_run": args.num_run,
                "complete": len(accepted) == 16 and set(args.workloads) == set(WORKLOADS)
                and set(args.indices) == set(METHODS),
                "version_fingerprint": version_key, "updated_utc": now(),
                "workloads": workloads, "runs": runs,
                "note": "Exact Search tier outcomes per Stage, not cumulative rates. "
                "Instrumentation is isolated; its throughput is not production performance. "
                "Timnah native Stat and convergence run once per real round."}
    atomic_json(args.output_dir / DATA_FILENAME, document)


def parser():
    figure = Path(__file__).resolve().parent
    value = EnglishArgumentParser(description=__doc__)
    value.add_argument("--repository", type=Path, default=figure.parent)
    value.add_argument("--output-dir", type=Path, default=figure)
    value.add_argument("--support-dir", type=Path, default=figure / "figure11_support")
    value.add_argument("--source-dir", type=Path, default=figure / ".figure11-source")
    value.add_argument("--build-dir", type=Path, default=figure / ".figure11-build")
    value.add_argument("--workloads", nargs="+", choices=WORKLOADS, default=list(WORKLOADS))
    value.add_argument("--indices", nargs="+", choices=METHODS, default=list(METHODS))
    value.add_argument("--num-load", type=int, default=100000000)
    value.add_argument("--num-run", type=int, default=100000000)
    value.add_argument("--jobs", type=int, default=8)
    value.add_argument("--timeout", type=float, default=0)
    value.add_argument("--force", action="store_true")
    value.add_argument("--dry-run", action="store_true")
    value.add_argument("--validate-only", action="store_true")
    return value


def main(argv=None):
    args = parser().parse_args(argv)
    if (args.num_load < 10000 or args.num_run < 10000 or args.num_run % 10 or args.jobs < 1 or
            not math.isfinite(args.timeout) or args.timeout < 0):
        raise ValueError("Counts must be >= 10000, num-run divisible by ten, jobs positive, and timeout nonnegative")
    if len(set(args.workloads)) != len(args.workloads) or len(set(args.indices)) != len(args.indices):
        raise ValueError("Workloads and indices must be unique")
    for name in ("repository", "output_dir", "support_dir", "source_dir", "build_dir"):
        setattr(args, name, getattr(args, name).resolve())
    for name in ("source_dir", "build_dir"):
        path = getattr(args, name)
        if path == args.repository or args.repository.is_relative_to(path):
            raise ValueError("Diagnostic directories cannot replace the repository")
        for protected in ("utils", "hashindex", "benchmark", "build"):
            if path.is_relative_to(args.repository / protected):
                raise ValueError("Diagnostic directories must be outside production directories")
    plan = [configuration(args, workload, index)
            for workload in args.workloads for index in args.indices]
    if args.dry_run:
        print("Isolated diagnostic build: " + english_text(args.build_dir / "figure11_probe"))
        for number, config in enumerate(plan, 1):
            print(english_text("[{}/{}] {}".format(number, len(plan), " ".join(config["command"]))))
        return 0
    args.output_dir.mkdir(parents=True, exist_ok=True)
    # Share the same lock as Figure 9/10 even if diagnostics use another output directory.
    lock_dir = Path(__file__).resolve().parent
    with (lock_dir / ".collection.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("Another figure collector is active")
        original = production_versions(args.repository)
        env = environment()
        build = build_diagnostic(args)
        if build is None:
            print("No validated diagnostic build is cached.", flush=True)
            return 2
        if production_versions(args.repository) != original:
            raise RuntimeError("Production sources or binaries changed during the isolated build")
        versions = {"real_trace_data": real_trace_support.datasets(args.repository), "production": original, "build": build,
                    "collector_sha256": digest_file(Path(__file__).resolve())}
        stable = {"versions": versions, "environment": env,
                  "num_load": args.num_load, "num_run": args.num_run}
        version_key = fingerprint(stable)
        session = args.output_dir / "runs-figure11" / version_key[:16]
        accepted, missing = {}, []
        for config in plan:
            slug = "EHCache" if config["index"] == "EH+cache" else config["index"]
            path = session / (config["workload"] + "-" + slug + ".json")
            expected = fingerprint({"session": version_key, "config": config})
            receipt = None if args.force else load_cached(path, expected, config)
            if receipt is None:
                missing.append((config, path, expected))
            else:
                accepted[(config["workload"], config["index"])] = receipt
        print("Validated cached runs: {}/{}. Missing runs: {}.".format(
            len(accepted), len(plan), len(missing)), flush=True)
        if args.validate_only:
            return 0 if not missing else 2
        metadata = {"schema_version": 1, "started_utc": now(), "status": "running",
                    "versions": versions, "environment": env, "plan": plan,
                    "completed": len(accepted), "expected": len(plan)}
        atomic_json(session / "session.json", metadata)
        publish(args, accepted, version_key)
        for config, path, expected in missing:
            if (production_versions(args.repository) != original or environment() != env or
                    digest_file(args.build_dir / "figure11_probe") != build["binary_sha256"]):
                raise RuntimeError("Production version, diagnostic executable or environment changed")
            print("[{}/{}] Running workload={} index={} threads=30 rounds=10 background={}".format(
                len(accepted)+1, len(plan), config["workload"], config["index"], config["num_bg"]), flush=True)
            receipt = execute(args, config, path, expected)
            if (production_versions(args.repository) != original or environment() != env or
                    digest_file(args.build_dir / "figure11_probe") != build["binary_sha256"]):
                raise RuntimeError("A measured version changed during collection; result rejected")
            atomic_json(path, receipt)
            accepted[(config["workload"], config["index"])] = receipt
            publish(args, accepted, version_key)
            metadata["completed"] = len(accepted)
            atomic_json(session / "session.json", metadata)
            print("Validated 100 measured stages. Stage 100 DRAM Search hit rate: {:.6f}.".format(
                receipt["stages"][-1]["dram_hit_rate"]), flush=True)
        metadata.update(status="complete", finished_utc=now())
        atomic_json(session / "session.json", metadata)
        print("Collection complete. Saved " + english_text(args.output_dir / DATA_FILENAME), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("Collection interrupted. Validated measurements remain available for resume.", file=sys.stderr)
        sys.exit(130)
    except Exception as error:
        print("Error: " + english_text(error), file=sys.stderr)
        sys.exit(1)
