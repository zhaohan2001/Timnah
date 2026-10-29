#include "chash.h"
#include "hashindex/tier_budget/tiered_memory.h"

DEFINE_uint32(chash_initial_depth, 16, "CHash initial directory depth [1,24]");
DEFINE_int32(chash_dram_node, 0, "CHash directory/metadata/staging-bucket NUMA node; -1 uses ordinary allocation");
DEFINE_int32(chash_slow_node, 2, "CHash KV-segment NUMA node; -1 uses ordinary allocation");

DEFINE_uint64(chash_fast_mib, 256, "Fixed fast-pool size in MiB (directory, metadata, staging buckets, and initial KV)");
DEFINE_uint64(chash_slow_initial_mib, 1024, "Initial slow-pool size in MiB");
DEFINE_uint64(chash_slow_growth_mib, 64, "Slow-pool growth block size in MiB");

namespace CHashNameSpace {
namespace {
constexpr u32 kSlots = 16;
constexpr u32 kBuckets = 32;
constexpr u32 kStage = kBuckets;
constexpr u64 kFull = (1ull << kSlots) - 1;
constexpr u8 kMaxDepth = 24;

// Access every shared byte used by optimistic readers through atomic_ref, avoiding ordinary seqlock
// data races under the C++ memory model. This basic version conservatively uses sequential consistency.
// ----------------------------------------------------------------
template <typename T>
T Read(const T& value) {
  return std::atomic_ref<T>(const_cast<T&>(value)).load();
}
template <typename T, typename U>
void Write(T& target, U value) {
  std::atomic_ref<T>(target).store(static_cast<T>(value));
}

bool ValidNode(int node) {
#ifdef __linux__
  return node == -1 || (numa_available() >= 0 && node >= 0 &&
      node <= numa_max_node() && numa_bitmask_isbitset(numa_all_nodes_ptr, node));
#else
  return node == -1;
#endif
}

// Use the low 24 bits from one HashFunction call: the low 8 bits form the fingerprint, and the next 16 bits
// form a four-part Bloom filter. Reserve the high 40 bits for the extendible directory.
// ----------------------------------------------------------------
template <size_t PartitionBits>
struct Filter {
  static_assert(std::has_single_bit(PartitionBits) && PartitionBits >= 64);
  static constexpr size_t kWordsPerPart = PartitionBits / 64;
  u64 words[4 * kWordsPerPart]{};
  static size_t Position(u64 hash, u32 part) {
    const u32 bits = (hash >> 8) & 0xffff;
    const u32 rotated = ((bits >> (part * 4)) | (bits << (16 - part * 4))) & 0xffff;
    return rotated & (PartitionBits - 1);
  }
  bool Contains(u64 hash) const {
    for (u32 i = 0; i < 4; ++i) {
      const auto bit = Position(hash, i);
      if (!(Read(words[i * kWordsPerPart + bit / 64]) & (1ull << (bit % 64))))
        return false;
    }
    return true;
  }
  void Add(u64 hash) {
    for (u32 i = 0; i < 4; ++i) {
      const auto bit = Position(hash, i);
      auto& word = words[i * kWordsPerPart + bit / 64];
      Write(word, Read(word) | (1ull << (bit % 64)));
    }
  }
  void Clear() { for (auto& word : words) Write(word, u64{0}); }
};

struct Slot { u64 key{}, value{}; };
struct alignas(64) Bucket { Slot slots[kSlots]{}; };
struct alignas(64) Segment { Bucket buckets[kBuckets]; };
static_assert(sizeof(Bucket) == 256);
static_assert(sizeof(Segment) == 8192);
struct BucketMeta {
  u64 bitmap{};
  Filter<64> filter;
  u8 fingerprints[kSlots]{};
};
static_assert(sizeof(BucketMeta) == 56);

struct alignas(64) Entry {
  std::atomic<u64> version{0};  // Odd values indicate a writer lock; even values indicate a stable version.
  Segment* segment{};
  u64 segment_bitmap{};
  u8 local_depth{};  // Accessed only by a writer holding this segment lock and the directory split lock.
  Filter<1024> filter;
  BucketMeta metadata[kBuckets + 1];
  Bucket stage;

  Bucket& GetBucket(u32 i) { return i == kStage ? stage : segment->buckets[i]; }
  const Bucket& GetBucket(u32 i) const { return i == kStage ? stage : segment->buckets[i]; }
  void Lock() {
    for (;;) {
      auto v = version.load();
      if (!(v & 1) && version.compare_exchange_weak(v, v + 1)) return;
      std::this_thread::yield();
    }
  }
  void Unlock() { version.fetch_add(1); }
};
struct EntryGuard {
  Entry& entry;
  explicit EntryGuard(Entry& e) : entry(e) { entry.Lock(); }
  ~EntryGuard() { entry.Unlock(); }
};

// Retain published directory generations until index destruction; directory slots use atomic pointers.
// Lock-free readers therefore never dereference reclaimed directories/segments. Revalidate routing before every return.
// ----------------------------------------------------------------
struct Directory {
  const u8 depth;
  const u32 size;
  std::atomic<Entry*>* entries;
  Directory(u8 d, TierBudget::Memory& memory) : depth(d), size(1u << d) {
    entries = static_cast<std::atomic<Entry*>*>(memory.Metadata(sizeof(*entries) * size));
    for (u32 i = 0; i < size; ++i) std::construct_at(entries + i, nullptr);
  }
  ~Directory() {
    for (u32 i = 0; i < size; ++i) std::destroy_at(entries + i);
  }
  u32 Index(u64 hash) const { return static_cast<u32>(hash >> (64 - depth)); }
  Entry* Get(u64 hash) const { return entries[Index(hash)].load(); }
};

// Directory control objects and pointer arrays also count toward the shared metadata pool.
struct DirectoryDeleter {
  void operator()(Directory* p) const { if (p) std::destroy_at(p); }
};
using DirectoryPtr = std::unique_ptr<Directory, DirectoryDeleter>;

struct Location {
  u32 bucket{}, slot{};
  bool found{};
};
Location Find(const Entry& e, u64 hash, u64 key, u64* result = nullptr) {
  if (!e.filter.Contains(hash)) return {};
  const u8 fp = static_cast<u8>(hash);
  // Check the staging bucket first, then scan slow-tier buckets in reverse order (paper Algorithm 1).
  for (int b = kStage; b >= 0; --b) {
    const auto& meta = e.metadata[b];
    auto mask = Read(meta.bitmap);
    if (!mask || !meta.filter.Contains(hash)) continue;
    while (mask) {
      const u32 slot = std::countr_zero(mask);
      mask &= mask - 1;
      if (Read(meta.fingerprints[slot]) != fp) continue;
      const auto& kv = e.GetBucket(b).slots[slot];
      if (Read(kv.key) == key) {
        if (result) *result = Read(kv.value);
        return {static_cast<u32>(b), slot, true};
      }
    }
  }
  return {};
}
}  // namespace

class CHash::Impl {
 public:
  explicit Impl(Options o) : options(o), memory(TierBudget::Config{
      o.dram_node, o.slow_node, o.fast_bytes, o.slow_initial_bytes, o.slow_growth_bytes, true}),
      generations(TierBudget::Allocator<DirectoryPtr>(&memory)),
      owned_entries(TierBudget::Allocator<Entry*>(&memory)) {
    auto directory = NewDirectory(o.initial_depth);
    try {
      owned_entries.reserve(directory->size);
      // Allocate the initial directory/metadata first, then place initial KV segments using a static 1:4 candidate ratio.
      for (u32 i = 0; i < directory->size; ++i)
        directory->entries[i].store(NewEntry(o.initial_depth, false));
      for (u32 i = 0; i < directory->size; ++i) {
        auto* e = directory->entries[i].load();
        e->segment = std::construct_at(static_cast<Segment*>(memory.InitialKV(sizeof(Segment), i)));
      }
      generations.push_back(std::move(directory));
      current.store(generations.back().get());
    } catch (...) { Cleanup(); throw; }
  }
  ~Impl() { Cleanup(); }
  void Cleanup() {
    for (auto* e : owned_entries) {
      if (e->segment) std::destroy_at(e->segment);
      std::destroy_at(e);
    }
    owned_entries.clear();
  }

  DirectoryPtr NewDirectory(u8 depth) {
    return DirectoryPtr(std::construct_at(
        static_cast<Directory*>(memory.Metadata(sizeof(Directory))), depth, memory));
  }

  Entry* NewEntry(u8 depth, bool allocate_segment = true) {
    auto* entry_memory = memory.Metadata(sizeof(Entry));
    auto* e = std::construct_at(static_cast<Entry*>(entry_memory));
    e->local_depth = depth;
    try {
      // This basic version explicitly initializes segments to establish valid object lifetimes for all objects and atomic accesses.
      if (allocate_segment) e->segment = std::construct_at(
          static_cast<Segment*>(memory.SplitKV(sizeof(Segment))));
      owned_entries.push_back(e);
    } catch (...) {
      if (e->segment) std::destroy_at(e->segment);
      std::destroy_at(e);
      throw;
    }
    return e;
  }

  bool Routed(Entry* e, u64 hash) const { return current.load()->Get(hash) == e; }

  bool Flush(Entry& e) {
    for (u32 b = 0; b < kBuckets; ++b) {
      if (Read(e.metadata[b].bitmap)) continue;
      auto& from = e.metadata[kStage];
      auto& to = e.metadata[b];
      // Move an entire 256-byte staging bucket at once. Copy atomic words instead of using memcpy,
      // preventing C++ data races between bucket reuse and existing readers that have not completed version validation.
      for (u32 i = 0; i < kSlots; ++i) {
        Write(e.segment->buckets[b].slots[i].key, Read(e.stage.slots[i].key));
        Write(e.segment->buckets[b].slots[i].value, Read(e.stage.slots[i].value));
        Write(to.fingerprints[i], Read(from.fingerprints[i]));
      }
      for (u32 i = 0; i < 4; ++i) Write(to.filter.words[i], Read(from.filter.words[i]));
      Write(to.bitmap, Read(from.bitmap));
      Write(e.segment_bitmap, Read(e.segment_bitmap) | (1ull << b));
      Write(from.bitmap, u64{0});
      from.filter.Clear();
      flushes.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  bool Put(Entry& e, u64 hash, u64 key, u64 value) {
    auto& meta = e.metadata[kStage];
    auto bitmap = Read(meta.bitmap);
    if (bitmap == kFull) {
      if (!Flush(e)) return false;
      bitmap = 0;
    }
    const auto slot = std::countr_zero((~bitmap) & kFull);
    Write(e.stage.slots[slot].key, key);
    Write(e.stage.slots[slot].value, value);
    Write(meta.fingerprints[slot], static_cast<u8>(hash));
    meta.filter.Add(hash);
    e.filter.Add(hash);
    // Publish KV and filter metadata first, then publish the valid bitmap.
    Write(meta.bitmap, bitmap | (1ull << slot));
    return true;
  }

  void Split(Entry& e, u64 hash) {
    // The caller already holds the version write lock for e; serialize directory updates only during splits/doublings.
    std::lock_guard lock(split_mutex);
    auto* old_dir = current.load();
    const u8 old_depth = e.local_depth;
    if (old_depth >= kMaxDepth) throw std::overflow_error("CHash maximum directory depth reached");
    std::vector<std::pair<u64, u64>> records;
    records.reserve((kBuckets + 1) * kSlots);
    for (u32 b = 0; b <= kStage; ++b) {
      auto mask = Read(e.metadata[b].bitmap);
      while (mask) {
        const u32 s = std::countr_zero(mask);
        mask &= mask - 1;
        const auto& kv = e.GetBucket(b).slots[s];
        records.emplace_back(Read(kv.key), Read(kv.value));
      }
    }
    // Complete all allocations that could fail before modifying the original segment.
    DirectoryPtr doubled;
    if (old_depth == old_dir->depth) {
      doubled = NewDirectory(old_dir->depth + 1);
      for (u32 i = 0; i < old_dir->size; ++i) {
        auto* ptr = old_dir->entries[i].load();
        doubled->entries[2 * i].store(ptr);
        doubled->entries[2 * i + 1].store(ptr);
      }
      generations.reserve(generations.size() + 1);
    }
    auto* right = NewEntry(old_depth + 1);
    auto* dir = doubled ? doubled.get() : old_dir;
    for (auto& meta : e.metadata) {
      Write(meta.bitmap, u64{0});
      meta.filter.Clear();
    }
    Write(e.segment_bitmap, u64{0});
    e.filter.Clear();
    e.local_depth = old_depth + 1;
    for (const auto& [key, value] : records) {
      const auto h = HashFunction(key);
      Entry& target = ((h >> (64 - e.local_depth)) & 1) ? *right : e;
      if (!Put(target, h, key, value)) std::terminate();
    }
    // An original segment maps to a contiguous directory range; update its right half after fully initializing metadata before publication.
    const u32 base = static_cast<u32>(hash >> (64 - old_depth)) << (dir->depth - old_depth);
    const u32 half = 1u << (dir->depth - old_depth - 1);
    for (u32 i = base + half; i < base + 2 * half; ++i) dir->entries[i].store(right);
    if (doubled) {
      generations.push_back(std::move(doubled));
      current.store(dir);
    }
    splits.fetch_add(1, std::memory_order_relaxed);
  }

  auto Search(u64 key, u64& value) const -> Result {
    const auto hash = HashFunction(key);
    for (;;) {
      auto* e = current.load()->Get(hash);
      const auto version = e->version.load();
      if (version & 1) { std::this_thread::yield(); continue; }
      u64 candidate{};
      const auto loc = Find(*e, hash, key, &candidate);
      if (version != e->version.load() || !Routed(e, hash)) continue;
      if (loc.found) value = candidate;
      return loc.found ? Result::SUCCESS : Result::FAILED;
    }
  }

  auto Mutate(u64 key, u64 value, int operation) -> Result {
    const auto hash = HashFunction(key);
    for (;;) {
      auto* e = current.load()->Get(hash);
      EntryGuard guard(*e);
      if (!Routed(e, hash)) continue;
      const auto loc = Find(*e, hash, key);
      if (operation == 0) {
        if (loc.found) return Result::FAILED;  // Match the duplicate-insertion semantics of sub.
        if (!Put(*e, hash, key, value)) { Split(*e, hash); continue; }
        size.fetch_add(1, std::memory_order_relaxed);
      } else {
        if (!loc.found) return Result::FAILED;
        if (operation == 1) Write(e->GetBucket(loc.bucket).slots[loc.slot].value, value);
        else {
          auto& meta = e->metadata[loc.bucket];
          const auto bitmap = Read(meta.bitmap) & ~(1ull << loc.slot);
          Write(meta.bitmap, bitmap);
          if (!bitmap && loc.bucket != kStage)
            Write(e->segment_bitmap, Read(e->segment_bitmap) & ~(1ull << loc.bucket));
          // Deletion clears only the valid bit; rebuild the Bloom filter on the next split.
          size.fetch_sub(1, std::memory_order_relaxed);
        }
      }
      return Result::SUCCESS;
    }
  }

  Options options;
  TierBudget::Memory memory;
  std::atomic<Directory*> current{nullptr};
  std::mutex split_mutex;
  std::vector<DirectoryPtr, TierBudget::Allocator<DirectoryPtr>> generations;
  std::vector<Entry*, TierBudget::Allocator<Entry*>> owned_entries;
  std::atomic<u64> size{0}, flushes{0}, splits{0};
};

// IndexInterface adapter; Create, Stop, and destruction require the caller to stop foreground access first.
// ----------------------------------------------------------------
CHash::CHash() : CHash(Options{static_cast<u8>(std::min(FLAGS_chash_initial_depth, 255u)),
                             FLAGS_chash_dram_node, FLAGS_chash_slow_node,
      FLAGS_chash_fast_mib << 20, FLAGS_chash_slow_initial_mib << 20,
      FLAGS_chash_slow_growth_mib << 20}) {}
CHash::CHash(Options options) : options_(options) {}
CHash::~CHash() = default;
auto CHash::Create(const ParamType&) -> Result {
  if (impl_ || options_.initial_depth < 1 || options_.initial_depth > kMaxDepth ||
      !ValidNode(options_.dram_node) || !ValidNode(options_.slow_node) ||
      !options_.fast_bytes || !options_.slow_initial_bytes || !options_.slow_growth_bytes) return Result::FAILED;
  impl_ = std::make_unique<Impl>(options_);
  LOG("CHash memory_initial {}\n", impl_->memory.Describe());
  LOG("CHash memory_placement {}\n", impl_->memory.Placement());
  return Result::SUCCESS;
}
auto CHash::Stop() -> void {}  // Flush buckets synchronously without background hot/cold migration threads.
auto CHash::Insert(const u64& k, const u64& v) -> Result {
  return impl_ ? impl_->Mutate(k, v, 0) : Result::FAILED;
}
auto CHash::Search(const u64& k, u64& v) -> Result {
  return impl_ ? impl_->Search(k, v) : Result::FAILED;
}
auto CHash::Update(const u64& k, const u64& v) -> Result {
  return impl_ ? impl_->Mutate(k, v, 1) : Result::FAILED;
}
auto CHash::Remove(const u64& k) -> Result {
  return impl_ ? impl_->Mutate(k, 0, 2) : Result::FAILED;
}
auto CHash::Stat() -> string {
  if (!impl_) return "CHash not created\n";
  std::lock_guard lock(impl_->split_mutex);
  const auto segments = impl_->owned_entries.size();
  const auto records = impl_->size.load();
  size_t directory_bytes = 0;
  for (const auto& d : impl_->generations) directory_bytes += sizeof(*d->entries) * d->size;
  return fmt::format("CHash size={} segments={} depth={} flushes={} splits={} "
                     "load_factor={:.4f} dram_bytes={} slow_bytes={} dram_node={} slow_node={}\n",
      records, segments, impl_->current.load()->depth, impl_->flushes.load(),
      impl_->splits.load(), double(records) / (segments * (kBuckets + 1) * kSlots),
      impl_->memory.GetStats().fast_reserved, impl_->memory.GetStats().slow_reserved,
      options_.dram_node, options_.slow_node) + impl_->memory.Describe() + "\n";
}
}  // namespace CHashNameSpace
