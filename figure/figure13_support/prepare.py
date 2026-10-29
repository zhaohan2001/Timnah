#!/usr/bin/env python3
"""Build isolated Figure 13 source snapshots without editing production files."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

MARKER = ".figure13-diagnostic-copy.json"
ROOTS = ("utils", "hashindex/timnah", "benchmark/ycsb/modules",
         "benchmark/synthetic", "benchmark/twitter", "benchmark/webmail")
SUFFIXES = {".h", ".hpp", ".cpp", ".cc"}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def patch(path, before, after, expected=1):
    data = path.read_text()
    count = data.count(before)
    if count != expected:
        raise RuntimeError(f"Unsupported source anchor in {path.name}: {count}, expected {expected}")
    path.write_text(data.replace(before, after))


def eh_only(path, original, begin, end):
    """Retain the exact original declarations/definitions for every tiered case.

    Only flat EH needs the allocation quota extension. Hiding its fields from
    the other variants also preserves their Arena/Tier sizes and member offsets,
    so Hot Lock is not confounded with an unrelated allocator object layout.
    """
    current = path.read_text()
    original_first, original_last = original.index(begin), original.index(end)
    current_first, current_last = current.index(begin), current.index(end)
    before = original[original_first:original_last]
    after = current[current_first:current_last]
    if before == after:
        raise RuntimeError(f"Expected an EH-only extension in {path.name}: {begin}")
    # Store the original text literally in the non-EH branch. This invariant is
    # independent of platform-specific sizeof(std::mutex) or alignment details.
    guarded = ("#if FIGURE13_VARIANT == 0\n" + after +
               "#else\n" + before + "#endif\n\n")
    patch(path, after, guarded)
    original_offset = len("#if FIGURE13_VARIANT == 0\n") + len(after) + len("#else\n")
    if guarded[original_offset:original_offset + len(before)] != before:
        raise RuntimeError("The non-EH original-source invariant failed")


def ablate(source):
    """Patch only a second copy; full Hot Lock compiles untouched Timnah files."""
    original = source / "hashindex/timnah"
    target = source / "hashindex/timnah_ablation"
    shutil.copytree(original, target)
    storage_h, storage_cc = target / "storage.h", target / "storage.cc"
    timnah_cc = target / "timnah.cc"
    original_storage_h = storage_h.read_text()
    original_storage_cc = storage_cc.read_text()
    # A flat EH has one directory, while its segment allocations fill exactly
    # the same fast bucket/counter capacities as the current two-table Timnah.
    patch(storage_h,
          "bool prefault = true, bool huge_pages = true);",
          "bool prefault = true, bool huge_pages = true,\n"
          "                 std::size_t primary_limit = SIZE_MAX, int fallback_node = -1);")
    patch(storage_h, "  int node_;\n  std::size_t block_bytes_;",
          "  int node_;\n  std::size_t primary_limit_;\n  int fallback_node_;\n"
          "  std::size_t block_bytes_;")
    patch(storage_h,
          "std::function<u64(u64)> hash_fn, bool huge_pages = true);",
          "std::function<u64(u64)> hash_fn, bool huge_pages = true, bool flat_mixed = false);")
    patch(storage_cc,
          "Arena::Arena(int node, std::size_t block_bytes, bool prefault, bool huge_pages)\n"
          "    : node_(node), block_bytes_(block_bytes), prefault_(prefault), huge_pages_(huge_pages) {",
          "Arena::Arena(int node, std::size_t block_bytes, bool prefault, bool huge_pages,\n"
          "             std::size_t primary_limit, int fallback_node)\n"
          "    : node_(node), primary_limit_(primary_limit), fallback_node_(fallback_node),\n"
          "      block_bytes_(block_bytes), prefault_(prefault), huge_pages_(huge_pages) {")
    patch(storage_cc, "    if (node_ >= 0) {",
          "    const int allocation_node = reserved_.load(std::memory_order_relaxed) < primary_limit_\n"
          "        ? node_ : fallback_node_;\n"
          "    if (allocation_node >= 0) {")
    patch(storage_cc, "static_cast<std::size_t>(node_)", "static_cast<std::size_t>(allocation_node)", 3)
    patch(storage_cc, '"Timnah mbind node " + std::to_string(node_)',
          '"Timnah mbind node " + std::to_string(allocation_node)')
    patch(storage_cc,
          "std::function<u64(u64)> hash_fn, bool huge_pages)\n",
          "std::function<u64(u64)> hash_fn, bool huge_pages, bool flat_mixed)\n")
    patch(storage_cc,
          "      buckets_(node, std::size_t{64} << 20, prefault, huge_pages),\n"
          "      counters_(node, std::size_t{64} << 20, prefault, huge_pages),",
          "      buckets_(flat_mixed ? 0 : node, std::size_t{64} << 20, prefault, huge_pages,\n"
          "               flat_mixed ? std::size_t{256} << 20 : SIZE_MAX, node),\n"
          "      counters_(flat_mixed ? 0 : node, std::size_t{64} << 20, prefault, huge_pages,\n"
          "                flat_mixed ? std::size_t{64} << 20 : SIZE_MAX, node),")
    # The remaining four variants must not inherit flat EH's extra allocator
    # fields, constructor arguments, branches, or shifted Tier member offsets.
    # Whole declarations/functions retain their exact original non-EH tokens.
    eh_only(storage_h, original_storage_h, "class Arena {", "struct alignas(64) Entry {")
    eh_only(storage_h, original_storage_h, "  Tier(int node,", "  ~Tier();")
    eh_only(storage_cc, original_storage_cc, "Arena::Arena(", "Arena::~Arena()")
    eh_only(storage_cc, original_storage_cc, "void* Arena::Allocate(", "Tier::Directory::Directory(")
    eh_only(storage_cc, original_storage_cc, "Tier::Tier(", "Tier::~Tier()")
    # Version Lock variants use key_state for all writes. There is no unrelated
    # Value/Hot word traffic in their structural path or split draining.
    patch(storage_cc,
          "    while (!bucket.TryValueLock()) std::this_thread::yield();\n"
          "    while (!bucket.TryHotLock()) std::this_thread::yield();\n"
          "    item.hot = true;",
          "#if FIGURE13_VARIANT >= 2\n"
          "    while (!bucket.TryValueLock()) std::this_thread::yield();\n"
          "#endif\n"
          "#if FIGURE13_VARIANT >= 4\n"
          "    while (!bucket.TryHotLock()) std::this_thread::yield();\n"
          "    item.hot = true;\n"
          "#endif")
    patch(storage_cc, "      item.bucket->ValueUnlock();",
          "#if FIGURE13_VARIANT >= 2\n      item.bucket->ValueUnlock();\n#endif")

    patch(timnah_cc,
          "    fast_ = std::make_unique<Tier>(options_.fast_node, options_.fast_depth, false, options_.prefault, hash, options_.huge_pages);\n"
          "    slow_ = std::make_unique<Tier>(options_.slow_node, options_.slow_depth, true, options_.prefault, hash, options_.huge_pages);",
          "#if FIGURE13_VARIANT == 0\n"
          "    slow_ = std::make_unique<Tier>(options_.slow_node, options_.slow_depth, true, options_.prefault, hash, options_.huge_pages, true);\n"
          "#else\n"
          "    fast_ = std::make_unique<Tier>(options_.fast_node, options_.fast_depth, false, options_.prefault, hash, options_.huge_pages);\n"
          "    slow_ = std::make_unique<Tier>(options_.slow_node, options_.slow_depth, true, options_.prefault, hash, options_.huge_pages);\n"
          "#endif")
    patch(timnah_cc, "  bool Tracking() const {\n",
          "  bool Tracking() const {\n#if FIGURE13_VARIANT < 3\n    return false;\n#endif\n")
    patch(timnah_cc,
          "    for (u32 i = 0; i < count_; ++i) {\n"
          "      if (!items_[i].bucket->TryValueLock()) return false;\n"
          "      items_[i].value = true;\n    }",
          "#if FIGURE13_VARIANT >= 2\n"
          "    for (u32 i = 0; i < count_; ++i) {\n"
          "      if (!items_[i].bucket->TryValueLock()) return false;\n"
          "      items_[i].value = true;\n    }\n#endif")
    patch(timnah_cc, "    for (u32 i = 0; i < count_; ++i) if (items_[i].want_hot) {",
          "    for (u32 i = 0; i < count_; ++i) if (FIGURE13_VARIANT >= 4 && items_[i].want_hot) {")
    patch(timnah_cc,
          "      const auto& word = fast && slot == 0 ? bucket.hot_version : bucket.value_version;\n"
          "      const auto value_version = Read(word);\n"
          "      if (value_version & 1) return result;\n"
          "      result.value = Read(bucket.slots[slot].value);\n"
          "      if (value_version != Read(word)) return result;",
          "#if FIGURE13_VARIANT >= 2\n"
          "      const auto& word = FIGURE13_VARIANT >= 4 && fast && slot == 0 ? bucket.hot_version : bucket.value_version;\n"
          "      const auto value_version = Read(word);\n"
          "      if (value_version & 1) return result;\n"
          "#endif\n"
          "      result.value = Read(bucket.slots[slot].value);\n"
          "#if FIGURE13_VARIANT >= 2\n"
          "      if (value_version != Read(word)) return result;\n"
          "#endif")
    patch(timnah_cc, "  auto Search(u64 key, u64& value) -> Result {\n",
          "  auto Search(u64 key, u64& value) -> Result {\n"
          "#if FIGURE13_VARIANT == 0\n    return FlatSearch(key, value);\n#endif\n")
    patch(timnah_cc, "  auto Insert(u64 key, u64 value) -> Result {\n",
          "  auto Insert(u64 key, u64 value) -> Result {\n"
          "#if FIGURE13_VARIANT == 0\n    return FlatInsert(key, value);\n#endif\n")
    patch(timnah_cc, "  auto Remove(u64 key) -> Result {\n",
          "  auto Remove(u64 key) -> Result {\n"
          "#if FIGURE13_VARIANT == 0\n    return FlatRemove(key);\n#endif\n")
    patch(timnah_cc, "  auto Update(u64 key, u64 value) -> Result {\n",
          "  auto Update(u64 key, u64 value) -> Result {\n"
          "#if FIGURE13_VARIANT == 0\n    return FlatUpdate(key, value);\n"
          "#elif FIGURE13_VARIANT == 1\n    return VersionUpdate(key, value);\n#endif\n")
    patch(timnah_cc, "in_fast && found.slot == 0))", "FIGURE13_VARIANT >= 4 && in_fast && found.slot == 0))")
    patch(timnah_cc, "  Options options_;\n", '#include "figure13_flat_methods.inc"\n\n  Options options_;\n')
    patch(timnah_cc,
          "    result.fast_segments = fast_->SegmentCount(); result.slow_segments = slow_->SegmentCount();\n"
          "    result.fast_bucket_bytes = fast_->BucketBytes(); result.fast_counter_bytes = fast_->CounterBytes();\n"
          "    result.slow_bucket_bytes = slow_->BucketBytes(); result.slow_counter_bytes = slow_->CounterBytes();",
          "#if FIGURE13_VARIANT == 0\n"
          "    result.fast_bucket_bytes = std::min(slow_->BucketBytes(), std::size_t{256} << 20);\n"
          "    result.fast_counter_bytes = std::min(slow_->CounterBytes(), std::size_t{64} << 20);\n"
          "    result.slow_bucket_bytes = slow_->BucketBytes() - result.fast_bucket_bytes;\n"
          "    result.slow_counter_bytes = slow_->CounterBytes() - result.fast_counter_bytes;\n"
          "    result.fast_segments = result.fast_bucket_bytes / (sizeof(Bucket) * kBuckets);\n"
          "    result.slow_segments = slow_->SegmentCount() - result.fast_segments;\n"
          "#else\n"
          "    result.fast_segments = fast_->SegmentCount(); result.slow_segments = slow_->SegmentCount();\n"
          "    result.fast_bucket_bytes = fast_->BucketBytes(); result.fast_counter_bytes = fast_->CounterBytes();\n"
          "    result.slow_bucket_bytes = slow_->BucketBytes(); result.slow_counter_bytes = slow_->CounterBytes();\n"
          "#endif")
    # Quiescent validation must support the missing second table in flat EH.
    patch(timnah_cc,
          "for (auto* table : {fast_.get(), slow_.get()}) for (auto* entry : table->EntriesSnapshot()) {",
          "for (auto* table : {fast_.get(), slow_.get()}) if (table) for (auto* entry : table->EntriesSnapshot()) {")
    patch(timnah_cc,
          "    for (int tier = 0; tier < 2; ++tier) {\n      auto& table = tier == 0 ? *fast_ : *slow_;",
          "    for (int tier = 0; tier < 2; ++tier) {\n"
          "      if (tier == 0 && !fast_) continue;\n"
          "      auto& table = tier == 0 ? *fast_ : *slow_;")
    patch(timnah_cc, "    for (auto* table : {fast_.get(), slow_.get()}) {\n      auto route = table->Locate(hash);",
          "    for (auto* table : {fast_.get(), slow_.get()}) {\n"
          "      if (!table) continue;\n      auto route = table->Locate(hash);")


def prepare(repository, source_dir):
    repository = Path(repository).resolve(strict=True)
    source_dir = Path(source_dir).resolve()
    if not (repository / "hashindex/timnah/timnah.cc").is_file():
        raise RuntimeError("Unsupported Hashing1 repository")
    if source_dir == repository or repository.is_relative_to(source_dir):
        raise RuntimeError("The isolated snapshot cannot replace the repository")
    if any(source_dir.is_relative_to(repository / root) for root in ("utils", "hashindex", "benchmark", "build")):
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
        flat = Path(__file__).with_name("flat_methods.inc")
        (temporary / "figure13_flat_methods.inc").write_bytes(flat.read_bytes())
        ablate(temporary)
        for relative, digest in original.items():
            if sha256(repository / relative) != digest:
                raise RuntimeError(f"Production source changed during snapshot: {relative}")
        copied = {str(p.relative_to(temporary)): sha256(p) for p in sorted(temporary.rglob("*")) if p.is_file()}
        assumptions = {
            "EH": "One growable directory; first 4096 segments on node 0, remaining segments on node 2; key-state version lock protects all writes",
            "Tiered EH": "Two tables, fixed DRAM plus growable CXL; key-state version lock protects all writes",
            "+K/V Lock": "Independent structural key lock and bucket-wide value lock; hot slot uses the same value lock; no heat or migration",
            "+Data Migration": "Adds original sampling, decay, cross-tier promotion/demotion, intra-bucket reorder, convergence; no separate hot lock",
            "+Hot Lock": "Byte-identical current Timnah source: independent hot-slot value lock plus all prior features",
            "memory": "Match current Timnah: node 0 bucket capacity 256 MiB plus counters 64 MiB; metadata separately on node 0; no claim of 256 MiB total",
            "convergence": "Preserve current default 0.002 for static YCSB; existing dynamic presets disable convergence for Twitter/Webmail",
            "allocator_control": "Flat-EH-only allocator fields, parameters and allocation paths are compiled only for variant 0; all tiered variants retain the exact original Arena class and Arena/Tier constructor/allocation definitions",
        }
        manifest = {"schema": "figure13-diagnostic-source-v1", "repository": str(repository),
                    "source_dir": str(source_dir), "production_sources_sha256": original,
                    "diagnostic_sources_sha256": copied, "mapping_assumptions": assumptions,
                    "generator_support_sha256": {
                        "prepare.py": sha256(Path(__file__).resolve()),
                        "flat_methods.inc": sha256(flat),
                    },
                    "production_files_modified": False, "full_hot_source_modified": False}
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
