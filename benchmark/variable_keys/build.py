#!/usr/bin/env python3
"""Build and install the independent variable-key executable safely."""
from __future__ import annotations

import argparse
from contextlib import ExitStack
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid


SCHEMA_VERSION = 1
SOURCE_SUFFIXES = {".h", ".hpp", ".cc", ".cpp", ".cmake", ".txt"}
BUILD_MARKER = ".variable-keys-build.json"
DISPATCH_MANIFEST = "ycsb.dispatch.json"
EXECUTABLES = {"ycsb_variable": "runtime/ycsb", "ycsb_dispatch": "ycsb_dispatch",
               "variable_key_correctness": "variable_key_correctness",
               "variable_trace_correctness": "variable_trace_correctness"}
CORRECTNESS_TARGETS = ("variable_key_correctness", "variable_trace_correctness")


def english(value):
    return str(value).encode("ascii", "backslashreplace").decode("ascii")


def now():
    return datetime.now(timezone.utc).isoformat()


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load(path):
    return json.loads(Path(path).read_text())


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def atomic_bytes(path, data, mode=0o644):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix="." + path.name + ".install-", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def atomic_json(path, value):
    atomic_bytes(path, (json.dumps(value, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode("ascii"))


def atomic_hardlink(source, destination):
    source, destination = Path(source), Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix="." + destination.name + ".install-", dir=destination.parent)
    os.close(descriptor)
    temporary = Path(name)
    temporary.unlink()
    try:
        os.link(source, temporary)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def matching_alias(scalar):
    scalar = Path(scalar)
    alias = scalar.parent / "libycsb8.so"
    return (scalar.is_file() and alias.is_file() and not scalar.is_symlink()
            and not alias.is_symlink() and os.path.samefile(scalar, alias))


def elf(path):
    path = Path(path)
    require(path.is_file() and not path.is_symlink(), "Expected a regular executable: " + str(path))
    with path.open("rb") as stream:
        require(stream.read(4) == b"\x7fELF", "Expected a Linux ELF executable: " + str(path))
    require(os.access(path, os.X_OK), "Executable permission is missing: " + str(path))


def assert_idle():
    for comm in Path("/proc").glob("[0-9]*/comm"):
        try:
            name = comm.read_text().strip()
            candidate = name in {"ycsb", "ycsb8", "ycsb_variable", "ycsb.before", "figure11_probe", "figure12_probe"}
            candidate = candidate or name.startswith(("figure13_", "figure14_", "figure15_", "figure16_"))
            if candidate and (comm.parent / "stat").read_text().split(") ", 1)[1][0] != "Z":
                raise RuntimeError("Another benchmark is active: pid=" + comm.parent.name + " name=" + name)
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass


def paths(repository):
    folder = repository / "build/benchmark/ycsb"
    return folder / "ycsb", folder.parent / "ycsb8/ycsb", folder / DISPATCH_MANIFEST


def installed(repository):
    ordinary, scalar, metadata = paths(repository)
    if not metadata.is_file():
        return None
    value = load(metadata)
    require(value.get("schema_version") == SCHEMA_VERSION, "Unsupported dispatcher manifest")
    require(Path(value.get("repository", "")).resolve() == repository, "Dispatcher manifest belongs to another checkout")
    require(Path(value.get("scalar_binary", "")).resolve() == scalar.resolve(), "Dispatcher scalar path was changed")
    require(Path(value.get("scalar_alias", "")).resolve() == (scalar.parent / "libycsb8.so").resolve(),
            "Dispatcher scalar fingerprint alias path was changed")
    return value


def scalar_source(repository):
    ordinary, scalar, _ = paths(repository)
    current = installed(repository)
    if current and sha256(ordinary) == current["dispatcher_sha256"]:
        require(sha256(scalar) == current["scalar_sha256"] and matching_alias(scalar),
                "The preserved scalar executable or fingerprint alias changed")
        return scalar
    return ordinary


def private_source_versions(repository):
    result = {}
    roots = ("benchmark/variable_keys", "hashindex/variable_keys", "utils",
             "benchmark/ycsb/modules", "benchmark/synthetic", "benchmark/twitter", "benchmark/webmail")
    for relative in roots:
        for path in sorted((repository / relative).rglob("*")):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES | {".py"}:
                result[str(path)] = sha256(path)
    shared = repository / "hashindex/tier_budget/tiered_memory.h"
    if shared.is_file():
        result[str(shared)] = sha256(shared)
    return result


def compile_configuration_versions(build_directory):
    result = {}
    cache = build_directory / "CMakeCache.txt"
    if cache.is_file():
        result[str(cache)] = sha256(cache)
    for pattern in ("flags.make", "link.txt"):
        for path in sorted((build_directory / "CMakeFiles").rglob(pattern)):
            if path.is_file():
                result[str(path)] = sha256(path)
    return result


def production_versions(repository, build_directory):
    ordinary, scalar, metadata = paths(repository)
    actual = scalar_source(repository)
    elf(actual)
    candidates = {ordinary.resolve(), actual.resolve()}
    if scalar.is_file():
        candidates.add(scalar.resolve())
    for relative in ("utils", "hashindex", "benchmark"):
        for path in (repository / relative).rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                if path.is_relative_to(repository / "hashindex/variable_keys") or path.is_relative_to(repository / "benchmark/variable_keys"):
                    continue
                candidates.add(path.resolve())
    for relative in ("CMakeLists.txt", "dependency.cmake", "build/CMakeCache.txt"):
        path = repository / relative
        if path.is_file():
            candidates.add(path.resolve())
    for path in (repository / "build").rglob("*"):
        if path.is_relative_to(build_directory):
            continue
        if path.is_file() and (".so" in path.name or path.name == "flags.make"):
            candidates.add(path.resolve())
    linked = subprocess.run(["ldd", str(actual)], check=True, capture_output=True, text=True)
    require("not found" not in linked.stdout, "A scalar runtime dependency cannot be resolved")
    for name in re.findall(r"(?:=>\s+)?(/[^\s]+)", linked.stdout):
        path = Path(name)
        if path.is_file():
            candidates.add(path.resolve())
    return {str(path): sha256(path) for path in sorted(candidates)}


def check_versions(versions, exceptions=()):
    exceptions = {str(Path(path)) for path in exceptions}
    changed = [name for name, digest in versions.items()
               if name not in exceptions and (not Path(name).is_file() or sha256(name) != digest)]
    require(not changed, "Protected production files changed: " + repr(changed))


def verify_supplied_manifest(repository, path):
    if path is None:
        return
    ordinary, scalar, _ = paths(repository)
    current = installed(repository)
    for name, digest in load(path).items():
        actual = scalar if current and Path(name) == ordinary else Path(name)
        require(actual.is_file() and sha256(actual) == digest, "Supplied protected baseline differs: " + name)


def backup(repository, build_directory, versions, scalar_source):
    label = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    folder = build_directory / "backups" / label
    folder.mkdir(parents=True, exist_ok=False)
    atomic_json(folder / "production-before.json", versions)
    with tarfile.open(folder / "production-before.tar.gz", "w:gz") as archive:
        for name in versions:
            path = Path(name)
            member = "repository/" + str(path.relative_to(repository)) if path.is_relative_to(repository) else "runtime/" + str(path).lstrip("/")
            archive.add(path, arcname=member, recursive=False)
    atomic_bytes(folder / "ycsb8.original", scalar_source.read_bytes(), stat.S_IMODE(scalar_source.stat().st_mode))
    require(sha256(folder / "ycsb8.original") == sha256(scalar_source), "Scalar executable backup verification failed")
    check_versions(versions)
    return folder


def child(command):
    print("Running: " + english(" ".join(map(str, command))), flush=True)
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    # This standalone build owns its explicit job count. Do not inherit a
    # parent's GNU make jobserver whose file descriptors are not passed here.
    for name in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL"):
        environment.pop(name, None)
    subprocess.run(list(map(str, command)), check=True, env=environment)


def prepare_build_directory(repository, build_directory):
    require(build_directory not in (repository, repository / "build"), "The independent build cannot replace the repository or ordinary build")
    require(not repository.is_relative_to(build_directory), "The independent build cannot contain the repository")
    require(not any(build_directory.is_relative_to(repository / name) for name in ("utils", "hashindex", "benchmark")),
            "The independent build cannot write into production source directories")
    marker = build_directory / BUILD_MARKER
    if marker.exists():
        value = load(marker)
        require(value.get("repository") == str(repository), "Independent build belongs to another checkout")
    elif build_directory.exists() and any(build_directory.iterdir()):
        raise RuntimeError("Refusing to use an unrecognized nonempty build directory")
    build_directory.mkdir(parents=True, exist_ok=True)
    atomic_json(marker, {"schema_version": 1, "repository": str(repository),
                         "source": str(repository / "benchmark/variable_keys")})


def cmake_hints(repository):
    """Read dependency search locations without changing the ordinary cache."""
    cache = repository / "build/CMakeCache.txt"
    selected = {"fmt_DIR", "GTest_DIR", "gflags_DIR", "jsoncpp_DIR",
                "CMAKE_PREFIX_PATH", "CMAKE_CXX_COMPILER", "CMAKE_CXX_COMPILER_ARG1"}
    result = {}
    if cache.is_file():
        for line in cache.read_text().splitlines():
            match = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_]*):[^=]*=(.*)", line)
            if match and match[1] in selected and match[2] and not match[2].endswith("-NOTFOUND"):
                result[match[1]] = match[2]
    return result


def configure(repository, build_directory, expected_scalar_sha256):
    source = repository / "benchmark/variable_keys"
    hints = cmake_hints(repository)
    child(["cmake", "-S", source, "-B", build_directory,
           "-DCMAKE_BUILD_TYPE=Release", "-DHASHING_REPOSITORY=" + str(repository),
           "-DHASHING_SCALAR_SHA256=" + expected_scalar_sha256] +
          ["-D" + name + "=" + value for name, value in sorted(hints.items())])
    return hints


def build(repository, build_directory, jobs):
    source_before = private_source_versions(repository)
    previous = None
    try:
        previous = verified_build(repository, build_directory)
    except (OSError, RuntimeError, ValueError, KeyError):
        pass
    expected_scalar_sha256 = sha256(scalar_source(repository))
    hints = configure(repository, build_directory, expected_scalar_sha256)
    child(["cmake", "--build", build_directory, "--parallel", str(jobs), "--target",
           *EXECUTABLES])
    for relative in EXECUTABLES.values():
        elf(build_directory / relative)
    executable_hashes = {relative: sha256(build_directory / relative)
                         for relative in EXECUTABLES.values()}
    compile_configuration = compile_configuration_versions(build_directory)
    start = time.monotonic()
    reuse_correctness = (previous is not None
        and compile_configuration == previous["compile_configuration_sha256"] and all(
        executable_hashes[relative] == previous["executables_sha256"][relative]
        for relative in EXECUTABLES.values()))
    if reuse_correctness:
        correctness_tests = previous["correctness_tests"]
        correctness_elapsed = previous["correctness_elapsed_seconds"]
        print("Reusing passing correctness proofs for unchanged private sources and executables.", flush=True)
    else:
        correctness_tests = {}
        for name in CORRECTNESS_TARGETS:
            test_start = time.monotonic()
            child([build_directory / name])
            correctness_tests[name] = {"passed": True, "elapsed_seconds": time.monotonic() - test_start}
        correctness_elapsed = time.monotonic() - start
    require(private_source_versions(repository) == source_before,
            "Private sources changed during compilation or correctness testing")
    report = {"schema_version": 1, "repository": str(repository), "build_directory": str(build_directory),
              "finished_utc": now(), "build_type": "Release", "correctness_passed": True,
              "trace_correctness_passed": True, "correctness_tests": correctness_tests,
              "dependency_search_hints": hints,
              "expected_scalar_sha256": expected_scalar_sha256,
              "executable_targets": EXECUTABLES,
              "correctness_elapsed_seconds": correctness_elapsed,
              "correctness_reused_unchanged_executables": reuse_correctness,
              "correctness_original_finished_utc": (previous.get("correctness_original_finished_utc", previous["finished_utc"])
                                                     if reuse_correctness else now()),
              "private_sources_sha256": source_before,
              "compile_configuration_sha256": compile_configuration,
              "executables_sha256": executable_hashes}
    atomic_json(build_directory / "build-report.json", report)
    return report


def verified_build(repository, build_directory):
    report = load(build_directory / "build-report.json")
    require(report.get("schema_version") == 1 and report.get("correctness_passed") is True
            and report.get("repository") == str(repository), "A passing independent build report is required")
    require(report.get("trace_correctness_passed") is True
            and set(report.get("correctness_tests", {})) == set(CORRECTNESS_TARGETS)
            and all(report["correctness_tests"][name].get("passed") is True for name in CORRECTNESS_TARGETS),
            "Passing index and trace correctness reports are required")
    require(report["private_sources_sha256"] == private_source_versions(repository), "Private sources changed after the passing correctness test")
    configuration = compile_configuration_versions(build_directory)
    require(str(build_directory / "CMakeCache.txt") in configuration
            and report.get("compile_configuration_sha256") == configuration,
            "Private compilation configuration changed after correctness testing")
    require(report.get("executable_targets") == EXECUTABLES
            and set(report["executables_sha256"]) == set(EXECUTABLES.values()),
            "The independent build executable layout changed")
    for name, digest in report["executables_sha256"].items():
        elf(build_directory / name)
        require(sha256(build_directory / name) == digest, "Private executable changed after correctness testing: " + name)
    return report


def refresh_dispatch_build(repository, build_directory, jobs):
    report = verified_build(repository, build_directory)
    old_executables = dict(report["executables_sha256"])
    digest = sha256(scalar_source(repository))
    hints = configure(repository, build_directory, digest)
    child(["cmake", "--build", build_directory, "--parallel", str(jobs), "--target", "ycsb_dispatch"])
    elf(build_directory / "ycsb_dispatch")
    require(private_source_versions(repository) == report["private_sources_sha256"],
            "Private sources changed during dispatcher refresh")
    for relative in (relative for name, relative in EXECUTABLES.items() if name != "ycsb_dispatch"):
        require(sha256(build_directory / relative) == old_executables[relative],
                "Dispatcher refresh unexpectedly changed a private benchmark or correctness executable")
    report["executables_sha256"]["ycsb_dispatch"] = sha256(build_directory / "ycsb_dispatch")
    report.update(expected_scalar_sha256=digest, dispatcher_refreshed_utc=now(), dependency_search_hints=hints,
                  compile_configuration_sha256=compile_configuration_versions(build_directory))
    atomic_json(build_directory / "build-report.json", report)
    return report


def refresh_requires_rebuild(repository, build_directory):
    try:
        proof = verified_build(repository, build_directory)
        require(proof.get("dependency_search_hints") == cmake_hints(repository),
                "Ordinary dependency or compiler settings changed")
        return False
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print("Recovering the independent build before dispatch refresh: " + english(error), flush=True)
        # A fully deleted private directory must regain its ownership marker
        # before the recovery backup creates any files inside that directory.
        prepare_build_directory(repository, build_directory)
        return True


def install(repository, build_directory, versions, backup_directory, refresh=False):
    ordinary, scalar, metadata = paths(repository)
    current = installed(repository)
    dispatcher = build_directory / "ycsb_dispatch"
    variable = build_directory / "runtime/ycsb"
    alias = scalar.parent / "libycsb8.so"
    for path in (ordinary, dispatcher, variable):
        elf(path)
    dispatch_digest = sha256(dispatcher)
    if current:
        elf(scalar)
        require(sha256(scalar) == current["scalar_sha256"], "The preserved scalar executable changed")
        require(matching_alias(scalar), "The preserved scalar fingerprint alias changed")
    if current and sha256(ordinary) == current["dispatcher_sha256"]:
        source = scalar
    else:
        # A root build has produced a fresh ordinary ELF at the original path.
        # Keep its exact bytes before restoring dispatch; never recompile it here.
        source = ordinary
        if scalar.exists() and not current:
            require(sha256(scalar) == sha256(source), "Refusing to overwrite an unrecognized scalar sibling")
        if alias.exists() and not current:
            require(matching_alias(scalar), "Refusing to overwrite an unrecognized scalar fingerprint alias")
    scalar_bytes = source.read_bytes()
    scalar_digest = hashlib.sha256(scalar_bytes).hexdigest()
    scalar_mode = stat.S_IMODE(source.stat().st_mode)
    require(load(build_directory / "build-report.json")["expected_scalar_sha256"] == scalar_digest,
            "The dispatcher is pinned to a different scalar executable; rebuild the dispatcher before installing")
    check_versions(versions)
    assert_idle()
    atomic_bytes(scalar, scalar_bytes, scalar_mode)
    atomic_hardlink(scalar, alias)
    require(sha256(scalar) == scalar_digest, "Preserved scalar executable verification failed")
    require(matching_alias(scalar) and sha256(alias) == scalar_digest,
            "Preserved scalar fingerprint alias verification failed")
    atomic_bytes(ordinary, dispatcher.read_bytes(), 0o755)
    manifest = {"schema_version": SCHEMA_VERSION, "repository": str(repository), "installed_utc": now(),
                "scalar_binary": str(scalar), "scalar_sha256": scalar_digest,
                "scalar_alias": str(alias), "scalar_alias_sha256": scalar_digest,
                "variable_binary": str(variable), "variable_sha256": sha256(variable),
                "dispatcher_sha256": dispatch_digest, "build_directory": str(build_directory),
                "backup_directory": str(backup_directory), "refresh_after_ordinary_relink": refresh,
                "ordinary_index_libraries_rebuilt": False, "ordinary_measurement_path_changed": False}
    atomic_json(metadata, manifest)
    # The preserved executable intentionally changes only during a future root
    # relink. All other protected source, library and configuration files stay put.
    exceptions = (ordinary, scalar, alias) if refresh else (ordinary,)
    check_versions(versions, exceptions)
    require(sha256(ordinary) == dispatch_digest and sha256(scalar) == scalar_digest,
            "Dispatcher installation verification failed")
    return manifest


def restore(repository):
    ordinary, scalar, metadata = paths(repository)
    current = installed(repository)
    require(current is not None, "Variable-key dispatch is not installed")
    require(sha256(ordinary) == current["dispatcher_sha256"], "The current executable is not the installed dispatcher")
    require(sha256(scalar) == current["scalar_sha256"], "The preserved scalar executable changed")
    require(matching_alias(scalar), "The preserved scalar fingerprint alias changed")
    atomic_bytes(ordinary, scalar.read_bytes(), stat.S_IMODE(scalar.stat().st_mode))
    atomic_json(Path(current["backup_directory"]) / "restored-dispatch-manifest.json", current)
    metadata.unlink()
    return {"restored": str(ordinary), "sha256": sha256(ordinary), "private_sources_or_build_deleted": False}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--build-directory", type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--protected-manifest", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--build-only", action="store_true")
    mode.add_argument("--install-only", action="store_true")
    mode.add_argument("--refresh-dispatch", action="store_true")
    mode.add_argument("--restore", action="store_true")
    args = parser.parse_args(argv)
    repository = args.repository.resolve(strict=True)
    build_directory = (args.build_directory or repository / "build/variable_keys").resolve()
    require(args.jobs > 0, "The parallel build job count must be positive")
    ordinary, scalar, metadata = paths(repository)
    if args.refresh_dispatch:
        # Figure source snapshots have a different real path and no primary
        # installation. Their ordinary executables must remain ordinary.
        if not metadata.is_file() or Path(load(metadata).get("repository", "")).resolve() != repository:
            print("Dispatcher refresh skipped: no installation for this checkout.")
            return 0
        current = installed(repository)
        if args.build_directory is None:
            build_directory = Path(current["build_directory"]).resolve()
        require(Path(current["build_directory"]).resolve() == build_directory,
                "Dispatcher refresh must use the installed private build directory")
    if not args.refresh_dispatch and not args.restore:
        require((repository / "benchmark/variable_keys/main.cpp").is_file(), "Variable-key benchmark sources are missing")
    with ExitStack() as stack:
        lock_path = repository / "figure/.collection.lock"
        if lock_path.parent.is_dir():
            lock = stack.enter_context(lock_path.open("a"))
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        assert_idle()
        if args.restore:
            print(json.dumps(restore(repository), indent=2))
            return 0
        verify_supplied_manifest(repository, args.protected_manifest)
        if args.refresh_dispatch:
            current = installed(repository)
            recover_private_build = refresh_requires_rebuild(repository, build_directory)
            versions = production_versions(repository, build_directory)
            scalar_source = scalar if sha256(ordinary) == current["dispatcher_sha256"] else ordinary
            backup_directory = backup(repository, build_directory, versions, scalar_source)
            if recover_private_build:
                build(repository, build_directory, args.jobs)
            else:
                refresh_dispatch_build(repository, build_directory, args.jobs)
            result = install(repository, build_directory, versions, backup_directory, refresh=True)
            result["private_build_recovered"] = recover_private_build
        else:
            prepare_build_directory(repository, build_directory)
            versions = production_versions(repository, build_directory)
            current = installed(repository)
            scalar_source = scalar if current and sha256(ordinary) == current["dispatcher_sha256"] else ordinary
            backup_directory = backup(repository, build_directory, versions, scalar_source)
            if args.install_only:
                verified_build(repository, build_directory)
            else:
                build(repository, build_directory, args.jobs)
            check_versions(versions)
            if args.build_only:
                result = {"build_only": True, "build_directory": str(build_directory),
                          "backup_directory": str(backup_directory), "protected_files_unchanged": len(versions)}
            else:
                result = install(repository, build_directory, versions, backup_directory)
        result["protected_file_count"] = len(versions)
        print(json.dumps(result, indent=2, ensure_ascii=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print("Variable-key build error: " + english(error), file=sys.stderr)
        raise SystemExit(1)
