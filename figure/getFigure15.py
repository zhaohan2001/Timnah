#!/usr/bin/env python3
"""Collect measured cross-tier multiplier sweeps using the ordinary Timnah benchmark."""

import real_trace_support
import argparse
from datetime import datetime, timezone
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


SCHEMA_VERSION = 1
METRIC = "round9_throughput"
DATA_FILENAME = "figure15-cross-tier-multiplier.json"
K_VALUES = (1.0, 1.1, 1.3, 1.5, 1.7, 2.0, 2.2, 2.5, 3.0, 4.0, 5.0, 8.0, 10.0, 15.0)
SERIES = {
    "ycsb_c_099": {"workload": "c", "skewness": 0.99, "label": "YCSB-C (theta=0.99)"},
    "ycsb_c_05": {"workload": "c", "skewness": 0.5, "label": "YCSB-C (theta=0.5)"},
    "twitter_storage": {"workload": "ts", "skewness": None, "label": "Twitter-Storage"},
}
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


def production_versions(repository, binary=None):
    """Fingerprint original sources, build configuration, binary and libraries."""
    ordinary = repository / "build/benchmark/ycsb/ycsb"
    if not ordinary.is_file():
        raise RuntimeError("The ordinary production ycsb executable is missing")
    selected = Path(binary) if binary is not None else ordinary
    paths = {ordinary.resolve(), selected.resolve(strict=True)}
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
    linked = subprocess.run(["ldd", str(selected)], capture_output=True,
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


def require(condition, message):
    if not condition:
        raise ValueError(message)


def require_fields(row, expected, location):
    for name, value in expected.items():
        require(row.get(name) == str(value), location + ": incorrect " + name)


def parse_run(raw, config):
    """Validate every round and retain the native maintenance evidence."""
    require(config["series_id"] in SERIES, "Unsupported series")
    design = SERIES[config["series_id"]]
    require(config["workload"] == design["workload"] and config["skewness"] == design["skewness"],
            "Series configuration does not match the requested workload")
    require(config["k"] in K_VALUES, "Unsupported cross-tier multiplier")
    require(config["num_threads"] == 30 and config["num_rounds"] == 10 and config["num_bg"] == 1,
            "The sweep requires 30 threads, ten rounds, and one background worker")
    is_storage = design["workload"] == "ts"
    replay = None
    if is_storage:
        replay=real_trace_support.validate(raw,'ts',10,config['num_run'],config['num_load'])
        config=dict(config,num_load=replay['preload_keys'])
    else:
        headers=re.findall(r'^(?:YCSB|Real) workload=.+$',raw,re.M)
        require(headers==['YCSB workload=c read=1 update=0 insert=0 remove=0 negative=0 distribution=zipfian'],'Unexpected YCSB header')
    memory_lines = re.findall(r"^Timnah memory_initial (.+)$", raw, re.M)
    require(len(memory_lines) == 1, "Exactly one initial memory record is required")
    initial = fields(memory_lines[0])
    require_fields(initial, {"fast_node": 0, "slow_node": 2, "fast_bucket_bytes": 256 << 20,
        "fast_counter_bytes": 64 << 20, "slow_bucket_bytes": 1024 << 20,
        "slow_counter_bytes": 256 << 20, "sample_rate": "0.1", "decay_ms": 1000,
        "decay_factor": "0.5", "convergence_threshold": "0" if is_storage else "0.002",
        "queue_capacity": 1024, "huge_page_advice": 1}, "Initial Timnah settings")
    load_lines = re.findall(r"^(?:Load statistics|\u8f7d\u5165\u9636\u6bb5\u7edf\u8ba1\u6570\u636e): Timnah (.+)$", raw, re.M)
    require(len(load_lines) == 1, "Exactly one complete Timnah load statistic is required")
    loaded = fields(load_lines[0])
    require_fields(loaded, {"size": config["num_load"], "fast_hits": 0, "slow_hits": 0, "misses": 0,
        "observed_rounds": 0, "tracking": 1, "converged": 0, "rates_frozen": 0,
        "fast_node": 0, "slow_node": 2, "fast_segments": 4096,
        "fast_bucket_bytes": 256 << 20, "fast_counter_bytes": 64 << 20}, "Loaded Timnah")
    traces = replay['audits'] if replay else []
    if is_storage:
        p=replay['profile']
        require('Real trace preset: timnah_convergence_threshold=0' in raw,'Continuous adaptation is required')
    else:
        require('Real profile=' not in raw,'Unexpected real trace in a YCSB run')
    markers = list(re.finditer(r"^Round (\d+)\s*$", raw, re.M))
    require([int(m[1]) for m in markers] == list(range(10)), "Every round must occur exactly once")
    convergence_events = [fields(line) for line in re.findall(r"^Timnah convergence (.+)$", raw, re.M)]
    require(len(convergence_events) <= 1 and (not is_storage or not convergence_events),
            "Unexpected convergence events for the selected adaptation policy")
    parsed, previous = [], loaded
    heat_keys = ("samples", "reorders", "promotions", "demotions", "decay_events")
    for number, marker in enumerate(markers):
        end = markers[number + 1].start() if number < 9 else len(raw)
        block = raw[marker.end():end]
        rates = re.findall(r"^(?:Run throughput|\u8fd0\u884c\u541e\u5410): ([0-9]+(?:\.[0-9]+)?) Mops/s$", block, re.M)
        native = re.findall(r"^(?:Run statistics|\u6267\u884c\u9636\u6bb5\u7edf\u8ba1\u6570\u636e): Timnah (.+)$", block, re.M)
        require(len(rates) == len(native) == 1, "Each round requires one throughput and Timnah statistic")
        rate = float(rates[0]); require(math.isfinite(rate) and rate > 0, "Throughput must be measured and positive")
        row = fields(native[0])
        require_fields(row, {"size": config["num_load"], "misses": 0, "fast_node": 0, "slow_node": 2,
            "fast_segments": 4096, "fast_bucket_bytes": 256 << 20, "fast_counter_bytes": 64 << 20}, "Round Timnah")
        require(row.get("tracking") in ("0", "1") and row.get("converged") in ("0", "1"), "Invalid maintenance state")
        require(row["tracking"] != row["converged"] and row.get("rates_frozen") == row["converged"],
                "The heat and convergence state is inconsistent")
        # Background movement updates its counters separately; an active
        # snapshot need not observe equal promotion and demotion counts.
        require(all(int(row[k]) >= int(previous[k]) for k in heat_keys), "Maintenance counters decreased")
        require(all(int(row[k]) >= int(previous[k]) for k in ("fast_hits", "slow_hits")), "Search counters decreased")
        counted_reads = sum(int(row[k]) - int(previous[k]) for k in ("fast_hits", "slow_hits"))
        trace = None
        if is_storage:
            trace = traces[number]
            reads, updates = int(trace["reads"]), int(trace["updates"])
            require(reads >= 0 and updates >= 0 and reads + updates == config["num_run"], "Invalid Storage operation counts")
            require(1 <= int(trace["key_id_min"]) <= int(trace["key_id_max"]) <= config["num_load"]
                    and int(trace["generation_ns"]) > 0, "Invalid Storage trace domain or generation time")
            require_fields(row, {"tracking": 1, "converged": 0, "rates_frozen": 0, "observed_rounds": number + 1}, "Dynamic maintenance")
            require(counted_reads == reads, "Native Search counters disagree with the dynamic trace")
        elif previous["tracking"] == "1":
            require(counted_reads == config["num_run"], "Tracked Search counts differ from the YCSB trace")
            require(int(row["observed_rounds"]) == int(previous["observed_rounds"]) + 1, "Round observation is not adjacent")
        else:
            require_fields(row, {"tracking": 0, "converged": 1, "rates_frozen": 1,
                                "observed_rounds": previous["observed_rounds"]}, "Frozen maintenance")
            frozen_keys = heat_keys + ("last_round_fast_rate", "last_round_slow_rate", "convergence_diff")
            require(counted_reads == 0 and all(row[k] == previous[k] for k in frozen_keys),
                    "Frozen heat or Search counters unexpectedly changed")
        if row["tracking"] == "0":
            # The native comparison uses full precision but logs nine decimal
            # places. A matching native event justifies the display tolerance.
            require(0 <= float(row["convergence_diff"]) <= 0.002 + 0.5e-9 and int(row["observed_rounds"]) >= 2
                    and int(row["queue_pending"]) == 0
                    and int(row["in_flight"]) == 0, "The convergence freeze did not finish maintenance")
            require(len(convergence_events) == 1, "A frozen run requires its native convergence event")
            event = convergence_events[0]
            require_fields(event, {"action": "freeze_heat_and_migration",
                                  "round": int(row["observed_rounds"]) - 1}, "Convergence event")
            require(abs(float(event["threshold"]) - 0.002) <= 1e-12 and
                    abs(float(event["l1_diff"]) - float(row["convergence_diff"])) <= 0.5e-9 and
                    abs(float(event["fast_rate"]) - float(row["last_round_fast_rate"])) <= 0.5e-9 and
                    abs(float(event["slow_rate"]) - float(row["last_round_slow_rate"])) <= 0.5e-9,
                    "The printed convergence event disagrees with the frozen native state")
        block_events = re.findall(r"^Timnah convergence (.+)$", block, re.M)
        require(len(block_events) == (1 if previous["tracking"] == "1" and row["tracking"] == "0" else 0),
                "The convergence event is not in the round that froze maintenance")
        for key in ("fast_hit_rate", "last_round_fast_rate", "last_round_slow_rate"):
            require(0 <= float(row[key]) <= 1, "Invalid tier hit-rate statistic")
        parsed.append({"round": number, "throughput_Mops": rate, "stats": row, "trace": trace})
        previous = row
    final = parsed[-1]["stats"]
    evidence = {"memory_initial": initial, "load": loaded, "traces": traces, "convergence_events": convergence_events,
        "maintenance_final": {k: final[k] for k in ("tracking", "converged", "rates_frozen", "observed_rounds",
            "last_round_fast_rate", "last_round_slow_rate", "convergence_diff", "samples", "reorders", "promotions", "demotions")}}
    return parsed, evidence


def configuration(args, series_id, k):
    d = SERIES[series_id]
    command = [str(args.binary), "-index=Timnah", "-workload=" + d["workload"],
        "-num_rounds=10", "-num_threads=30", "-num_load_threads=30", "-num_run_threads=30", "-num_bg=1",
        "-num_load=" + str(args.num_load), "-num_run=" + str(args.num_run), "-rndSeed=1729",
        "-latency=true", "-latency_sample_rate=0.01", "-bind_core=true", "-verify=false", "-shift=false", "-shuffle=true",
        "-keySize=8", "-valueSize=8", "-timnah_cross_multiplier=" + format(k, ".12g"),
        "-timnah_intra_multiplier=2", "-timnah_sample_rate=0.1", "-timnah_decay_ms=1000", "-timnah_decay_factor=0.5",
        "-timnah_queue_capacity=1024", "-timnah_huge_pages=true", "-timnah_fast_depth=12", "-timnah_slow_depth=14",
        "-timnah_fast_node=0", "-timnah_slow_node=2"]
    if d["workload"] == "c":
        command.append("-skewness=" + format(d["skewness"], ".12g"))
    else:
        command += ["-real_epoch_ops=" + str(EPOCH_OPS), "-real_drift_stride=" + str(max(args.num_load // 100, 1))]
    cpulist = Path("/sys/devices/system/node/node0/cpulist")
    return {"series_id": series_id, "workload": d["workload"], "skewness": d["skewness"], "k": float(k),
        "num_threads": 30, "num_bg": 1, "num_rounds": 10, "num_load": args.num_load, "num_run": args.num_run,
        "expected_cpu_affinity": cpulist.read_text().strip() if cpulist.is_file() else None, "command": command}


def cached(path, expected, config):
    try:
        receipt = json.loads(path.read_text())
        if (receipt["schema_version"] != SCHEMA_VERSION or receipt["fingerprint"] != expected
                or receipt["config"] != config or receipt["exit_code"] != 0 or receipt["validated"] is not True
                or not config["expected_cpu_affinity"] or receipt["cpu_affinity"] != config["expected_cpu_affinity"]):
            return None
        raw = gzip.decompress(Path(receipt["stdout_gzip"]).read_bytes())
        log = Path(receipt["log"]).read_bytes()
        if (digest_bytes(raw) != receipt["stdout_sha256"] or digest_bytes(log) != receipt["log_sha256"]
                or log != english_text(raw.decode("utf-8", "replace")).encode("ascii")):
            return None
        rounds, evidence = parse_run(raw.decode("utf-8", "replace"), config)
        measurement = {"k": config["k"], "round": 9, "throughput_Mops": rounds[9]["throughput_Mops"]}
        if rounds != receipt["rounds"] or evidence != receipt["evidence"] or measurement != receipt["measurement"]:
            return None
        return receipt
    except (OSError, ValueError, KeyError, TypeError, EOFError, zlib.error):
        return None


def assert_idle():
    for p in Path("/proc").glob("[0-9]*/comm"):
        try:
            name = p.read_text().strip()
            if name in ("ycsb", "ycsb.before", "figure11_probe", "figure12_probe") or name.startswith(("figure13_", "figure14_", "figure15_")):
                if (p.parent / "stat").read_text().split(") ", 1)[1][0] != "Z":
                    raise RuntimeError("Another benchmark is active: " + p.parent.name)
        except (OSError, ProcessLookupError):
            pass


def execute(args, config, path, expected):
    raw_path = path.with_suffix(".stdout.pending")
    started, begin = now(), time.monotonic()
    expected_affinity = Path("/sys/devices/system/node/node0/cpulist").read_text().strip()
    require(expected_affinity == config["expected_cpu_affinity"], "The CPU node topology changed")
    observed_affinity = None
    with raw_path.open("wb") as stream:
        process = subprocess.Popen(config["command"], cwd=args.binary.parent, stdout=stream,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        heartbeat = begin
        try:
            while True:
                try:
                    status = Path("/proc/{}/status".format(process.pid)).read_text()
                    match = re.search(r"^Cpus_allowed_list:\s*(.+)$", status, re.M)
                    if match and match[1] == expected_affinity:
                        observed_affinity = match[1]
                except (OSError, ProcessLookupError):
                    pass
                try:
                    code = process.wait(timeout=0.1)
                    break
                except subprocess.TimeoutExpired:
                    elapsed = time.monotonic() - begin
                    if args.timeout and elapsed >= args.timeout:
                        raise TimeoutError("The benchmark exceeded the requested timeout")
                    if time.monotonic() - heartbeat >= 30:
                        print("Still running {} at k={}: {:.0f} seconds.".format(config["series_id"], format(config["k"], "g"), elapsed), flush=True)
                        heartbeat = time.monotonic()
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
        raise RuntimeError("The benchmark exited with {}. See {}".format(code, log))
    require(observed_affinity == expected_affinity, "Node 0 CPU affinity was not observed")
    rounds, evidence = parse_run(raw.decode("utf-8", "replace"), config)
    return {"schema_version": SCHEMA_VERSION, "fingerprint": expected, "config": config,
        "exit_code": code, "validated": True, "started_utc": started, "finished_utc": now(),
        "elapsed_seconds": time.monotonic() - begin, "cpu_affinity": observed_affinity,
        "stdout_gzip": str(compressed), "stdout_sha256": digest_bytes(raw), "log": str(log), "log_sha256": digest_file(log),
        "rounds": rounds, "evidence": evidence,
        "measurement": {"k": config["k"], "round": 9, "throughput_Mops": rounds[9]["throughput_Mops"]}}


def frozen(args, original, env):
    if digest_file(Path(__file__).resolve()) != args.collector_sha256:
        raise RuntimeError("The collector changed during collection")
    if production_versions(args.repository, args.binary) != original or environment() != env:
        raise RuntimeError("Ordinary sources, executable, libraries, build configuration or environment changed")


def publish(args, accepted, session_key):
    series = {name: [accepted[(name, k)]["measurement"] for k in K_VALUES if (name, k) in accepted] for name in SERIES}
    runs = {name: [accepted[(name, k)] for k in K_VALUES if (name, k) in accepted] for name in SERIES}
    complete = all([r["k"] for r in series[name]] == list(K_VALUES) for name in SERIES)
    output = {"schema_version": SCHEMA_VERSION, "metric": METRIC, "index": "Timnah", "num_threads": 30,
        "num_bg": 1, "num_rounds": 10, "num_load": args.num_load, "num_run": args.num_run, "round": 9,
        "complete": complete, "k_values": list(K_VALUES), "selected_series": args.series,
        "selected_k_values": args.k_values, "series_definitions": SERIES,
        "version_fingerprint": session_key, "updated_utc": now(), "series": series, "runs": runs,
        "capacity_scope": "256 MiB fast buckets plus 64 MiB counters; metadata additional on node 0",
        "note": "Actual ordinary benchmark throughput from round 9, with all ten rounds retained. "
                "Only the existing cross-tier multiplier startup flag changes within a series. "
                "The intra-tier multiplier remains 2; sampling, decay, NUMA placement and source code are unchanged. "
                "Static C keeps the current 0.002 convergence freeze; Storage keeps continuous adaptation. "
                "The implementation also allows promotion when the candidate exceeds the hottest fast-slot score; "
                "the k-times-minimum condition is not the only promotion path. Zero cold counters can make k irrelevant. "
                "The configured latency sampler stays enabled at 0.01. No simulated or interpolated performance values."}
    atomic_json(args.output_dir / DATA_FILENAME, output)


def parser():
    folder = Path(__file__).resolve().parent
    p = EnglishArgumentParser(description=__doc__)
    p.add_argument("--repository", type=Path, default=folder.parent)
    p.add_argument("--binary", type=Path, default=None)
    p.add_argument("--output-dir", type=Path, default=folder)
    p.add_argument("--series", nargs="+", choices=SERIES, default=list(SERIES))
    p.add_argument("--k-values", nargs="+", type=float, choices=K_VALUES, default=list(K_VALUES))
    p.add_argument("--num-load", type=int, default=100000000)
    p.add_argument("--num-run", type=int, default=100000000)
    p.add_argument("--timeout", type=float, default=0)
    p.add_argument("--force", action="store_true")
    p.add_argument("--validate-only", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    if args.num_load < 10000 or args.num_run < 10000 or not math.isfinite(args.timeout) or args.timeout < 0:
        raise ValueError("Counts must be at least 10000 and timeout finite/nonnegative")
    if len(set(args.series)) != len(args.series) or len(set(args.k_values)) != len(args.k_values):
        raise ValueError("Series and multiplier values must be unique")
    args.repository, args.output_dir = args.repository.resolve(), args.output_dir.resolve()
    args.binary = (args.binary or args.repository / "build/benchmark/ycsb/ycsb").resolve(strict=True)
    plan = [configuration(args, s, k) for s in args.series for k in args.k_values]
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
        original, env = production_versions(args.repository, args.binary), environment()
        args.collector_sha256 = digest_file(Path(__file__).resolve())
        frozen(args, original, env)
        versions = {"real_trace_data": real_trace_support.datasets(args.repository), "production": original, "collector_sha256": args.collector_sha256}
        session_key = fingerprint({"versions": versions, "environment": env,
                                   "num_load": args.num_load, "num_run": args.num_run})
        session = args.output_dir / "runs-figure15" / session_key[:16]
        session.mkdir(parents=True, exist_ok=True)
        accepted, missing = {}, []
        for c in plan:
            slug = format(c["k"], ".12g").replace(".", "p")
            path = session / (c["series_id"] + "-k" + slug + ".json")
            expected = fingerprint({"session": session_key, "config": c})
            record = None if args.force else cached(path, expected, c)
            if record is None:
                missing.append((c, path, expected))
            else:
                accepted[(c["series_id"], c["k"])] = record
        count = len(plan) - len(missing)
        print("Validated cached points: {}/{}. Missing: {}.".format(count, len(plan), len(missing)), flush=True)
        if args.validate_only:
            return 0 if not missing else 2
        metadata = {"schema_version": 1, "status": "running", "started_utc": now(), "versions": versions,
            "environment": env, "plan": plan, "completed": count, "expected": len(plan), "session_fingerprint": session_key}
        atomic_json(session / "session.json", metadata)
        publish(args, accepted, session_key)
        try:
            for c, path, expected in missing:
                assert_idle()
                frozen(args, original, env)
                print("[{}/{}] Running {} at k={}: 30 threads, ten rounds, one background worker.".format(
                    count + 1, len(plan), c["series_id"], format(c["k"], "g")), flush=True)
                record = execute(args, c, path, expected)
                frozen(args, original, env)
                atomic_json(path, record)
                accepted[(c["series_id"], c["k"])] = record
                count += 1
                publish(args, accepted, session_key)
                metadata["completed"] = count
                atomic_json(session / "session.json", metadata)
                print("Validated round 9: {:.3f} Mops/s. Completed {}/{}.".format(
                    record["measurement"]["throughput_Mops"], count, len(plan)), flush=True)
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


