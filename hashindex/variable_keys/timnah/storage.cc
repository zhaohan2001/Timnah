#include "storage.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#ifdef __linux__
#include <linux/mempolicy.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace TimnahNameSpace::detail {
namespace {
std::size_t Round(std::size_t value, std::size_t alignment) {
  const auto extra = (alignment - value % alignment) % alignment;
  if (value > std::numeric_limits<std::size_t>::max() - extra) throw std::bad_alloc();
  return value + extra;
}
bool TryVersion(u64& word) {
  auto ref = std::atomic_ref<u64>(word);
  auto value = ref.load(std::memory_order_seq_cst);
  return !(value & 1) && ref.compare_exchange_strong(value, value + 1,
                                                    std::memory_order_seq_cst);
}
void UnlockVersion(u64& word) {
  auto ref = std::atomic_ref<u64>(word);
  // Only the successful lock owner modifies this version until publication.
  // Keep the entry CAS and speculative payload/version accesses seq_cst.
  const auto value = ref.load(std::memory_order_relaxed);
  assert(value & 1);
  ref.store(value + 1, std::memory_order_release);
}
u32 NormalizedCount(const CounterBucket& counter, u32 slot, u64 epoch,
                    double decay_factor) {
  const auto old = std::atomic_ref<u64>(const_cast<u64&>(counter.epoch))
                       .load(std::memory_order_relaxed) & ~kCounterBusy;
  const auto value = std::atomic_ref<u32>(const_cast<u32&>(counter.counts[slot]))
                         .load(std::memory_order_relaxed);
  const u64 elapsed = epoch > old ? epoch - old : 0;
  if (!elapsed || decay_factor == 1.0) return value;
  if (decay_factor == 0.5) return elapsed >= 32 ? 0 : value >> elapsed;
  const double decayed = double(value) * std::pow(decay_factor, double(elapsed));
  return decayed <= 0 ? 0 : static_cast<u32>(std::min(decayed, double(value)));
}

// The Entry routing gate is set before this lock set is acquired. New routes
// wait at the gate, and prior structural callers fail Valid() and release their
// try-lock sets. This lets a splitter drain finite in-flight operations instead
// of repeatedly losing a 256-bucket try-lock race under sustained insertion.
class SegmentLocks {
 public:
  ~SegmentLocks() {
    while (count_) {
      auto& item = held_[--count_];
      if (item.key) item.bucket->KeyUnlock();
      if (item.hot) item.bucket->HotUnlock();
      item.bucket->ValueUnlock();
    }
  }
  void Lock(Bucket& bucket) {
    auto& item = held_[count_++];
    item.bucket = &bucket;
    // No high-level operation waits for another bucket while owning these
    // locks: it uses all-or-release try-locks. Single-value updates never wait
    // for a second bucket and promptly finish or fail the route validation.
    while (!bucket.TryValueLock()) std::this_thread::yield();
    while (!bucket.TryHotLock()) std::this_thread::yield();
    item.hot = true;
    while (!bucket.TryKeyLock()) std::this_thread::yield();
    item.key = true;
  }
 private:
  struct Held { Bucket* bucket{}; bool hot{}, key{}; };
  Held held_[kBuckets]{};
  std::size_t count_{};
};
}  // namespace

void Bucket::SetMask(u32 mask) {
  assert(KeyState() & 1);
  assert(!(mask & ~kFullMask));
  const auto state = KeyState();
  Write(key_state, (state & kVersionMask) | (u64(mask & kFullMask) << kMaskShift));
}
bool Bucket::TryKeyLock() {
  auto ref = std::atomic_ref<u64>(key_state);
  auto state = ref.load(std::memory_order_seq_cst);
  if (state & 1) return false;
  const auto locked = (state & ~kVersionMask) | (((state & kVersionMask) + 1) & kVersionMask);
  return ref.compare_exchange_strong(state, locked, std::memory_order_seq_cst);
}
void Bucket::KeyUnlock() {
  auto ref = std::atomic_ref<u64>(key_state);
  auto state = ref.load(std::memory_order_relaxed);
  assert(state & 1);
  const auto unlocked = (state & ~kVersionMask) | (((state & kVersionMask) + 1) & kVersionMask);
  // Publish both the owner's final occupancy mask and payload writes. The
  // seq_cst entry CAS still orders writers against speculative SC readers.
  ref.store(unlocked, std::memory_order_release);
}
bool Bucket::TryValueLock() { return TryVersion(value_version); }
void Bucket::ValueUnlock() { UnlockVersion(value_version); }
bool Bucket::TryHotLock() { return TryVersion(hot_version); }
void Bucket::HotUnlock() { UnlockVersion(hot_version); }

struct Arena::Block {
  void* address{};
  std::size_t bytes{}, used{};
  ~Block() {
    if (!address) return;
#ifdef __linux__
    munmap(address, bytes);
#else
    std::free(address);
#endif
  }
};

Arena::Arena(int node, std::size_t block_bytes, bool prefault, bool huge_pages)
    : node_(node), block_bytes_(block_bytes), prefault_(prefault), huge_pages_(huge_pages) {
  if (node < -1 || !block_bytes) throw std::invalid_argument("Timnah invalid NUMA arena configuration");
#ifdef __linux__
  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) throw std::runtime_error("Timnah cannot determine page size");
  page_size_ = static_cast<std::size_t>(page);
#else
  if (node != -1) throw std::invalid_argument("Timnah NUMA binding requires Linux");
  page_size_ = 4096;
#endif
  block_bytes_ = Round(block_bytes_, page_size_);
}
Arena::~Arena() = default;

void* Arena::Allocate(std::size_t bytes, std::size_t alignment) {
  if (!bytes) return nullptr;
  if (!alignment || !std::has_single_bit(alignment))
    throw std::invalid_argument("Timnah arena alignment must be a power of two");
  std::lock_guard lock(mutex_);
  auto offset = [&](const Block& block) {
    const auto base = reinterpret_cast<std::uintptr_t>(block.address);
    const auto aligned = Round(base + block.used, alignment);
    return static_cast<std::size_t>(aligned - base);
  };
  Block* selected = blocks_.empty() ? nullptr : blocks_.back().get();
  std::size_t start = selected ? offset(*selected) : 0;
  if (!selected || start > selected->bytes || bytes > selected->bytes - start) {
    // mmap/aligned_alloc already align every fresh block to a page. Requests
    // with smaller alignment need no leading slack, including exact-size
    // contiguous tier allocations. Larger alignments still need slack.
    const auto padding = alignment <= page_size_ ? 0 : alignment - 1;
    if (bytes > std::numeric_limits<std::size_t>::max() - padding) throw std::bad_alloc();
    const auto capacity = Round(std::max(block_bytes_, bytes + padding), page_size_);
    auto block = std::make_unique<Block>();
    block->bytes = capacity;
#ifdef __linux__
    block->address = mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block->address == MAP_FAILED) {
      block->address = nullptr;
      throw std::system_error(errno, std::generic_category(), "Timnah mmap");
    }
    if (node_ >= 0) {
      constexpr std::size_t bits = sizeof(unsigned long) * 8;
      std::vector<unsigned long> mask(static_cast<std::size_t>(node_) / bits + 1, 0);
      mask[static_cast<std::size_t>(node_) / bits] |= 1ul << (static_cast<std::size_t>(node_) % bits);
      if (syscall(SYS_mbind, block->address, capacity, MPOL_BIND, mask.data(),
                  static_cast<unsigned long>(mask.size() * bits) + 1, 0) != 0)
        throw std::system_error(errno, std::generic_category(), "Timnah mbind node " + std::to_string(node_));
    }
    // Advisory, private to this index: reduce translation misses on the large
    // random-access arenas without changing their NUMA policy or byte budget.
    // Kernels that cannot provide THP safely retain ordinary pages.
    if (huge_pages_ && capacity >= (std::size_t{2} << 20))
      (void)madvise(block->address, capacity, MADV_HUGEPAGE);
#else
    block->address = std::aligned_alloc(page_size_, capacity);
    if (!block->address) throw std::bad_alloc();
#endif
    if (prefault_) {
      auto* touch = static_cast<volatile unsigned char*>(block->address);
      for (std::size_t i = 0; i < capacity; i += page_size_) touch[i] = 0;
    }
    const auto old_reserved = reserved_.load(std::memory_order_relaxed);
    if (capacity > std::numeric_limits<std::size_t>::max() - old_reserved) throw std::bad_alloc();
    selected = block.get();
    blocks_.push_back(std::move(block));
    reserved_.store(old_reserved + capacity, std::memory_order_relaxed);
    start = offset(*selected);
  }
  const auto consumed = start - selected->used + bytes;
  auto* result = static_cast<std::byte*>(selected->address) + start;
  selected->used = start + bytes;
  used_.fetch_add(consumed, std::memory_order_relaxed);
  return result;
}

Tier::Directory::Directory(u8 d, Arena& memory) : depth(d), size(u32{1} << d) {
  entries = static_cast<std::atomic<Entry*>*>(memory.Allocate(sizeof(*entries) * size));
  for (u32 i = 0; i < size; ++i) std::construct_at(entries + i, nullptr);
}
Tier::Directory::~Directory() {
  for (u32 i = 0; i < size; ++i) std::destroy_at(entries + i);
}

Tier::Tier(int node, u8 initial_depth, bool growable, bool prefault,
           std::function<u64(u64)> hash_fn, bool huge_pages)
    : node_(node), growable_(growable), initial_depth_(initial_depth),
      hash_fn_(std::move(hash_fn)),
      buckets_(node, std::size_t{64} << 20, prefault, huge_pages),
      counters_(node, std::size_t{64} << 20, prefault, huge_pages),
      metadata_(node < 0 ? -1 : 0, std::size_t{2} << 20, prefault, huge_pages) {
  if (initial_depth < 1 || initial_depth > 24 || !hash_fn_)
    throw std::invalid_argument("Timnah invalid initial directory depth or hash function");
  const std::size_t count = std::size_t{1} << initial_depth;
  entries_.reserve(count);
  directories_.reserve(25);
  auto* directory = NewDirectory(initial_depth);
  try {
    CounterBucket* fixed_counts{};
    if (!growable_) {
      constexpr auto bucket_segment_bytes = sizeof(Bucket) * kBuckets;
      constexpr auto counter_segment_bytes = sizeof(CounterBucket) * kBuckets;
      if (count > std::numeric_limits<std::size_t>::max() / bucket_segment_bytes ||
          count > std::numeric_limits<std::size_t>::max() / counter_segment_bytes)
        throw std::bad_alloc();
      fixed_buckets_ = static_cast<Bucket*>(buckets_.Allocate(count * bucket_segment_bytes));
      fixed_counts = static_cast<CounterBucket*>(counters_.Allocate(count * counter_segment_bytes));
    }
    for (u32 i = 0; i < directory->size; ++i) {
      const auto offset = static_cast<std::size_t>(i) * kBuckets;
      auto* entry = growable_ ? NewEntry(initial_depth) :
          NewEntry(initial_depth, 0, fixed_buckets_ + offset, fixed_counts + offset);
      directory->entries[i].store(entry, std::memory_order_seq_cst);
    }
    directories_.push_back(directory);
    current_.store(directory, std::memory_order_seq_cst);
  } catch (...) {
    std::destroy_at(directory);
    for (auto* entry : entries_) {
      for (u32 b = 0; b < kBuckets; ++b) {
        std::destroy_at(entry->buckets[b].counters);
        std::destroy_at(entry->buckets + b);
      }
      std::destroy_at(entry);
    }
    throw;
  }
}
Tier::~Tier() {
  for (auto* directory : directories_) std::destroy_at(directory);
  for (auto* entry : entries_) {
    for (u32 b = 0; b < kBuckets; ++b) {
      std::destroy_at(entry->buckets[b].counters);
      std::destroy_at(entry->buckets + b);
    }
    std::destroy_at(entry);
  }
}
Tier::Directory* Tier::NewDirectory(u8 depth) {
  auto* memory = metadata_.Allocate(sizeof(Directory));
  return std::construct_at(static_cast<Directory*>(memory), depth, metadata_);
}
Entry* Tier::NewEntry(u8 depth, u64 epoch, Bucket* memory, CounterBucket* counts) {
  auto* entry_memory = static_cast<Entry*>(metadata_.Allocate(sizeof(Entry)));
  if (!memory) memory = static_cast<Bucket*>(buckets_.Allocate(sizeof(Bucket) * kBuckets));
  if (!counts) counts = static_cast<CounterBucket*>(counters_.Allocate(sizeof(CounterBucket) * kBuckets));
  // All allocations are complete before any nontrivial construction is started.
  auto* entry = std::construct_at(entry_memory);
  entry->buckets = memory;
  entry->depth.store(depth, std::memory_order_relaxed);
  for (u32 b = 0; b < kBuckets; ++b) {
    std::construct_at(memory + b);
    auto* counter = std::construct_at(counts + b);
    counter->epoch = epoch & ~kCounterBusy;
    memory[b].counters = counter;
  }
  entries_.push_back(entry);
  segment_count_.store(entries_.size(), std::memory_order_relaxed);
  return entry;
}
u8 Tier::DirectoryDepth() const {
  return current_.load(std::memory_order_seq_cst)->depth;
}
std::vector<Entry*> Tier::EntriesSnapshot() const {
  std::lock_guard lock(directory_mutex_);
  return entries_;
}

bool Tier::Split(u64 hash, Entry* expected, u64 epoch, double decay_factor) {
  if (!growable_) return false;
  if (!(decay_factor > 0 && decay_factor <= 1) || !std::isfinite(decay_factor))
    throw std::invalid_argument("Timnah invalid counter decay factor");
  std::unique_lock directory_lock(directory_mutex_, std::try_to_lock);
  if (!directory_lock.owns_lock()) return false;
  auto* directory = current_.load(std::memory_order_seq_cst);
  if (!expected || directory->Get(hash) != expected) return false;
  const auto old_depth = expected->depth.load(std::memory_order_seq_cst);
  struct SplitGate {
    Entry* entry;
    explicit SplitGate(Entry* value) : entry(value) {
      entry->splitting.store(true, std::memory_order_seq_cst);
    }
    ~SplitGate() { entry->splitting.store(false, std::memory_order_seq_cst); }
  } gate(expected);
  // The gate precedes locks in scope so it is reopened only after all locks
  // have been released, including allocation failures and early returns.
  SegmentLocks locks;
  for (u32 b = 0; b < kBuckets; ++b) locks.Lock(expected->buckets[b]);
  if (expected->buckets[hash & (kBuckets - 1)].Mask() != kFullMask) return false;
  if (old_depth >= 24) throw std::overflow_error("Timnah maximum directory depth reached");

  struct Record { u64 key{}, value{}; u32 count{}, bucket{}; bool right{}; };
  std::vector<Record> records;
  records.reserve(kBuckets * kSlots);
  epoch &= ~kCounterBusy;
  for (u32 b = 0; b < kBuckets; ++b) {
    const auto& bucket = expected->buckets[b];
    auto mask = bucket.Mask();
    while (mask) {
      const auto slot = static_cast<u32>(std::countr_zero(mask));
      mask &= mask - 1;
      const auto key = Read(bucket.slots[slot].key);
      records.push_back({key, Read(bucket.slots[slot].value),
                         NormalizedCount(*bucket.counters, slot, epoch, decay_factor), b,
                         bool((hash_fn_(key) >> (63 - old_depth)) & 1)});
    }
  }
  // No KV/counter/directory mutation happens until all throwing work is done.
  // Preserve amortized growth while ensuring push_back cannot allocate after
  // mutation begins. Reserving exactly size+1 at every split would be quadratic.
  if (entries_.size() == entries_.capacity())
    entries_.reserve(std::max(entries_.size() + 1, entries_.capacity() * 2));
  directories_.reserve(directories_.size() + 1);
  Directory* doubled = nullptr;
  if (old_depth == directory->depth) {
    doubled = NewDirectory(directory->depth + 1);
    for (u32 i = 0; i < directory->size; ++i) {
      auto* entry = directory->entries[i].load(std::memory_order_seq_cst);
      doubled->entries[2 * i].store(entry, std::memory_order_seq_cst);
      doubled->entries[2 * i + 1].store(entry, std::memory_order_seq_cst);
    }
  }
  Entry* right{};
  try { right = NewEntry(old_depth + 1, epoch); }
  catch (...) { if (doubled) std::destroy_at(doubled); throw; }
  auto* published = doubled ? doubled : directory;

  for (u32 b = 0; b < kBuckets; ++b) {
    auto& bucket = expected->buckets[b];
    bucket.SetMask(0);
    for (u32 i = 0; i < kSlots; ++i)
      std::atomic_ref<u32>(bucket.counters->counts[i]).store(0, std::memory_order_relaxed);
    std::atomic_ref<u64>(bucket.counters->epoch).store(epoch, std::memory_order_relaxed);
  }
  expected->depth.store(old_depth + 1, std::memory_order_seq_cst);
  for (const auto& record : records) {
    auto* target = record.right ? right : expected;
    auto& bucket = target->buckets[record.bucket];
    // right is not published and does not need a Key Lock yet.
    const auto mask = bucket.Mask();
    const auto slot = static_cast<u32>(std::countr_zero((~mask) & kFullMask));
    assert(slot < kSlots);
    Write(bucket.slots[slot].key, record.key);
    Write(bucket.slots[slot].value, record.value);
    std::atomic_ref<u32>(bucket.counters->counts[slot]).store(record.count, std::memory_order_relaxed);
    if (target == expected) bucket.SetMask(mask | (u32{1} << slot));
    else Write(bucket.key_state, u64(mask | (u32{1} << slot)) << Bucket::kMaskShift);
  }
  const u32 base = static_cast<u32>(hash >> (64 - old_depth)) << (published->depth - old_depth);
  const u32 half = u32{1} << (published->depth - old_depth - 1);
  for (u32 i = base + half; i < base + 2 * half; ++i)
    published->entries[i].store(right, std::memory_order_seq_cst);
  if (doubled) {
    directories_.push_back(doubled);
    current_.store(doubled, std::memory_order_seq_cst);
  }
  splits_.fetch_add(1, std::memory_order_relaxed);
  return true;
}
}  // namespace TimnahNameSpace::detail
