#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace TimnahNameSpace::detail {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr u32 kSlots = 14;
inline constexpr u32 kBuckets = 256;
inline constexpr u32 kFullMask = (u32{1} << kSlots) - 1;
inline constexpr u64 kCounterBusy = u64{1} << 63;

// Conservative snapshots are deliberately used for the speculative KV path.
// Approximate counters use separate relaxed accesses in the policy layer.
template <class T> inline T Read(const T& value) {
  return std::atomic_ref<T>(const_cast<T&>(value)).load(std::memory_order_seq_cst);
}
template <class T, class U> inline void Write(T& target, U value) {
  std::atomic_ref<T>(target).store(static_cast<T>(value), std::memory_order_seq_cst);
}

struct Slot { u64 key{}, value{}; };
struct alignas(64) CounterBucket {
  u32 counts[kSlots]{};
  u64 epoch{};
};
static_assert(sizeof(CounterBucket) == 64);

// Occupancy is published in the same atomic word as the Key Lock version.
// All payload writes require Key Lock, except in-place values protected by
// Value/Hot Lock and validated against the unchanged Key Lock snapshot.
struct alignas(64) Bucket {
  static constexpr u64 kVersionMask = (u64{1} << 50) - 1;
  static constexpr unsigned kMaskShift = 50;
  u64 key_state{};
  u64 value_version{};
  u64 hot_version{};
  CounterBucket* counters{};
  Slot slots[kSlots]{};

  u64 KeyState() const { return Read(key_state); }
  static u32 Mask(u64 state) { return static_cast<u32>(state >> kMaskShift); }
  u32 Mask() const { return Mask(KeyState()); }
  void SetMask(u32 mask);
  bool TryKeyLock();
  void KeyUnlock();
  bool TryValueLock();
  void ValueUnlock();
  bool TryHotLock();
  void HotUnlock();
};
static_assert(sizeof(Bucket) == 256);
static_assert(offsetof(Bucket, slots) == 32);

class Arena {
 public:
  explicit Arena(int node, std::size_t block_bytes = std::size_t{64} << 20,
                 bool prefault = true, bool huge_pages = true);
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  void* Allocate(std::size_t bytes, std::size_t alignment = 64);
  std::size_t UsedBytes() const noexcept { return used_.load(std::memory_order_relaxed); }
  std::size_t ReservedBytes() const noexcept { return reserved_.load(std::memory_order_relaxed); }
  int Node() const noexcept { return node_; }

 private:
  struct Block;
  int node_;
  std::size_t block_bytes_;
  std::size_t page_size_;
  bool prefault_;
  bool huge_pages_;
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<Block>> blocks_;
  std::atomic<std::size_t> used_{0}, reserved_{0};
};

struct alignas(64) Entry {
  Bucket* buckets{};
  std::atomic<u8> depth{};
  // A splitter gates new routes before draining existing bucket writers.
  // Entry remains allocated until Tier destruction, so retrying readers can
  // safely observe the gate and reload the published directory.
  std::atomic<bool> splitting{false};
};
struct Route {
  Entry* entry{};
  Bucket* bucket{};
  u8 depth{};
  u32 bucket_index{};
};

// Tier owns every segment and directory generation until quiescent destruction.
// Valid() must be paired with a stable Key Lock snapshot by the caller.
class Tier {
 public:
  Tier(int node, u8 initial_depth, bool growable, bool prefault,
       std::function<u64(u64)> hash_fn, bool huge_pages = true);
  ~Tier();
  Tier(const Tier&) = delete;
  Tier& operator=(const Tier&) = delete;
  // Locate waits for an active split; callers must not own any bucket locks.
  // A previously obtained route can become gated, in which case Valid is false
  // and its caller must release its complete try-lock set before retrying.
  Route Locate(u64 hash) const;
  bool Valid(const Route& route, u64 hash) const;
  // The caller must release all bucket locks before requesting a split.
  bool Split(u64 hash, Entry* expected, u64 current_epoch, double decay_factor);
  std::vector<Entry*> EntriesSnapshot() const;
  std::size_t SegmentCount() const noexcept { return segment_count_.load(std::memory_order_relaxed); }
  u8 DirectoryDepth() const;
  u64 SplitCount() const noexcept { return splits_.load(std::memory_order_relaxed); }
  std::size_t BucketBytes() const noexcept { return buckets_.UsedBytes(); }
  std::size_t CounterBytes() const noexcept { return counters_.UsedBytes(); }
  std::size_t MetadataBytes() const noexcept { return metadata_.UsedBytes(); }
  std::size_t ReservedBytes() const noexcept {
    return buckets_.ReservedBytes() + counters_.ReservedBytes() + metadata_.ReservedBytes();
  }
  int Node() const noexcept { return node_; }

 private:
  struct Directory {
    u8 depth{};
    u32 size{};
    std::atomic<Entry*>* entries{};
    Directory(u8 depth, Arena& memory);
    ~Directory();
    Entry* Get(u64 hash) const {
      return entries[hash >> (64 - depth)].load(std::memory_order_seq_cst);
    }
  };
  Directory* NewDirectory(u8 depth);
  Entry* NewEntry(u8 depth, u64 epoch = 0, Bucket* memory = nullptr,
                  CounterBucket* counts = nullptr);
  Entry* Get(u64 hash) const;

  int node_;
  bool growable_;
  u8 initial_depth_;
  // Only the immutable tier uses a single contiguous bucket allocation.
  // Routes can calculate its bucket independently of the Entry lookup.
  Bucket* fixed_buckets_{};
  std::function<u64(u64)> hash_fn_;
  // These arenas outlive their placement-constructed objects.
  Arena buckets_, counters_, metadata_;
  mutable std::mutex directory_mutex_;
  std::vector<Entry*> entries_;
  std::vector<Directory*> directories_;
  std::atomic<Directory*> current_{nullptr};
  std::atomic<std::size_t> segment_count_{0};
  std::atomic<u64> splits_{0};
};

inline Entry* Tier::Get(u64 hash) const {
  return current_.load(std::memory_order_seq_cst)->Get(hash);
}

inline Route Tier::Locate(u64 hash) const {
  const auto bucket = static_cast<u32>(hash & (kBuckets - 1));
  // The fixed tier never enters Split: its directory, Entry identity and local
  // depth remain unchanged. Resolve its bucket without an Entry dependency;
  // Bucket Key/Value/Hot validation still governs concurrent KV mutation.
  if (!growable_) {
    const auto prefix = static_cast<std::size_t>(hash >> (64 - initial_depth_));
    return {entries_[prefix], fixed_buckets_ + prefix * kBuckets + bucket,
            initial_depth_, bucket};
  }
  for (;;) {
    auto* entry = Get(hash);
    if (entry->splitting.load(std::memory_order_seq_cst)) {
      std::this_thread::yield();
      continue;
    }
    const auto depth = entry->depth.load(std::memory_order_seq_cst);
    if (entry->splitting.load(std::memory_order_seq_cst)) continue;
    return {entry, entry->buckets + bucket, depth, bucket};
  }
}

inline bool Tier::Valid(const Route& route, u64 hash) const {
  return route.entry && !route.entry->splitting.load(std::memory_order_seq_cst) &&
         Get(hash) == route.entry &&
         route.entry->depth.load(std::memory_order_seq_cst) == route.depth &&
         route.bucket_index == (hash & (kBuckets - 1)) &&
         route.bucket == route.entry->buckets + route.bucket_index;
}
}  // namespace TimnahNameSpace::detail
