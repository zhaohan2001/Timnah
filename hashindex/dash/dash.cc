#include "dash.h"
#include "hashindex/tier_budget/tiered_memory.h"
#ifdef __SSE2__
#include <emmintrin.h>
#endif

DEFINE_uint32(dash_initial_depth, 16, "Dash-EH initial directory depth [1,24]");
DEFINE_int32(dash_dram_node, 0, "Dash-EH fast-pool NUMA node (metadata and some initial segments); -1 uses ordinary allocation");
DEFINE_int32(dash_slow_node, 2, "Dash-EH slow-pool NUMA node (remaining KV and overflow metadata); -1 uses ordinary allocation");

DEFINE_uint64(dash_fast_mib, 256, "Fixed fast-pool size in MiB (directory, metadata, staging buckets, and initial KV)");
DEFINE_uint64(dash_slow_initial_mib, 1024, "Initial slow-pool size in MiB");
DEFINE_uint64(dash_slow_growth_mib, 64, "Slow-pool growth block size in MiB");

namespace DashNameSpace {
namespace {
constexpr u32 kSlots = 14, kNormal = 64, kStash = 2, kTotal = kNormal + kStash;
constexpr u32 kFull = (1u << kSlots) - 1;
constexpr u8 kMaxDepth = 24;
constexpr u32 Next(u32 b) { return (b + 1) & (kNormal - 1); }
constexpr u32 Prev(u32 b) { return (b - 1) & (kNormal - 1); }
u32 Home(u64 hash) { return (hash >> 8) & (kNormal - 1); }

template <typename T> T Read(const T& value) {
  return std::atomic_ref<T>(const_cast<T&>(value)).load();
}
template <typename T, typename U> void Write(T& target, U value) {
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
// The bucket header is exactly 32 bytes: version lock 4 + count/allocation/membership bitmap 4 + local fingerprints 16
// + overflow descriptor 8, followed by fourteen 16-byte KV pairs in the same slow-tier bucket.
// overflow bits [0,31] hold four fingerprints, [32,35] hold valid bits,
// [36,43] hold stash IDs, [44,47] hold membership bits, and [48,63] count overflow records without fingerprints.
// ----------------------------------------------------------------
struct Slot { u64 key{}, value{}; };
struct alignas(64) Bucket {
  u32 version{};
  u32 state{};  // alloc[0,13], membership[14,27], count[28,31]
  u64 fingerprints[2]{};
  u64 overflow{};
  Slot slots[kSlots]{};
  bool TryLock() {
    auto ref = std::atomic_ref<u32>(version);
    u32 v = ref.load();
    return !(v & 1) && ref.compare_exchange_strong(v, v + 1);
  }
  void Unlock() { std::atomic_ref<u32>(version).fetch_add(1); }
};
static_assert(sizeof(Bucket) == 256 && offsetof(Bucket, slots) == 32);
struct alignas(64) Segment { Bucket buckets[kTotal]; };
static_assert(sizeof(Segment) == 16896);  // 16KiB normal + 512B stash
struct alignas(64) Entry {
  Segment* segment{};
  std::atomic<u8> depth{};
};

// All additional locks use try-lock; release locks acquired by this attempt and retry on failure to avoid circular neighbor-bucket deadlocks.
class Locks {
 public:
  ~Locks() { for (u32 i = count_; i > 0; --i) locked_[i - 1]->Unlock(); }
  bool TryAdd(Bucket& b) {
    for (u32 i = 0; i < count_; ++i) if (locked_[i] == &b) return true;
    if (!b.TryLock()) return false;
    locked_[count_++] = &b;
    return true;
  }
  bool Pair(Segment& s, u32 a, u32 b) {
    if (a > b) std::swap(a, b);
    return TryAdd(s.buckets[a]) && TryAdd(s.buckets[b]);
  }
  void All(Segment& s) {
    for (auto& b : s.buckets) while (!TryAdd(b)) std::this_thread::yield();
  }
 private:
  std::array<Bucket*, kTotal> locked_{};
  u32 count_{};
};

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
  Entry* Get(u64 hash) const { return entries[hash >> (64 - depth)].load(); }
};

// Directory control objects and pointer arrays also count toward the shared metadata pool.
struct DirectoryDeleter {
  void operator()(Directory* p) const { if (p) std::destroy_at(p); }
};
using DirectoryPtr = std::unique_ptr<Directory, DirectoryDeleter>;

u32 Alloc(u32 state) { return state & kFull; }
u32 Members(u32 state) { return (state >> 14) & kFull; }
u32 Count(u32 state) { return state >> 28; }
u32 Pack(u32 alloc, u32 members) {
  return alloc | ((members & alloc) << 14) | (u32(std::popcount(alloc)) << 28);
}
u32 Candidates(const Bucket& b, u64 hash, u32 mask) {
  if (!mask) return 0;
  const u64 lo = Read(b.fingerprints[0]), hi = Read(b.fingerprints[1]);
#ifdef __SSE2__
  const auto fps = _mm_set_epi64x(static_cast<long long>(hi), static_cast<long long>(lo));
  const auto needle = _mm_set1_epi8(static_cast<char>(hash));
  return static_cast<u32>(_mm_movemask_epi8(_mm_cmpeq_epi8(fps, needle))) & mask;
#else
  u32 result = 0;
  for (u32 i = 0; i < kSlots; ++i)
    if (((i < 8 ? lo : hi) >> (8 * (i % 8)) & 255) == (hash & 255)) result |= 1u << i;
  return result & mask;
#endif
}
struct Location { u32 bucket{}, slot{}; bool found{}; };
Location Probe(const Segment& s, u32 b, u64 hash, u64 key, int membership, u64* value) {
  const auto& bucket = s.buckets[b];
  const auto state = Read(bucket.state);
  auto mask = Alloc(state);
  if (membership == 0) mask &= ~Members(state);
  if (membership == 1) mask &= Members(state);
  mask = Candidates(bucket, hash, mask);
  while (mask) {
    const auto i = std::countr_zero(mask);
    mask &= mask - 1;
    if (Read(bucket.slots[i].key) == key) {
      if (value) *value = Read(bucket.slots[i].value);
      return {b, static_cast<u32>(i), true};
    }
  }
  return {};
}
u32 StashMask(const Segment& s, u32 home, u64 hash) {
  const auto a = Read(s.buckets[home].overflow);
  const auto b = Read(s.buckets[Next(home)].overflow);
  if (a >> 48) return (1u << kStash) - 1;
  u32 mask = 0;
  for (u32 side = 0; side < 2; ++side) {
    const auto hints = side ? b : a;
    for (u32 i = 0; i < 4; ++i) {
      if (!(hints & (1ull << (32 + i))) || ((hints >> (44 + i)) & 1) != side) continue;
      if (((hints >> (i * 8)) & 255) == (hash & 255))
        mask |= 1u << ((hints >> (36 + 2 * i)) & 3);
    }
  }
  return mask & ((1u << kStash) - 1);
}
Location FindHeld(const Segment& s, u64 hash, u64 key) {
  const auto home = Home(hash);
  auto loc = Probe(s, home, hash, key, 0, nullptr);
  if (loc.found) return loc;
  loc = Probe(s, Next(home), hash, key, 1, nullptr);
  if (loc.found) return loc;
  const auto mask = StashMask(s, home, hash);
  for (u32 j = 0; j < kStash; ++j) if (mask & (1u << j)) {
    loc = Probe(s, kNormal + j, hash, key, -1, nullptr);
    if (loc.found) return loc;
  }
  return {};
}
void PutSlot(Bucket& b, u64 hash, u64 key, u64 value, bool member) {
  const auto state = Read(b.state), alloc = Alloc(state);
  const u32 slot = std::countr_zero((~alloc) & kFull);
  if (slot >= kSlots) throw std::logic_error("Dash bucket overflow");
  Write(b.slots[slot].key, key);
  Write(b.slots[slot].value, value);
  const auto word = slot / 8, shift = (slot % 8) * 8;
  Write(b.fingerprints[word], (Read(b.fingerprints[word]) & ~(255ull << shift)) |
                            ((hash & 255) << shift));
  const auto members = member ? Members(state) | (1u << slot) : Members(state) & ~(1u << slot);
  Write(b.state, Pack(alloc | (1u << slot), members));
}
void EraseSlot(Bucket& b, u32 slot) {
  const auto state = Read(b.state);
  Write(b.state, Pack(Alloc(state) & ~(1u << slot), Members(state)));
}
void AddHint(Segment& s, u64 hash, u32 stash) {
  const auto home = Home(hash);
  for (u32 side = 0; side < 2; ++side) {
    auto& b = s.buckets[side ? Next(home) : home];
    auto hints = Read(b.overflow);
    const u32 occupied = (hints >> 32) & 15;
    if (occupied == 15) continue;
    const auto i = std::countr_zero((~occupied) & 15u);
    hints &= ~((255ull << (8 * i)) | (3ull << (36 + 2 * i)) | (1ull << (44 + i)));
    hints |= (hash & 255) << (8 * i);
    hints |= 1ull << (32 + i);
    hints |= u64(stash) << (36 + 2 * i);
    hints |= u64(side) << (44 + i);
    Write(b.overflow, hints);
    return;
  }
  auto& b = s.buckets[home];
  Write(b.overflow, Read(b.overflow) + (1ull << 48));
}
void RemoveHint(Segment& s, u64 hash, u32 stash) {
  const auto home = Home(hash);
  for (u32 side = 0; side < 2; ++side) {
    auto& b = s.buckets[side ? Next(home) : home];
    auto hints = Read(b.overflow);
    for (u32 i = 0; i < 4; ++i) {
      if ((hints & (1ull << (32 + i))) && ((hints >> (44 + i)) & 1) == side &&
          ((hints >> (8 * i)) & 255) == (hash & 255) &&
          ((hints >> (36 + 2 * i)) & 3) == stash) {
        Write(b.overflow, hints & ~(1ull << (32 + i)));
        return;
      }
    }
  }
  auto& b = s.buckets[home];
  const auto hints = Read(b.overflow);
  if (!(hints >> 48)) throw std::logic_error("Dash missing overflow metadata");
  Write(b.overflow, hints - (1ull << 48));
}
}  // namespace

class Dash::Impl {
 public:
  explicit Impl(Options o) : options(o), memory(TierBudget::Config{
      o.dram_node, o.slow_node, o.fast_bytes, o.slow_initial_bytes, o.slow_growth_bytes, true}),
      generations(TierBudget::Allocator<DirectoryPtr>(&memory)),
      owned(TierBudget::Allocator<Entry*>(&memory)) {
    auto dir = NewDirectory(o.initial_depth);
    try {
      owned.reserve(dir->size);
      for (u32 i = 0; i < dir->size; ++i)
        dir->entries[i].store(NewEntry(o.initial_depth, false));
      for (u32 i = 0; i < dir->size; ++i) {
        auto* e = dir->entries[i].load();
        e->segment = std::construct_at(static_cast<Segment*>(memory.InitialKV(sizeof(Segment), i)));
      }
      generations.push_back(std::move(dir));
      current.store(generations.back().get());
    } catch (...) { Cleanup(); throw; }
  }
  ~Impl() { Cleanup(); }
  void Cleanup() {
    for (auto* e : owned) { if (e->segment) std::destroy_at(e->segment); std::destroy_at(e); }
    owned.clear();
  }
  DirectoryPtr NewDirectory(u8 depth) {
    return DirectoryPtr(std::construct_at(
        static_cast<Directory*>(memory.Metadata(sizeof(Directory))), depth, memory));
  }

  Entry* NewEntry(u8 depth, bool allocate_segment = true) {
    auto* e = std::construct_at(static_cast<Entry*>(memory.Metadata(sizeof(Entry))));
    e->depth.store(depth);
    try {
      if (allocate_segment) e->segment = std::construct_at(static_cast<Segment*>(memory.SplitKV(sizeof(Segment))));
      owned.push_back(e);
    } catch (...) {
      if (e->segment) std::destroy_at(e->segment);
      std::destroy_at(e);
      throw;
    }
    return e;
  }
  bool Routed(Entry* e, u64 hash) const { return current.load()->Get(hash) == e; }

  auto Search(u64 key, u64& value) const -> Result {
    const auto hash = HashFunction(key);
    const auto home = Home(hash), next = Next(home);
    for (;;) {
      auto* e = current.load()->Get(hash);
      const auto& s = *e->segment;
      const auto v0 = Read(s.buckets[home].version), v1 = Read(s.buckets[next].version);
      if ((v0 | v1) & 1) { std::this_thread::yield(); continue; }
      u64 candidate{};
      auto loc = Probe(s, home, hash, key, 0, &candidate);
      if (!loc.found) loc = Probe(s, next, hash, key, 1, &candidate);
      u32 touched = 0;
      std::array<u32, kStash> stash_versions{};
      bool retry = false;
      if (!loc.found) {
        const auto mask = StashMask(s, home, hash);
        for (u32 j = 0; j < kStash; ++j) if (mask & (1u << j)) {
          const auto b = kNormal + j;
          stash_versions[j] = Read(s.buckets[b].version);
          if (stash_versions[j] & 1) { retry = true; break; }
          touched |= 1u << j;
          loc = Probe(s, b, hash, key, -1, &candidate);
          if (loc.found) break;
        }
      }
      if (retry) continue;
      for (u32 j = 0; j < kStash; ++j)
        if ((touched & (1u << j)) && stash_versions[j] != Read(s.buckets[kNormal+j].version))
          retry = true;
      if (retry || v0 != Read(s.buckets[home].version) ||
          v1 != Read(s.buckets[next].version) || !Routed(e, hash)) continue;
      if (loc.found) value = candidate;
      return loc.found ? Result::SUCCESS : Result::FAILED;
    }
  }

  // Return -1 for contention on an additional bucket lock, 0 for no space, and 1 for a successful insertion.
  int TryInsert(Segment& s, Locks& locks, u64 hash, u64 key, u64 value) {
    const auto home = Home(hash), next = Next(home);
    auto& a = s.buckets[home];
    auto& b = s.buckets[next];
    const auto na = Count(Read(a.state)), nb = Count(Read(b.state));
    if (na < kSlots || nb < kSlots) {
      const bool member = na > nb;
      PutSlot(member ? b : a, hash, key, value, member);
      return 1;
    }
    // forward: move native records from next to next+1; backward: return borrowed records from home to home-1.
    const auto bstate = Read(b.state);
    const auto forward = Alloc(bstate) & ~Members(bstate);
    if (forward) {
      auto& dest = s.buckets[Next(next)];
      if (!locks.TryAdd(dest)) return -1;
      if (Count(Read(dest.state)) < kSlots) {
        const auto slot = std::countr_zero(forward);
        const auto k = Read(b.slots[slot].key), v = Read(b.slots[slot].value);
        PutSlot(dest, HashFunction(k), k, v, true);
        EraseSlot(b, slot);
        PutSlot(b, hash, key, value, true);
        displacements.fetch_add(1, std::memory_order_relaxed);
        return 1;
      }
    }
    const auto astate = Read(a.state);
    const auto backward = Alloc(astate) & Members(astate);
    if (backward) {
      auto& dest = s.buckets[Prev(home)];
      if (!locks.TryAdd(dest)) return -1;
      if (Count(Read(dest.state)) < kSlots) {
        const auto slot = std::countr_zero(backward);
        const auto k = Read(a.slots[slot].key), v = Read(a.slots[slot].value);
        PutSlot(dest, HashFunction(k), k, v, false);
        EraseSlot(a, slot);
        PutSlot(a, hash, key, value, false);
        displacements.fetch_add(1, std::memory_order_relaxed);
        return 1;
      }
    }
    for (u32 j = 0; j < kStash; ++j) {
      auto& stash = s.buckets[kNormal + j];
      if (!locks.TryAdd(stash)) return -1;
      if (Count(Read(stash.state)) == kSlots) continue;
      PutSlot(stash, hash, key, value, false);
      AddHint(s, hash, j);
      stash_inserts.fetch_add(1, std::memory_order_relaxed);
      return 1;
    }
    return 0;
  }

  void Split(Entry* e, u64 hash, u8 expected_depth) {
    std::lock_guard directory_lock(split_mutex);
    if (!Routed(e, hash) || e->depth.load() != expected_depth) return;
    auto& s = *e->segment;
    Locks all;
    all.All(s);
    if (!Routed(e, hash) || e->depth.load() != expected_depth) return;
    const auto home = Home(hash), next = Next(home);
    // Another writer may delete a record while the split lock is pending; return to ordinary insertion if space becomes available.
    if (Count(Read(s.buckets[home].state)) < kSlots ||
        Count(Read(s.buckets[next].state)) < kSlots) return;
    for (u32 j = 0; j < kStash; ++j)
      if (Count(Read(s.buckets[kNormal+j].state)) < kSlots) return;
    if (expected_depth >= kMaxDepth) throw std::overflow_error("Dash maximum directory depth reached");
    struct Record { u64 key, value; u32 bucket; bool member; };
    std::vector<Record> records;
    records.reserve(kTotal * kSlots);
    for (u32 b = 0; b < kTotal; ++b) {
      const auto state = Read(s.buckets[b].state);
      auto mask = Alloc(state);
      while (mask) {
        const auto slot = std::countr_zero(mask);
        mask &= mask - 1;
        records.push_back({Read(s.buckets[b].slots[slot].key), Read(s.buckets[b].slots[slot].value),
                           b, bool(Members(state) & (1u << slot))});
      }
    }
    auto* old_dir = current.load();
    DirectoryPtr doubled;
    if (expected_depth == old_dir->depth) {
      doubled = NewDirectory(old_dir->depth + 1);
      for (u32 i = 0; i < old_dir->size; ++i) {
        auto* ptr = old_dir->entries[i].load();
        doubled->entries[2*i].store(ptr);
        doubled->entries[2*i+1].store(ptr);
      }
      generations.reserve(generations.size() + 1);
    }
    auto* right = NewEntry(expected_depth + 1);
    // Keep the version lock intact so this split invalidates snapshots held by existing readers.
    for (auto& bucket : s.buckets) { Write(bucket.state, u32{0}); Write(bucket.overflow, u64{0}); }
    e->depth.store(expected_depth + 1);
    // Preserve bucket ownership and membership; partition only by the new MSB so each destination receives a subset of its original bucket,
    // preventing recursive splits caused by stash overflow due to load order within a split.
    for (const auto& r : records) {
      const auto h = HashFunction(r.key);
      auto& target = ((h >> (64 - expected_depth - 1)) & 1) ? *right->segment : s;
      PutSlot(target.buckets[r.bucket], h, r.key, r.value, r.member);
      if (r.bucket >= kNormal) AddHint(target, h, r.bucket - kNormal);
    }
    auto* dir = doubled ? doubled.get() : old_dir;
    const u32 base = u32(hash >> (64 - expected_depth)) << (dir->depth - expected_depth);
    const u32 half = 1u << (dir->depth - expected_depth - 1);
    for (u32 i = base + half; i < base + 2*half; ++i) dir->entries[i].store(right);
    if (doubled) { generations.push_back(std::move(doubled)); current.store(dir); }
    splits.fetch_add(1, std::memory_order_relaxed);
  }

  auto Mutate(u64 key, u64 value, int operation) -> Result {
    const auto hash = HashFunction(key), home = u64(Home(hash));
    for (;;) {
      auto* e = current.load()->Get(hash);
      u8 split_depth{};
      {
        auto& s = *e->segment;
        Locks locks;
        if (!locks.Pair(s, home, Next(home))) { std::this_thread::yield(); continue; }
        if (!Routed(e, hash)) continue;
        const auto loc = FindHeld(s, hash, key);
        if (operation == 0) {
          if (loc.found) return Result::FAILED;
          const auto status = TryInsert(s, locks, hash, key, value);
          if (status < 0) continue;
          if (status > 0) { size.fetch_add(1, std::memory_order_relaxed); return Result::SUCCESS; }
          split_depth = e->depth.load();
        } else {
          if (!loc.found) return Result::FAILED;
          if (!locks.TryAdd(s.buckets[loc.bucket])) continue;
          if (operation == 1) Write(s.buckets[loc.bucket].slots[loc.slot].value, value);
          else {
            if (loc.bucket >= kNormal) RemoveHint(s, hash, loc.bucket - kNormal);
            EraseSlot(s.buckets[loc.bucket], loc.slot);
            size.fetch_sub(1, std::memory_order_relaxed);
          }
          return Result::SUCCESS;
        }
      }
      Split(e, hash, split_depth);
    }
  }

  Options options;
  TierBudget::Memory memory;
  std::atomic<Directory*> current{nullptr};
  std::mutex split_mutex;
  std::vector<DirectoryPtr, TierBudget::Allocator<DirectoryPtr>> generations;
  std::vector<Entry*, TierBudget::Allocator<Entry*>> owned;
  std::atomic<u64> size{0}, splits{0}, displacements{0}, stash_inserts{0};
};

Dash::Dash() : Dash(Options{static_cast<u8>(std::min(FLAGS_dash_initial_depth, 255u)),
                           FLAGS_dash_dram_node, FLAGS_dash_slow_node,
      FLAGS_dash_fast_mib << 20, FLAGS_dash_slow_initial_mib << 20,
      FLAGS_dash_slow_growth_mib << 20}) {}
Dash::Dash(Options options) : options_(options) {}
Dash::~Dash() = default;
auto Dash::Create(const ParamType&) -> Result {
  if (impl_ || options_.initial_depth < 1 || options_.initial_depth > kMaxDepth ||
      !ValidNode(options_.dram_node) || !ValidNode(options_.slow_node) ||
      !options_.fast_bytes || !options_.slow_initial_bytes || !options_.slow_growth_bytes) return Result::FAILED;
  impl_ = std::make_unique<Impl>(options_);
  LOG("Dash memory_initial {}\n", impl_->memory.Describe());
  LOG("Dash memory_placement {}\n", impl_->memory.Placement());
  return Result::SUCCESS;
}
auto Dash::Stop() -> void {}  // No background threads; destruction is safe after the caller stops foreground access.
auto Dash::Insert(const u64& k, const u64& v) -> Result { return impl_ ? impl_->Mutate(k,v,0) : Result::FAILED; }
auto Dash::Search(const u64& k, u64& v) -> Result { return impl_ ? impl_->Search(k,v) : Result::FAILED; }
auto Dash::Update(const u64& k, const u64& v) -> Result { return impl_ ? impl_->Mutate(k,v,1) : Result::FAILED; }
auto Dash::Remove(const u64& k) -> Result { return impl_ ? impl_->Mutate(k,0,2) : Result::FAILED; }
auto Dash::Stat() -> string {
  if (!impl_) return "Dash not created\n";
  std::lock_guard lock(impl_->split_mutex);
  const auto n = impl_->owned.size(), records = impl_->size.load();
  size_t directory_bytes = 0;
  for (const auto& d : impl_->generations) directory_bytes += sizeof(*d->entries) * d->size;
  const auto memory = impl_->memory.GetStats();
  return fmt::format("Dash size={} segments={} depth={} splits={} displacements={} stash_inserts={} "
                     "load_factor={:.4f} dram_bytes={} slow_bytes={} dram_node={} slow_node={}\n",
      records,n,impl_->current.load()->depth,impl_->splits.load(),impl_->displacements.load(),
      impl_->stash_inserts.load(),double(records)/(n*kTotal*kSlots),
      memory.fast_reserved,memory.slow_reserved,options_.dram_node,options_.slow_node) +
      fmt::format("Dash layout bucket_bytes={} metadata_bytes={} slot_bytes={} slots_per_bucket={} "
                  "normal_buckets={} stash_buckets={} segment_bytes={} directory_array_bytes={} "
                  "fast_kv_segments={} slow_kv_segments={} logical_kv_bytes={}\n",
          sizeof(Bucket),offsetof(Bucket, slots),sizeof(Slot),kSlots,kNormal,kStash,sizeof(Segment),
          directory_bytes,memory.fast_kv/sizeof(Segment),memory.slow_kv/sizeof(Segment),
          records*sizeof(Slot)) + impl_->memory.Describe() + "\n";
}
}  // namespace DashNameSpace
