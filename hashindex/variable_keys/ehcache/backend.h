#pragma once

#include "utils/common.hpp"
#include "../timnah/key_codec.h"
#include "hashindex/tier_budget/tiered_memory.h"

namespace EHCacheNameSpace {

// Volatile CCEH-style backing table. The bounded probe window and segment
// directory are independent of the record cache. Persistence is deliberately
// absent, as in the paper's DRAM/CXL evaluation of EH+cache.
class Backend {
 public:
  explicit Backend(TierBudget::Memory& memory, u8 initial_depth)
      : memory_(memory) {
    if (initial_depth < 1 || initial_depth > kMaxDepth)
      throw std::invalid_argument("EHCache initial depth must be in [1,24]");
    try {
      auto* directory = NewDirectory(initial_depth);
      for (u32 i = 0; i < directory->size; ++i)
        directory->entries[i].store(NewEntry(initial_depth));
      current_.store(directory);
    } catch (...) {
      Cleanup();
      throw;
    }
  }

  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  // Destruction, like Memory destruction, requires all callers to be quiescent.
  ~Backend() { Cleanup(); }

  Result Insert(u64 key, u64 value) { return Mutate(key, value, Operation::Insert); }
  Result Update(u64 key, u64 value) { return Mutate(key, value, Operation::Update); }
  Result Remove(u64 key) { return Mutate(key, 0, Operation::Remove); }

  Result Search(u64 key, u64& value, u64* stored_key = nullptr) const {
    const u64 hash = TimnahNameSpace::detail::KeyCodec::Hash(key);
    for (;;) {
      Entry* entry = current_.load()->Get(hash);
      const u64 version = entry->version.load();
      if (version & 1) {
        std::this_thread::yield();
        continue;
      }
      u64 candidate{};
      const Location found = Find(*entry, hash, key, &candidate);
      const u64 canonical = found.found
          ? Read(entry->segment->buckets[found.bucket].slots[found.slot].key) : 0;
      if (version != entry->version.load() || !Routed(entry, hash)) continue;
      if (!found.found) return Result::FAILED;
      if (stored_key) *stored_key = canonical;
      value = candidate;
      return Result::SUCCESS;
    }
  }

  size_t Size() const { return size_.load(std::memory_order_relaxed); }

  std::string Describe() const {
    std::lock_guard lock(split_mutex_);
    size_t directory_bytes{};
    for (auto* directory = directories_; directory; directory = directory->older)
      directory_bytes += sizeof(Directory) +
                         sizeof(std::atomic<Entry*>) * directory->size;
    std::ostringstream out;
    out << "eh_size=" << Size() << " eh_segments=" << entry_count_
        << " eh_depth=" << static_cast<unsigned>(current_.load()->depth)
        << " eh_splits=" << split_count_
        << " eh_bucket_bytes=" << sizeof(Bucket)
        << " eh_segment_bytes=" << sizeof(Segment)
        << " eh_probe_buckets=" << kProbeBuckets
        << " eh_directory_bytes=" << directory_bytes
        << " eh_entry_metadata_bytes=" << entry_count_ * sizeof(Entry)
        << " eh_load_factor="
        << static_cast<double>(Size()) / (entry_count_ * kBuckets * kSlots);
    return out.str();
  }

 private:
  static constexpr u32 kSlots = 16;
  static constexpr u32 kBuckets = 64;
  static constexpr u32 kProbeBuckets = 4;
  static constexpr u16 kFull = std::numeric_limits<u16>::max();
  static constexpr u8 kMaxDepth = 24;

  // Every word that optimistic readers can access concurrently with a writer
  // uses atomic_ref. A version check alone would not make plain C++ loads safe.
  // Sequential consistency also orders payload accesses against version checks.
  template <typename T>
  static T Read(const T& value) {
    return std::atomic_ref<T>(const_cast<T&>(value)).load();
  }
  template <typename T>
  static void Write(T& target, T value) {
    std::atomic_ref<T>(target).store(value);
  }

  struct Slot { u64 key{}, value{}; };
  struct alignas(64) Bucket { Slot slots[kSlots]{}; };
  struct alignas(64) Segment { Bucket buckets[kBuckets]{}; };
  static_assert(sizeof(Bucket) == 256);
  static_assert(sizeof(Segment) == 16384);

  struct alignas(64) Entry {
    std::atomic<u64> version{0};
    Segment* segment{};
    Entry* next{};
    u8 local_depth{};
    u16 occupied[kBuckets]{};

    void Lock() {
      for (;;) {
        u64 version_now = version.load();
        if (!(version_now & 1) &&
            version.compare_exchange_weak(version_now, version_now + 1)) return;
        std::this_thread::yield();
      }
    }
    void Unlock() { version.fetch_add(1); }
  };

  struct Guard {
    Entry& entry;
    explicit Guard(Entry& e) : entry(e) { entry.Lock(); }
    ~Guard() { entry.Unlock(); }
  };

  struct Directory {
    const u8 depth;
    const u32 size;
    std::atomic<Entry*>* const entries;
    Directory* const older;

    Directory(u8 d, std::atomic<Entry*>* array, Directory* previous)
        : depth(d), size(u32{1} << d), entries(array), older(previous) {}
    Entry* Get(u64 hash) const {
      return entries[static_cast<u32>(hash >> (64 - depth))].load();
    }
  };

  struct Location {
    u32 bucket{}, slot{};
    bool found{};
  };
  enum class Operation { Insert, Update, Remove };

  static Location Find(const Entry& entry, u64 hash, u64 key,
                       u64* value = nullptr) {
    const u32 start = static_cast<u32>(hash) & (kBuckets - 1);
    for (u32 offset = 0; offset < kProbeBuckets; ++offset) {
      const u32 bucket = (start + offset) & (kBuckets - 1);
      u16 mask = Read(entry.occupied[bucket]);
      while (mask) {
        const u32 slot = std::countr_zero(mask);
        mask = static_cast<u16>(mask & (mask - 1));
        const Slot& record = entry.segment->buckets[bucket].slots[slot];
        if (TimnahNameSpace::detail::KeyCodec::Equal(Read(record.key), key)) {
          if (value) *value = Read(record.value);
          return {bucket, slot, true};
        }
      }
    }
    return {};
  }

  static bool Put(Entry& entry, u64 hash, u64 key, u64 value) {
    const u32 start = static_cast<u32>(hash) & (kBuckets - 1);
    for (u32 offset = 0; offset < kProbeBuckets; ++offset) {
      const u32 bucket = (start + offset) & (kBuckets - 1);
      const u16 mask = Read(entry.occupied[bucket]);
      if (mask == kFull) continue;
      const u32 slot = std::countr_zero(static_cast<u16>(~mask));
      Slot& record = entry.segment->buckets[bucket].slots[slot];
      Write(record.key, key);
      Write(record.value, value);
      Write(entry.occupied[bucket], static_cast<u16>(mask | (u16{1} << slot)));
      return true;
    }
    return false;
  }

  // Allocation only occurs during construction or with split_mutex_ held.
  // All directory generations and ownership links are charged to Memory;
  // backing records always use the slow tier, including initial segments.
  Directory* NewDirectory(u8 depth) {
    const u32 count = u32{1} << depth;
    auto* array = static_cast<std::atomic<Entry*>*>(
        memory_.Metadata(sizeof(std::atomic<Entry*>) * count));
    for (u32 i = 0; i < count; ++i) std::construct_at(array + i, nullptr);
    auto* result = std::construct_at(
        static_cast<Directory*>(memory_.Metadata(sizeof(Directory))),
        depth, array, directories_);
    directories_ = result;
    return result;
  }

  Entry* NewEntry(u8 depth) {
    auto* result = std::construct_at(
        static_cast<Entry*>(memory_.Metadata(sizeof(Entry))));
    result->local_depth = depth;
    try {
      result->segment = std::construct_at(
          static_cast<Segment*>(memory_.SplitKV(sizeof(Segment))));
    } catch (...) {
      std::destroy_at(result);
      throw;
    }
    result->next = entries_;
    entries_ = result;
    ++entry_count_;
    return result;
  }

  bool Routed(const Entry* entry, u64 hash) const {
    return current_.load()->Get(hash) == entry;
  }

  bool Split(Entry& left, u64 hash) {
    // The caller already holds left's version lock. Other segments can still
    // be read or written while directory expansion is serialized here.
    std::lock_guard lock(split_mutex_);
    Directory* old_directory = current_.load();
    const u8 old_depth = left.local_depth;
    if (old_depth == kMaxDepth) return false;

    Directory* directory = old_directory;
    if (old_depth == old_directory->depth) {
      directory = NewDirectory(old_directory->depth + 1);
      for (u32 i = 0; i < old_directory->size; ++i) {
        Entry* entry = old_directory->entries[i].load();
        directory->entries[2 * i].store(entry);
        directory->entries[2 * i + 1].store(entry);
      }
    }
    Entry* right = NewEntry(old_depth + 1);

    // No allocations follow this point. Each child retains the original
    // bucket/slot positions, so bounded-probe reachability is preserved even
    // when a probe window wraps around the end of a segment.
    for (u32 bucket = 0; bucket < kBuckets; ++bucket) {
      const u16 original = Read(left.occupied[bucket]);
      u16 moved{};
      u16 mask = original;
      while (mask) {
        const u32 slot = std::countr_zero(mask);
        mask = static_cast<u16>(mask & (mask - 1));
        const Slot& from = left.segment->buckets[bucket].slots[slot];
        const u64 key = Read(from.key);
        if ((TimnahNameSpace::detail::KeyCodec::Hash(key) >> (63 - old_depth)) & 1) {
          Slot& to = right->segment->buckets[bucket].slots[slot];
          Write(to.key, key);
          Write(to.value, Read(from.value));
          moved = static_cast<u16>(moved | (u16{1} << slot));
        }
      }
      Write(right->occupied[bucket], moved);
      Write(left.occupied[bucket], static_cast<u16>(original & ~moved));
    }
    left.local_depth = old_depth + 1;

    const u32 prefix = static_cast<u32>(hash >> (64 - old_depth));
    const u32 base = prefix << (directory->depth - old_depth);
    const u32 half = u32{1} << (directory->depth - old_depth - 1);
    for (u32 i = base + half; i < base + 2 * half; ++i)
      directory->entries[i].store(right);
    if (directory != old_directory) current_.store(directory);
    ++split_count_;
    return true;
  }

  Result Mutate(u64 key, u64 value, Operation operation) {
    const u64 hash = TimnahNameSpace::detail::KeyCodec::Hash(key);
    for (;;) {
      Entry* entry = current_.load()->Get(hash);
      Guard guard(*entry);
      if (!Routed(entry, hash)) continue;
      const Location found = Find(*entry, hash, key);
      if (operation == Operation::Insert) {
        if (found.found) return Result::FAILED;
        if (!Put(*entry, hash, key, value)) {
          if (!Split(*entry, hash)) return Result::FULL;
          continue;
        }
        size_.fetch_add(1, std::memory_order_relaxed);
      } else {
        if (!found.found) return Result::FAILED;
        if (operation == Operation::Update) {
          Write(entry->segment->buckets[found.bucket].slots[found.slot].value, value);
        } else {
          const u16 mask = Read(entry->occupied[found.bucket]);
          Write(entry->occupied[found.bucket],
                static_cast<u16>(mask & ~(u16{1} << found.slot)));
          size_.fetch_sub(1, std::memory_order_relaxed);
        }
      }
      return Result::SUCCESS;
    }
  }

  void Cleanup() {
    while (entries_) {
      Entry* next = entries_->next;
      std::destroy_at(entries_->segment);
      std::destroy_at(entries_);
      entries_ = next;
    }
    while (directories_) {
      Directory* older = directories_->older;
      for (u32 i = 0; i < directories_->size; ++i)
        std::destroy_at(directories_->entries + i);
      std::destroy_at(directories_);
      directories_ = older;
    }
  }

  TierBudget::Memory& memory_;
  std::atomic<Directory*> current_{nullptr};
  mutable std::mutex split_mutex_;
  Directory* directories_{};
  Entry* entries_{};
  size_t entry_count_{};
  size_t split_count_{};
  std::atomic<size_t> size_{0};
};

}  // namespace EHCacheNameSpace
