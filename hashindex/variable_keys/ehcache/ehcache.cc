#include "ehcache.h"
#include "../timnah/key_codec.h"
#include "backend.h"

DEFINE_uint32(ehcache_initial_depth, 16, "EH+cache initial EH directory depth");
DEFINE_int32(ehcache_dram_node, 0, "EH+cache DRAM NUMA node");
DEFINE_int32(ehcache_slow_node, 2, "EH+cache CXL simulation NUMA node");
DEFINE_uint64(ehcache_fast_mib, 512, "EH+cache total cache+metadata fast budget MiB");
DEFINE_uint64(ehcache_slow_initial_mib, 2048, "EH+cache initial slow arena MiB");
DEFINE_uint64(ehcache_slow_growth_mib, 64, "EH+cache slow growth block MiB");
DEFINE_double(ehcache_sample_rate, 0.1, "EH+cache exact LFU access sampling rate");

namespace EHCacheNameSpace {
namespace {
template <typename T> T Read(const T& value) {
  return std::atomic_ref<T>(const_cast<T&>(value)).load();
}
template <typename T> void Write(T& target, T value) {
  std::atomic_ref<T>(target).store(value);
}
u64 Hash(u64 value) {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}
size_t MiB(u64 value) {
  if (value > (std::numeric_limits<size_t>::max() >> 20))
    throw std::invalid_argument("EH+cache MiB capacity overflow");
  return static_cast<size_t>(value) << 20;
}
std::atomic<u64> next_identity{1};
}  // namespace

// The cache is a bounded, partitioned linear hash table. Incremental splits
// change (base, split) one bucket at a time; preallocated primary and overflow
// buckets keep all resident cache storage within the fast-memory budget.
class EHCache::Impl {
  static constexpr u32 kShards = 256, kSlots = 9;
  static constexpr u32 kNone = UINT32_MAX, kStats = 512;
  struct Slot { u64 key{}, value{}, frequency{}; };
  struct alignas(64) Bucket {
    u32 next{kNone};
    u16 occupied{};
    u16 padding{};
    Slot slots[kSlots]{};
  };
  static_assert(sizeof(Bucket) == 256);
  struct alignas(64) Shard {
    std::atomic_flag mutex = ATOMIC_FLAG_INIT;
    std::atomic<u64> version{0};
    Bucket* buckets{};
    u32 base{1}, split{}, entries{}, free_overflow{kNone};
    u32 max_primary{}, total_buckets{};
    u64 splits{};
    void Lock() {
      unsigned retries = 0;
      while (mutex.test_and_set(std::memory_order_acquire)) {
        // Test-and-test-and-set avoids repeatedly requesting exclusive cache
        // ownership while another worker is still in a slow-tier operation.
        while (mutex.test(std::memory_order_relaxed)) {
          if (++retries % 256 == 0) std::this_thread::yield();
#if defined(__x86_64__) || defined(__i386__)
          else __builtin_ia32_pause();
#endif
        }
      }
    }
  };
  static_assert(sizeof(Shard) == 64);
  struct Guard {
    Shard& shard;
    bool writing{};
    explicit Guard(Shard& s) : shard(s) { shard.Lock(); }
    void BeginWrite() {
      if (!writing) { shard.version.fetch_add(1); writing = true; }
    }
    ~Guard() {
      if (writing) shard.version.fetch_add(1);
      shard.mutex.clear(std::memory_order_release);
    }
  };
  struct alignas(64) Counters {
    std::atomic<u64> hits{0}, misses{0}, samples{0}, admissions{0}, evictions{0};
  };
  static_assert(sizeof(Counters) == 64);
  struct ThreadState { u64 identity{}, random{}; Counters* counters{}; };
  struct Found { Slot* slot{}; Bucket* bucket{}; u32 position{}; };
 public:
  explicit Impl(const Options& options)
      : identity_(next_identity.fetch_add(1)), options_(options),
        memory_(TierBudget::Config{options.dram_node, options.slow_node,
                options.fast_bytes, options.slow_initial_bytes,
                options.slow_growth_bytes, true}) {
    if (!std::isfinite(options.sample_rate) || options.sample_rate < 0 ||
        options.sample_rate > 1)
      throw std::invalid_argument("EH+cache sampling rate must be in [0,1]");
    if (options.fast_bytes < (1ull << 20))
      throw std::invalid_argument("EH+cache needs at least 1 MiB fast memory");
    sample_threshold_ = static_cast<u64>(options.sample_rate * (u64{1} << 53));
    // Reserve 1/4 of the total fast budget for directories, occupancy, and
    // ownership metadata. All backend growth is charged to the same arena;
    // metadata exceeding the remaining fast space spills to the slow node.
    const size_t cache_target = options.fast_bytes - options.fast_bytes / 4;
    const size_t fixed = sizeof(Shard) * kShards + sizeof(Counters) * kStats;
    const size_t per_shard = (cache_target - fixed) / kShards / sizeof(Bucket);
    if (per_shard < 8 || per_shard > UINT32_MAX)
      throw std::invalid_argument("EH+cache cache capacity out of range");
    const u32 total = static_cast<u32>(per_shard);
    const u32 primary = total - std::max(2u, total / 7);
    shards_ = static_cast<Shard*>(memory_.Metadata(sizeof(Shard) * kShards));
    counters_ = static_cast<Counters*>(memory_.Metadata(sizeof(Counters) * kStats));
    for (u32 i = 0; i < kStats; ++i) std::construct_at(counters_ + i);
    auto* pool = static_cast<Bucket*>(
        memory_.Metadata(size_t{total} * kShards * sizeof(Bucket)));
    cache_reserved_ = size_t{total} * kShards * sizeof(Bucket) + fixed;
    for (u32 i = 0; i < kShards; ++i) {
      Shard& shard = *std::construct_at(shards_ + i);
      shard.buckets = pool + size_t{i} * total;
      shard.max_primary = primary;
      shard.total_buckets = total;
      shard.free_overflow = primary;
      for (u32 b = 0; b < total; ++b) {
        Bucket& bucket = *std::construct_at(shard.buckets + b);
        if (b >= primary) bucket.next = b + 1 == total ? kNone : b + 1;
      }
    }
    backend_ = std::make_unique<Backend>(memory_, options.initial_depth);
    LOG("EHCache initialized: cache=partitioned_linear_hash policy=resident_chain_LFU "
        "sample_rate={} counter=exact_FAA write_policy=write_through "
        "tracking=always_on cache_reserved={} {} {}\n", options.sample_rate,
        cache_reserved_, memory_.Describe(), memory_.Placement());
  }

  Result Search(u64 key, u64& value) {
    ThreadState& thread = Local();
    const bool sampled = Sample(thread);
    const u64 hash = TimnahNameSpace::detail::KeyCodec::Hash(key), local_hash = hash >> 8;
    Shard& shard = shards_[hash & (kShards - 1)];
    // Non-sampled hits read payload optimistically, without a write to shared
    // policy state. Sampled hits pin the resident slot with the shard lock so
    // FAA can never accidentally charge a concurrently evicted/reused slot.
    if (!sampled && Optimistic(shard, local_hash, key, value)) {
      Increment(thread.counters->hits);
      return Result::SUCCESS;
    }
    Guard guard(shard);
    const auto found = Find(shard, local_hash, key);
    if (found.slot) {
      value = Read(found.slot->value);
      if (sampled) Count(*found.slot, *thread.counters);
      Increment(thread.counters->hits);
      return Result::SUCCESS;
    }
    Increment(thread.counters->misses);
    // Keep the cache-top lock through the authoritative CXL read and admission.
    // Concurrent Update/Remove thus cannot install a newer value in between.
    u64 stored_key{};
    const auto result = backend_->Search(key, value, &stored_key);
    if (result == Result::SUCCESS) {
      guard.BeginWrite();
      Admit(shard, local_hash, stored_key, value, *thread.counters);
    }
    return result;
  }

  Result Mutate(u64 key, u64 value, int operation) {
    ThreadState& thread = Local();
    const bool sampled = Sample(thread);
    const u64 hash = TimnahNameSpace::detail::KeyCodec::Hash(key), local_hash = hash >> 8;
    Shard& shard = shards_[hash & (kShards - 1)];
    Guard guard(shard);
    guard.BeginWrite();
    // Top-down acquisition and bottom-up release: the DRAM cache lock remains
    // held for the full authoritative slow-tier operation, including splits.
    const auto result = operation == 0 ? backend_->Insert(key, value) :
                        operation == 1 ? backend_->Update(key, value) :
                                         backend_->Remove(key);
    auto found = Find(shard, local_hash, key);
    if (operation == 2) {
      if (found.slot) {
        Write(found.bucket->occupied, static_cast<u16>(
            Read(found.bucket->occupied) & ~(u16{1} << found.position)));
        --shard.entries;
      }
    } else if (result == Result::SUCCESS) {
      if (found.slot) {
        Write(found.slot->value, value);
        if (sampled) Count(*found.slot, *thread.counters);
      } else {
        // An Update query token points to a call-local Header. Cache only the
        // canonical immutable token owned by the authoritative backing table.
        u64 stored_key = key, ignored_value{};
        if (operation != 0 &&
            backend_->Search(key, ignored_value, &stored_key) != Result::SUCCESS)
          throw std::logic_error("EHCache lost a successfully mutated key");
        Admit(shard, local_hash, stored_key, value, *thread.counters);
      }
    }
    return result;
  }

  // Benchmark calls Stat after workers stop. Backend diagnostics serialize its
  // allocation state; cache statistics are gathered under each shard's lock.
  string Stat() {
    u64 entries = 0, splits = 0, active = 0;
    for (u32 i = 0; i < kShards; ++i) {
      Guard guard(shards_[i]);
      entries += shards_[i].entries;
      splits += shards_[i].splits;
      active += shards_[i].base + shards_[i].split;
    }
    u64 hits = 0, misses = 0, samples = 0, admissions = 0, evictions = 0;
    for (u32 i = 0; i < std::min(used_counters_.load(), kStats); ++i) {
      hits += counters_[i].hits.load(std::memory_order_relaxed);
      misses += counters_[i].misses.load(std::memory_order_relaxed);
      samples += counters_[i].samples.load(std::memory_order_relaxed);
      admissions += counters_[i].admissions.load(std::memory_order_relaxed);
      evictions += counters_[i].evictions.load(std::memory_order_relaxed);
    }
    std::ostringstream out;
    out << "EHCache " << backend_->Describe() << " cache_entries=" << entries
        << " cache_hits=" << hits << " cache_misses=" << misses
        << " cache_hit_ratio=" << (hits + misses ? double(hits) / (hits + misses) : 0)
        << " cache_samples=" << samples << " cache_admissions=" << admissions
        << " cache_evictions=" << evictions << " cache_splits=" << splits
        << " cache_active_buckets=" << active << " cache_reserved_bytes=" << cache_reserved_
        << " duplicate_record_bytes=" << entries * 16
        << " sample_rate=" << options_.sample_rate << " tracking=always_on "
        << memory_.Describe() << ' ' << memory_.Placement();
    return out.str();
  }

 private:
  ThreadState& Local() {
    static thread_local ThreadState state;
    if (state.identity != identity_) {
      const u32 index = used_counters_.fetch_add(1);
      if (index >= kStats) throw std::runtime_error("EH+cache supports 512 worker registrations");
      state = {identity_, Hash(identity_ ^ (u64{index} + 1)), counters_ + index};
    }
    return state;
  }
  bool Sample(ThreadState& state) const {
    state.random ^= state.random << 13;
    state.random ^= state.random >> 7;
    state.random ^= state.random << 17;
    return (state.random >> 11) < sample_threshold_;
  }
  static void Increment(std::atomic<u64>& counter) {
    // Exactly one worker owns each diagnostics counter. Atomic load/store
    // permits diagnostic reads without an unnecessary locked RMW per query.
    counter.store(counter.load(std::memory_order_relaxed) + 1,
                  std::memory_order_relaxed);
  }
  static void Count(Slot& slot, Counters& counters) {
    // Exact cumulative LFU counts for the sampled observations. No decay and
    // no convergence freeze are applied to the normal EH+cache baseline.
    auto counter = std::atomic_ref<u64>(slot.frequency);
    if (counter.load(std::memory_order_relaxed) != UINT64_MAX)
      counter.fetch_add(1, std::memory_order_relaxed);
    Increment(counters.samples);
  }
  static u32 Route(const Shard& shard, u64 hash) {
    const u32 base = Read(shard.base), split = Read(shard.split);
    u32 bucket = static_cast<u32>(hash) & (base - 1);
    if (bucket < split) bucket = static_cast<u32>(hash) & (2 * base - 1);
    return bucket;
  }
  static Found Find(Shard& shard, u64 hash, u64 key) {
    for (u32 index = Route(shard, hash); index != kNone;
         index = Read(shard.buckets[index].next)) {
      Bucket& bucket = shard.buckets[index];
      u16 occupied = Read(bucket.occupied);
      while (occupied) {
        const u32 position = std::countr_zero(occupied);
        occupied = static_cast<u16>(occupied & (occupied - 1));
        if (TimnahNameSpace::detail::KeyCodec::Equal(Read(bucket.slots[position].key), key))
          return {bucket.slots + position, &bucket, position};
      }
    }
    return {};
  }
  static bool Optimistic(Shard& shard, u64 hash, u64 key, u64& value) {
    // A changing linear-hash route can temporarily select any allocated bucket,
    // all of which remain alive. Bound traversal to prevent following a reused
    // overflow freelist indefinitely while a split is underway.
    const auto version = shard.version.load();
    if (version & 1) return false;
    Slot* found = nullptr;
    u32 steps = 0;
    for (u32 index = Route(shard, hash); index != kNone && index < shard.total_buckets &&
         steps++ < shard.total_buckets;
         index = Read(shard.buckets[index].next)) {
      Bucket& bucket = shard.buckets[index];
      u16 mask = Read(bucket.occupied);
      while (mask) {
        const u32 position = std::countr_zero(mask);
        mask = static_cast<u16>(mask & (mask - 1));
        if (TimnahNameSpace::detail::KeyCodec::Equal(Read(bucket.slots[position].key), key)) {
          found = bucket.slots + position;
          break;
        }
      }
      if (found) break;
      if (version != shard.version.load()) return false;
    }
    const u64 candidate = found ? Read(found->value) : 0;
    if (version != shard.version.load()) return false;
    if (found) value = candidate;
    return found != nullptr;
  }
  static Slot* Empty(Shard& shard, u32 head) {
    u32 last = head;
    for (u32 index = head; index != kNone; index = Read(shard.buckets[index].next)) {
      Bucket& bucket = shard.buckets[index];
      const u16 mask = Read(bucket.occupied);
      if (mask != ((1u << kSlots) - 1)) {
        const u32 position = std::countr_zero(static_cast<u16>(~mask));
        Write(bucket.occupied, static_cast<u16>(mask | (u16{1} << position)));
        return bucket.slots + position;
      }
      last = index;
    }
    if (shard.free_overflow == kNone) return nullptr;
    const u32 allocated = shard.free_overflow;
    Bucket& bucket = shard.buckets[allocated];
    shard.free_overflow = Read(bucket.next);
    Write(bucket.next, kNone);
    Write(bucket.occupied, u16{1});
    Write(shard.buckets[last].next, allocated);
    return bucket.slots;
  }
  static void Fill(Slot& slot, u64 key, u64 value, u64 frequency) {
    Write(slot.key, key); Write(slot.value, value); Write(slot.frequency, frequency);
  }
  static void Split(Shard& shard, Counters& counters) {
    const u32 old = shard.split, added = shard.base + shard.split;
    // There is no shared cache directory and no doubling copy: the next bucket
    // becomes active and only the split-cursor chain is redistributed.
    for (u32 index = old; index != kNone; index = Read(shard.buckets[index].next)) {
      Bucket& bucket = shard.buckets[index];
      u16 mask = Read(bucket.occupied);
      while (mask) {
        const u32 position = std::countr_zero(mask);
        mask = static_cast<u16>(mask & (mask - 1));
        Slot& slot = bucket.slots[position];
        const u64 key = Read(slot.key);
        if ((static_cast<u32>(TimnahNameSpace::detail::KeyCodec::Hash(key) >> 8) & (2 * shard.base - 1)) != added) continue;
        if (Slot* destination = Empty(shard, added)) {
          Fill(*destination, key, Read(slot.value), Read(slot.frequency));
          Write(bucket.occupied, static_cast<u16>(
              Read(bucket.occupied) & ~(u16{1} << position)));
        } else {
          // Overflow reserve exhaustion may evict a cache copy during growth;
          // the authoritative EH record remains untouched and searchable.
          Write(bucket.occupied, static_cast<u16>(
              Read(bucket.occupied) & ~(u16{1} << position)));
          --shard.entries;
          Increment(counters.evictions);
        }
      }
    }
    u32 previous = old, index = Read(shard.buckets[old].next);
    while (index != kNone) {
      Bucket& bucket = shard.buckets[index];
      const u32 next = Read(bucket.next);
      if (!Read(bucket.occupied)) {
        Write(shard.buckets[previous].next, next);
        Write(bucket.next, shard.free_overflow);
        shard.free_overflow = index;
      } else previous = index;
      index = next;
    }
    const u32 next_split = shard.split + 1;
    if (next_split == shard.base) {
      // Publish base first; an optimistic reader seeing the intermediate state
      // still selects an allocated bucket and then rejects its version.
      Write(shard.base, shard.base * 2);
      Write(shard.split, u32{0});
    } else Write(shard.split, next_split);
    ++shard.splits;
  }
  static void Admit(Shard& shard, u64 hash, u64 key, u64 value, Counters& counters) {
    const u32 active = shard.base + shard.split;
    if (active < shard.max_primary && shard.entries >= size_t{active} * kSlots * 3 / 4)
      Split(shard, counters);
    const u32 head = Route(shard, hash);
    if (Slot* empty = Empty(shard, head)) {
      Fill(*empty, key, value, 1);
      ++shard.entries;
    } else {
      // Bounded LFU candidate set: minimum cumulative sampled frequency in the
      // routed bucket chain. Avoid a global heap/list on every sampled access.
      Slot* victim = nullptr;
      u64 minimum = UINT64_MAX;
      for (u32 index = head; index != kNone; index = Read(shard.buckets[index].next)) {
        Bucket& bucket = shard.buckets[index];
        for (auto& slot : bucket.slots) {
          const u64 frequency = Read(slot.frequency);
          if (!victim || frequency < minimum) { victim = &slot; minimum = frequency; }
        }
      }
      Fill(*victim, key, value, 1);
      Increment(counters.evictions);
    }
    Increment(counters.admissions);
  }

  const u64 identity_;
  Options options_;
  TierBudget::Memory memory_;
  Shard* shards_{};
  Counters* counters_{};
  std::atomic<u32> used_counters_{0};
  size_t cache_reserved_{};
  u64 sample_threshold_{};
  std::unique_ptr<Backend> backend_;
};

EHCache::EHCache() {
  if (FLAGS_ehcache_initial_depth > 24)
    throw std::invalid_argument("EH+cache initial depth must be in [1,24]");
  options_.initial_depth = static_cast<u8>(FLAGS_ehcache_initial_depth);
  options_.dram_node = FLAGS_ehcache_dram_node;
  options_.slow_node = FLAGS_ehcache_slow_node;
  options_.fast_bytes = MiB(FLAGS_ehcache_fast_mib);
  options_.slow_initial_bytes = MiB(FLAGS_ehcache_slow_initial_mib);
  options_.slow_growth_bytes = MiB(FLAGS_ehcache_slow_growth_mib);
  options_.sample_rate = FLAGS_ehcache_sample_rate;
}
EHCache::EHCache(Options options) : options_(options) {}
EHCache::~EHCache() = default;
Result EHCache::Create(const ParamType&) {
  if (impl_) return Result::FAILED;
  impl_ = std::make_unique<Impl>(options_);
  return Result::SUCCESS;
}
void EHCache::Stop() {}
Result EHCache::Insert(const u64& key, const u64& value) {
  return impl_ ? impl_->Mutate(key, value, 0) : Result::FAILED;
}
Result EHCache::Search(const u64& key, u64& value) {
  return impl_ ? impl_->Search(key, value) : Result::FAILED;
}
Result EHCache::Update(const u64& key, const u64& value) {
  return impl_ ? impl_->Mutate(key, value, 1) : Result::FAILED;
}
Result EHCache::Remove(const u64& key) {
  return impl_ ? impl_->Mutate(key, 0, 2) : Result::FAILED;
}
string EHCache::Stat() { return impl_ ? impl_->Stat() : "EHCache not initialized"; }
}  // namespace EHCacheNameSpace
