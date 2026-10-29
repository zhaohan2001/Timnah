#!/usr/bin/env python3
"""Snapshot benchmark sources for an isolated Figure 12 measurement build.

The probe dynamically links the existing production index and utility libraries.
It never compiles, patches, rebuilds, or replaces an index implementation.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile


MARKER = ".figure12-diagnostic-copy.json"
ROOTS = (
    "utils",
    "benchmark/ycsb/modules",
    "benchmark/synthetic",
    "benchmark/twitter",
    "benchmark/webmail",
)
SUFFIXES = {".h", ".hpp", ".cpp", ".cc"}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(repository: Path, source_dir: Path) -> dict:
    """Atomically install byte-identical benchmark copies and a hash manifest."""
    repository = Path(repository).resolve(strict=True)
    source_dir = Path(source_dir).resolve()
    if not (repository / "hashindex/hashindex.cpp").is_file():
        raise RuntimeError("The repository is not a supported Hashing1 tree")
    for relative in ("build/hashindex/libhashindex.so", "build/utils/libutils.so"):
        if not (repository / relative).is_file():
            raise RuntimeError(f"An existing production library is missing: {relative}")
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

        for relative, digest in production_hashes.items():
            if sha256(repository / relative) != digest:
                raise RuntimeError(f"Production source changed during snapshot: {relative}")
        copied_hashes = {
            str(path.relative_to(temporary)): sha256(path)
            for path in sorted(temporary.rglob("*")) if path.is_file()
        }
        manifest = {
            "schema": "figure12-diagnostic-source-v1",
            "repository": str(repository),
            "source_dir": str(source_dir),
            "production_sources_sha256": production_hashes,
            "diagnostic_sources_sha256": copied_hashes,
            "index_implementation": "existing unchanged production shared libraries",
            "source_patches": [],
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
    print(json.dumps(prepare(args.repository, args.source_dir), indent=2))


if __name__ == "__main__":
    main()
