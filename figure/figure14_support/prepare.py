#!/usr/bin/env python3
"""Copy unchanged production sources for the isolated Figure 14 capacity study."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

MARKER = ".figure14-diagnostic-copy.json"
ROOTS = ("utils", "hashindex/timnah", "hashindex/chash", "hashindex/tier_budget", "benchmark/ycsb/modules",
         "benchmark/synthetic", "benchmark/twitter", "benchmark/webmail")
SUFFIXES = {".h", ".hpp", ".cpp", ".cc"}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def prepare(repository, source_dir):
    repository = Path(repository).resolve(strict=True)
    source_dir = Path(source_dir).resolve()
    if not (repository / "hashindex/timnah/timnah.cc").is_file():
        raise RuntimeError("Unsupported Hashing1 repository")
    if source_dir == repository or repository.is_relative_to(source_dir):
        raise RuntimeError("The isolated snapshot cannot replace the repository")
    if any(source_dir.is_relative_to(repository / root)
           for root in ("utils", "hashindex", "benchmark", "build")):
        raise RuntimeError("The isolated snapshot cannot be in production directories")
    if source_dir.exists() and any(source_dir.iterdir()) and not (source_dir / MARKER).is_file():
        raise RuntimeError("Refusing to replace an unrecognized source directory")
    source_dir.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=source_dir.name + ".preparing-", dir=source_dir.parent))
    try:
        original = {}
        for root in ROOTS:
            if not (repository / root).is_dir():
                raise RuntimeError(f"Required source directory is missing: {root}")
            for origin in sorted((repository / root).rglob("*")):
                if not origin.is_file() or origin.suffix not in SUFFIXES:
                    continue
                if origin.is_symlink():
                    raise RuntimeError(f"Unsupported source symlink: {origin}")
                relative = origin.relative_to(repository)
                destination = temporary / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(origin.read_bytes())
                original[str(relative)] = sha256(origin)
        for relative, digest in original.items():
            if sha256(repository / relative) != digest:
                raise RuntimeError(f"Production source changed during snapshot: {relative}")
            if sha256(temporary / relative) != digest:
                raise RuntimeError(f"Source copy changed: {relative}")
        copied = {str(path.relative_to(temporary)): sha256(path)
                  for path in sorted(temporary.rglob("*")) if path.is_file()}
        if copied != original:
            raise RuntimeError("The Figure 14 source snapshot is not byte-identical")
        manifest = {
            "schema": "figure14-diagnostic-source-v1",
            "repository": str(repository), "source_dir": str(source_dir),
            "production_sources_sha256": original,
            "diagnostic_sources_sha256": copied,
            "generator_support_sha256": {"prepare.py": sha256(Path(__file__).resolve())},
            "production_files_modified": False,
            "timnah_source_modified": False,
            "chash_source_modified": False,
            "parameterization": "Existing timnah_fast_depth startup flag and unmodified CHash defaults; no algorithm or object-layout patches",
        }
        (temporary / MARKER).write_text(json.dumps(manifest, indent=2) + "\n")
        if source_dir.exists():
            shutil.rmtree(source_dir)
        temporary.rename(source_dir)
        return manifest
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    parser.add_argument("source_dir", type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.repository, args.source_dir), indent=2))
