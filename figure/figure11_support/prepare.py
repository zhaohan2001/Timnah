#!/usr/bin/env python3
"""Prepare an isolated, instrumented source copy for Figure 11.

Production source files and build artifacts are read only. Search hooks count
the tier of each successful lookup after its normal stability checks. Native
heat counters and convergence logic are left unchanged.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile


MARKER = ".figure11-diagnostic-copy.json"
ROOTS = (
    "utils",
    "hashindex/timnah",
    "hashindex/dash",
    "hashindex/chash",
    "hashindex/ehcache",
    "hashindex/tier_budget",
    "benchmark/ycsb/modules",
    "benchmark/synthetic",
    "benchmark/twitter",
    "benchmark/webmail",
)
SUFFIXES = {".h", ".hpp", ".cpp", ".cc"}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _patch(path: Path, before: str, after: str) -> None:
    text = path.read_text(encoding="utf-8")
    occurrences = text.count(before)
    if occurrences != 1:
        raise RuntimeError(
            f"Unsupported source layout in {path.name}: expected one patch "
            f"anchor, found {occurrences}; production files are unchanged"
        )
    path.write_text(text.replace(before, after, 1), encoding="utf-8")


def _instrument(source: Path) -> None:
    for relative in (
        "hashindex/timnah/timnah.cc",
        "hashindex/dash/dash.cc",
        "hashindex/chash/chash.cc",
        "hashindex/ehcache/ehcache.cc",
    ):
        path = source / relative
        text = path.read_text(encoding="utf-8")
        path.write_text('#include "figure11_observer.hpp"\n' + text,
                        encoding="utf-8")

    _patch(
        source / "hashindex/tier_budget/tiered_memory.h",
        "  Stats GetStats() const { return stats_; }",
        """  // Diagnostic copy only: classify the found payload, not metadata
  // size, key frequency or a logical bucket name. The fixed fast mapping is
  // immutable for this Memory object's lifetime.
  bool IsFast(const void* address) const noexcept {
    const auto pointer = reinterpret_cast<std::uintptr_t>(address);
    const auto start = reinterpret_cast<std::uintptr_t>(fast_.address);
    return fast_.address && pointer >= start && pointer - start < fast_.bytes;
  }

  Stats GetStats() const { return stats_; }""",
    )

    _patch(
        source / "hashindex/timnah/timnah.cc",
        """        if (state) LocalIncrement(state->fast_hits);
        value = fast.value;""",
        """        if (state) LocalIncrement(state->fast_hits);
        figure11::Record(true);
        value = fast.value;""",
    )
    _patch(
        source / "hashindex/timnah/timnah.cc",
        """        if (state) LocalIncrement(state->slow_hits);
        value = slow.value;""",
        """        if (state) LocalIncrement(state->slow_hits);
        figure11::Record(false);
        value = slow.value;""",
    )
    _patch(
        source / "hashindex/dash/dash.cc",
        """      if (loc.found) value = candidate;
      return loc.found ? Result::SUCCESS : Result::FAILED;""",
        """      if (loc.found) {
        figure11::Record(memory.IsFast(
            &s.buckets[loc.bucket].slots[loc.slot].value));
        value = candidate;
      }
      return loc.found ? Result::SUCCESS : Result::FAILED;""",
    )
    _patch(
        source / "hashindex/chash/chash.cc",
        """      if (loc.found) value = candidate;
      return loc.found ? Result::SUCCESS : Result::FAILED;""",
        """      if (loc.found) {
        figure11::Record(memory.IsFast(
            &e->GetBucket(loc.bucket).slots[loc.slot].value));
        value = candidate;
      }
      return loc.found ? Result::SUCCESS : Result::FAILED;""",
    )
    _patch(
        source / "hashindex/ehcache/ehcache.cc",
        """    if (!sampled && Optimistic(shard, local_hash, key, value)) {
      Increment(thread.counters->hits);
      return Result::SUCCESS;
    }""",
        """    if (!sampled && Optimistic(shard, local_hash, key, value)) {
      Increment(thread.counters->hits);
      figure11::Record(true);
      return Result::SUCCESS;
    }""",
    )
    _patch(
        source / "hashindex/ehcache/ehcache.cc",
        """      if (sampled) Count(*found.slot, *thread.counters);
      Increment(thread.counters->hits);
      return Result::SUCCESS;""",
        """      if (sampled) Count(*found.slot, *thread.counters);
      Increment(thread.counters->hits);
      figure11::Record(true);
      return Result::SUCCESS;""",
    )
    _patch(
        source / "hashindex/ehcache/ehcache.cc",
        """    const auto result = backend_->Search(key, value);
    if (result == Result::SUCCESS) {
      guard.BeginWrite();""",
        """    const auto result = backend_->Search(key, value);
    if (result == Result::SUCCESS) {
      figure11::Record(false);
      guard.BeginWrite();""",
    )


def prepare(repository: Path, source_dir: Path) -> dict:
    """Return a manifest after atomically installing a fresh diagnostic copy."""
    repository = Path(repository).resolve(strict=True)
    source_dir = Path(source_dir).resolve()
    if not (repository / "hashindex/timnah/timnah.cc").is_file():
        raise RuntimeError("The repository is not a supported Hashing1 tree")
    if source_dir == repository or repository.is_relative_to(source_dir):
        raise RuntimeError("The diagnostic copy cannot replace the repository")
    for protected in ("utils", "hashindex", "benchmark", "build"):
        if source_dir.is_relative_to(repository / protected):
            raise RuntimeError("The diagnostic copy must be outside production directories")
    if source_dir.exists() and any(source_dir.iterdir()):
        if not (source_dir / MARKER).is_file():
            raise RuntimeError("Refusing to replace an unrecognized source directory")

    source_dir.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=source_dir.name + ".preparing-",
                                      dir=source_dir.parent))
    try:
        production_hashes = {}
        for root in ROOTS:
            origin = repository / root
            if not origin.is_dir():
                raise RuntimeError(f"Required source directory is missing: {root}")
            for path in sorted(origin.rglob("*")):
                if not path.is_file() or path.suffix not in SUFFIXES:
                    continue
                if path.is_symlink():
                    raise RuntimeError(f"Unsupported symlink source: {path}")
                relative = path.relative_to(repository)
                contents = path.read_bytes()
                production_hashes[str(relative)] = hashlib.sha256(contents).hexdigest()
                destination = temporary / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(contents)

        observer = Path(__file__).resolve().with_name("observer.hpp")
        (temporary / "figure11_observer.hpp").write_bytes(observer.read_bytes())
        _instrument(temporary)

        # Fail if production sources were changed concurrently while copying.
        for relative, digest in production_hashes.items():
            if sha256(repository / relative) != digest:
                raise RuntimeError(f"Production source changed during snapshot: {relative}")

        copied_hashes = {
            str(path.relative_to(temporary)): sha256(path)
            for path in sorted(temporary.rglob("*")) if path.is_file()
        }
        manifest = {
            "schema": "figure11-diagnostic-source-v1",
            "repository": str(repository),
            "source_dir": str(source_dir),
            "production_sources_sha256": production_hashes,
            "diagnostic_sources_sha256": copied_hashes,
            "observer_sha256": sha256(observer),
            "metric": "successful Search payload tier; failures counted by driver",
            "production_files_modified": False,
        }
        (temporary / MARKER).write_text(json.dumps(manifest, indent=2) + "\n",
                                       encoding="utf-8")
        if source_dir.exists():
            shutil.rmtree(source_dir)
        temporary.rename(source_dir)
        return manifest
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    parser.add_argument("source_dir", type=Path)
    args = parser.parse_args()
    manifest = prepare(args.repository, args.source_dir)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
