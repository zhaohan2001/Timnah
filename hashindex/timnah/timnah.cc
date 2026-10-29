#include "timnah.h"
#include "storage.h"
#include "key_codec.h"
#include "sampling.h"
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <limits>
#include <memory_resource>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_set>

DEFINE_uint32(timnah_fast_depth, 12, "Timnah fixed fast-tier initial directory depth");
DEFINE_uint32(timnah_slow_depth, 14, "Timnah growable slow-tier initial directory depth");
DEFINE_int32(timnah_fast_node, 0, "Timnah DRAM NUMA node; -1 ordinary allocation");
DEFINE_int32(timnah_slow_node, 2, "Timnah simulated CXL NUMA node; -1 ordinary allocation");
DEFINE_double(timnah_sample_rate, 0.1, "Timnah approximate hotness sampling probability");
DEFINE_double(timnah_intra_multiplier, 2.0, "Timnah preceding-slot reorder multiplier");
DEFINE_double(timnah_cross_multiplier, 2.0, "Timnah coldest-fast-slot promotion multiplier");
DEFINE_uint64(timnah_decay_ms, 1000, "Timnah counter decay epoch duration in milliseconds");
DEFINE_double(timnah_decay_factor, 0.5, "Timnah counter decay factor per epoch");
DEFINE_uint64(timnah_queue_capacity, 1024, "Timnah bounded ring capacity per foreground thread");
DEFINE_bool(timnah_huge_pages, true, "Timnah private arenas request transparent huge pages; NUMA policy and byte capacity unchanged");
DEFINE_double(timnah_convergence_threshold, 0.002, "Timnah adjacent-round tier-hit-rate L1 distance below this freezes heat/migration; 0 disables");

namespace TimnahNameSpace {
using namespace detail;
namespace {
std::atomic<u64> next_instance_id{1};

// One foreground thread owns each field. Snapshot may read concurrently, so
// keep atomic accesses without paying for an unnecessary locked read-modify-write.
void LocalIncrement(std::atomic<u64>& counter) {
  counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

u32 ApproxRead(const u32& n) {
  return std::atomic_ref<u32>(const_cast<u32&>(n)).load(std::memory_order_relaxed);
}
void ApproxWrite(u32& n, u32 value) {
  std::atomic_ref<u32>(n).store(value, std::memory_order_relaxed);
}
u64 CounterEpoch(const CounterBucket& c) {
  return std::atomic_ref<u64>(const_cast<u64&>(c.epoch)).load(std::memory_order_acquire);
}
u32 Scale(u32 value, u64 delta, double factor) {
  if (!delta || !value || factor == 1) return value;
  if (factor == 0.5) return delta >= 32 ? 0 : value >> delta;
  return static_cast<u32>(double(value) * std::pow(factor, double(delta)));
}

// All structural operations use try-locks and release the entire set on failure.
// No bucket Key Lock is held while waiting for a Value/Hot Lock or directory lock.
class StructuralLocks {
 public:
  ~StructuralLocks() { Release(); }
  bool Try(std::initializer_list<std::pair<Bucket*, bool>> buckets) {
    for (const auto& [bucket, hot] : buckets) {
      if (!bucket) continue;
      u32 i = 0;
      while (i < count_ && items_[i].bucket != bucket) ++i;
      if (i < count_) items_[i].want_hot |= hot;
      else items_[count_++] = {bucket, hot};
    }
    std::sort(items_.begin(), items_.begin() + count_,
              [](const Item& a, const Item& b) { return std::less<Bucket*>{}(a.bucket, b.bucket); });
    for (u32 i = 0; i < count_; ++i) {
      if (!items_[i].bucket->TryValueLock()) return false;
      items_[i].value = true;
    }
    for (u32 i = 0; i < count_; ++i) if (items_[i].want_hot) {
      if (!items_[i].bucket->TryHotLock()) return false;
      items_[i].hot = true;
    }
    for (u32 i = 0; i < count_; ++i) {
      if (!items_[i].bucket->TryKeyLock()) return false;
      items_[i].key = true;
    }
    return true;
  }
  void Release() {
    for (u32 i = count_; i > 0; --i) if (items_[i-1].key) {
      items_[i-1].bucket->KeyUnlock(); items_[i-1].key = false;
    }
    for (u32 i = count_; i > 0; --i) if (items_[i-1].hot) {
      items_[i-1].bucket->HotUnlock(); items_[i-1].hot = false;
    }
    for (u32 i = count_; i > 0; --i) if (items_[i-1].value) {
      items_[i-1].bucket->ValueUnlock(); items_[i-1].value = false;
    }
    count_ = 0;
  }
 private:
  struct Item { Bucket* bucket{}; bool want_hot{}, value{}, hot{}, key{}; };
  std::array<Item, 3> items_{};
  u32 count_{};
};
struct ValueGuard {
  Bucket* bucket{}; bool hot{};
  bool Try(Bucket& b, bool h) {
    hot = h;
    if (!(hot ? b.TryHotLock() : b.TryValueLock())) return false;
    bucket = &b; return true;
  }
  void Release() {
    if (bucket) { if (hot) bucket->HotUnlock(); else bucket->ValueUnlock(); bucket = nullptr; }
  }
  ~ValueGuard() { Release(); }
};
struct Lookup {
  Route route{};
  u64 version{}, value{}, stored_key{};
  int slot{-1};
  bool stable{};
};
struct WorkItem { u64 key{}, hash{}; };
struct alignas(64) ThreadState {
  explicit ThreadState(size_t capacity, u64 seed)
      : random(seed ? seed : 1), ring(capacity), pending(&pending_pool) {
    pending.reserve(capacity);
  }
  std::atomic<u64> fast_hits{0}, slow_hits{0}, misses{0}, samples{0}, enqueued{0}, dropped{0};
  std::atomic<u64> busy_dropped{0}, full_dropped{0}, deduplicated{0}, reorders{0};
  u64 random;
  alignas(64) std::mutex mutex;
  std::vector<WorkItem> ring;
  size_t head{}, tail{}, count{};
  std::pmr::unsynchronized_pool_resource pending_pool;
  std::pmr::unordered_set<u64> pending;
};
}  // namespace

class Timnah::Impl {
 public:
  explicit Impl(Options options, u64 num_bg)
      : options_(options), id_(next_instance_id.fetch_add(1)),
        sample_threshold_(SamplingThreshold(options.sample_rate)),
        started_(std::chrono::steady_clock::now()), worker_count_(num_bg) {
    if (options_.variable_keys) codec_ = std::make_unique<KeyCodec>(options_.slow_node, options_.prefault, options_.huge_pages);
    auto hash = [this](u64 key) { return Hash(key); };
    fast_ = std::make_unique<Tier>(options_.fast_node, options_.fast_depth, false, options_.prefault, hash, options_.huge_pages);
    slow_ = std::make_unique<Tier>(options_.slow_node, options_.slow_depth, true, options_.prefault, hash, options_.huge_pages);
    try {
      for (u64 i = 0; i < num_bg; ++i) workers_.emplace_back([this, i] { Worker(i); });
    } catch (...) { Stop(); throw; }
  }
  ~Impl() { Stop(); }
  u64 Hash(u64 key) const { return codec_ ? KeyCodec::Hash(key) : HashFunction(key); }
  bool Equal(u64 a, u64 b) const { return codec_ ? KeyCodec::Equal(a, b) : a == b; }
  bool Tracking() const {
    return tracking_active_.load(std::memory_order_acquire) && !stopped_.load(std::memory_order_relaxed);
  }
  u64 Epoch() const { return epoch_.load(std::memory_order_relaxed); }
  void Tick() {
    if (!Tracking()) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_).count();
    const u64 desired = u64(elapsed) / options_.decay_interval_ms + manual_epochs_.load(std::memory_order_relaxed);
    u64 old = epoch_.load(std::memory_order_relaxed);
    while (old < desired && !epoch_.compare_exchange_weak(old, desired, std::memory_order_relaxed)) {}
    if (old < desired) decay_events_.fetch_add(desired - old, std::memory_order_relaxed);
  }
  bool Normalize(Bucket& bucket, u64 epoch) {
    auto& counts = *bucket.counters;
    auto ref = std::atomic_ref<u64>(counts.epoch);
    u64 previous = ref.load(std::memory_order_acquire);
    if (previous & kCounterBusy) return false;
    if (previous >= epoch) return true;
    if (!ref.compare_exchange_strong(previous, previous | kCounterBusy,
                                    std::memory_order_acq_rel)) return false;
    for (auto& count : counts.counts) ApproxWrite(count, Scale(ApproxRead(count), epoch - previous, options_.decay_factor));
    ref.store(epoch, std::memory_order_release);
    return true;
  }
  u32 Score(const Bucket& bucket, u32 slot, u64 epoch) const {
    const auto& counts = *bucket.counters;
    const u64 previous = CounterEpoch(counts);
    if (previous & kCounterBusy) return 0;
    return Scale(ApproxRead(counts.counts[slot]), epoch > previous ? epoch - previous : 0, options_.decay_factor);
  }
  ThreadState& State() {
    struct Cache {
      u64 id{}; ThreadState* state{};
      std::unordered_map<u64, std::pair<std::weak_ptr<int>, ThreadState*>> by_instance;
    };
    static thread_local Cache cache;
    if (cache.id == id_) return *cache.state;
    if (auto found = cache.by_instance.find(id_); found != cache.by_instance.end()) {
      cache.id = id_; cache.state = found->second.second; return *cache.state;
    }
    if (cache.by_instance.size() >= 64) {
      for (auto it = cache.by_instance.begin(); it != cache.by_instance.end();) {
        if (it->second.first.expired()) it = cache.by_instance.erase(it); else ++it;
      }
    }
    auto state = std::make_unique<ThreadState>(options_.queue_capacity,
        HashFunction(id_ ^ u64(std::hash<std::thread::id>{}(std::this_thread::get_id()))));
    auto* result = state.get();
    {
      std::lock_guard lock(states_mutex_);
      states_.push_back(std::move(state));
      states_generation_.fetch_add(1, std::memory_order_release);
    }
    cache.by_instance.emplace(id_, std::make_pair(std::weak_ptr<int>(lifetime_), result));
    cache.id = id_; cache.state = result; return *result;
  }
  std::vector<ThreadState*> States() const {
    std::lock_guard lock(states_mutex_);
    std::vector<ThreadState*> result;
    result.reserve(states_.size());
    for (const auto& state : states_) result.push_back(state.get());
    return result;
  }
  bool Sample(ThreadState& state) const {
    return Sample53(state.random, sample_threshold_);
  }
  int FindHeld(const Bucket& bucket, u64 key) const {
    auto mask = bucket.Mask();
    while (mask) {
      const u32 slot = std::countr_zero(mask); mask &= mask - 1;
      if (Equal(Read(bucket.slots[slot].key), key)) return int(slot);
    }
    return -1;
  }
  template <bool bytes>
  Lookup FindKey(Tier& tier, u64 hash, u64 key, bool fast) const {
    Lookup result;
    result.route = tier.Locate(hash);
    const auto& bucket = *result.route.bucket;
    result.version = bucket.KeyState();
    if (result.version & 1) return result;
    auto mask = Bucket::Mask(result.version);
    while (mask) {
      const u32 slot = std::countr_zero(mask); mask &= mask - 1;
      const u64 stored = Read(bucket.slots[slot].key);
      if constexpr (bytes) { if (!KeyCodec::Equal(stored, key)) continue; }
      else { if (stored != key) continue; }
      const auto& word = fast && slot == 0 ? bucket.hot_version : bucket.value_version;
      const auto value_version = Read(word);
      if (value_version & 1) return result;
      result.value = Read(bucket.slots[slot].value);
      if (value_version != Read(word)) return result;
      result.slot = int(slot); result.stored_key = stored;
      break;
    }
    // The fixed fast directory never changes. Its contents still need the
    // complete Key/Value snapshot validation, including after a cross-tier miss.
    result.stable = result.version == bucket.KeyState() && (fast || tier.Valid(result.route, hash));
    return result;
  }
  Lookup Find(Tier& tier, u64 hash, u64 key, bool fast) const {
    return codec_ ? FindKey<true>(tier, hash, key, fast) : FindKey<false>(tier, hash, key, fast);
  }
  bool ValidFastMiss(const Lookup& result, u64 hash) const {
    (void)hash;
    return result.stable && result.version == result.route.bucket->KeyState();
  }
  auto Search(u64 key, u64& value) -> Result {
    const u64 hash = Hash(key);
    // After convergence the read path no longer visits thread-local tracking
    // state or updates diagnostic hit counters, matching SB's frozen policy.
    auto* state = Tracking() ? &State() : nullptr;
    for (;;) {
      auto fast = Find(*fast_, hash, key, true);
      if (!fast.stable) { std::this_thread::yield(); continue; }
      if (fast.slot >= 0) {
        if (state) LocalIncrement(state->fast_hits);
        value = fast.value;
        if (state) Observe(*state, fast, hash, true);
        return Result::SUCCESS;
      }
      auto slow = Find(*slow_, hash, key, false);
      if (!slow.stable || !ValidFastMiss(fast, hash)) continue;
      if (slow.slot >= 0) {
        if (state) LocalIncrement(state->slow_hits);
        value = slow.value;
        if (state) Observe(*state, slow, hash, false);
        return Result::SUCCESS;
      }
      if (state) LocalIncrement(state->misses);
      return Result::FAILED;
    }
  }
  void Put(Bucket& bucket, u32 slot, u64 key, u64 value, u32 count) {
    Write(bucket.slots[slot].key, key); Write(bucket.slots[slot].value, value);
    ApproxWrite(bucket.counters->counts[slot], count);
    bucket.SetMask(bucket.Mask() | (u32{1} << slot));
  }
  void Erase(Bucket& bucket, u32 slot) {
    bucket.SetMask(bucket.Mask() & ~(u32{1} << slot));
    ApproxWrite(bucket.counters->counts[slot], 0);
  }
  auto Insert(u64 key, u64 value) -> Result {
    const auto hash = Hash(key);
    for (;;) {
      auto f = fast_->Locate(hash), s = slow_->Locate(hash);
      StructuralLocks locks;
      if (!locks.Try({{f.bucket, true}, {s.bucket, false}})) { std::this_thread::yield(); continue; }
      if (!slow_->Valid(s, hash)) continue;
      if (FindHeld(*f.bucket, key) >= 0 || FindHeld(*s.bucket, key) >= 0) return Result::FAILED;
      Bucket* target = f.bucket->Mask() != kFullMask ? f.bucket : (s.bucket->Mask() != kFullMask ? s.bucket : nullptr);
      if (target) {
        const auto epoch = Epoch();
        if (!Normalize(*target, epoch)) continue;
        const auto slot = std::countr_zero((~target->Mask()) & kFullMask);
        Put(*target, slot, key, value, 0);
        size_.fetch_add(1, std::memory_order_relaxed); return Result::SUCCESS;
      }
      locks.Release();
      slow_->Split(hash, s.entry, Epoch(), options_.decay_factor);
    }
  }
  auto Update(u64 key, u64 value) -> Result {
    const auto hash = Hash(key);
    for (;;) {
      auto f = Find(*fast_, hash, key, true);
      if (!f.stable) continue;
      bool in_fast = f.slot >= 0;
      auto found = in_fast ? f : Find(*slow_, hash, key, false);
      if (!found.stable || (!in_fast && !ValidFastMiss(f, hash))) continue;
      if (found.slot < 0) return Result::FAILED;
      ValueGuard guard;
      if (!guard.Try(*found.route.bucket, in_fast && found.slot == 0)) { std::this_thread::yield(); continue; }
      if (found.version != found.route.bucket->KeyState() ||
          (!in_fast && !slow_->Valid(found.route, hash)) ||
          (!in_fast && !ValidFastMiss(f, hash)) ||
          !Equal(Read(found.route.bucket->slots[found.slot].key), key)) continue;
      Write(found.route.bucket->slots[found.slot].value, value);
      guard.Release();
      if (Tracking()) Observe(State(), found, hash, in_fast);
      return Result::SUCCESS;
    }
  }
  auto Remove(u64 key) -> Result {
    const auto hash = Hash(key);
    for (;;) {
      auto f = fast_->Locate(hash), s = slow_->Locate(hash);
      StructuralLocks locks;
      if (!locks.Try({{f.bucket, true}, {s.bucket, false}})) { std::this_thread::yield(); continue; }
      if (!slow_->Valid(s, hash)) continue;
      const auto a = FindHeld(*f.bucket, key), b = FindHeld(*s.bucket, key);
      if (a < 0 && b < 0) return Result::FAILED;
      if (a >= 0) Erase(*f.bucket, u32(a)); else Erase(*s.bucket, u32(b));
      size_.fetch_sub(1, std::memory_order_relaxed); return Result::SUCCESS;
    }
  }
  bool ReorderHeld(Bucket& bucket, u32 slot, u64 epoch, ThreadState* foreground = nullptr) {
    if (slot == 0 || !(bucket.Mask() & (u32{1} << slot))) return false;
    const u32 count = Score(bucket, slot, epoch);
    u32 target = slot;
    const bool first_valid = bucket.Mask() & 1;
    if (count > (first_valid ? Score(bucket, 0, epoch) : 0)) target = 0;
    else if (count > options_.intra_multiplier * ((bucket.Mask() & (u32{1} << (slot - 1))) ? Score(bucket, slot - 1, epoch) : 0)) target = slot - 1;
    if (target == slot) return false;
    const bool other_valid = bucket.Mask() & (u32{1} << target);
    const auto key = Read(bucket.slots[slot].key), value = Read(bucket.slots[slot].value);
    const auto other_key = Read(bucket.slots[target].key), other_value = Read(bucket.slots[target].value);
    const auto other_count = Score(bucket, target, epoch);
    Write(bucket.slots[target].key, key); Write(bucket.slots[target].value, value);
    Write(bucket.slots[slot].key, other_key); Write(bucket.slots[slot].value, other_value);
    ApproxWrite(bucket.counters->counts[target], count);
    ApproxWrite(bucket.counters->counts[slot], other_valid ? other_count : 0);
    auto mask = bucket.Mask() | (u32{1} << target);
    if (!other_valid) mask &= ~(u32{1} << slot);
    bucket.SetMask(mask);
    if (foreground) LocalIncrement(foreground->reorders);
    else reorders_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  bool Reorder(Tier& tier, const Route& route, u64 hash, u64 key, bool fast, ThreadState* foreground = nullptr) {
    StructuralLocks locks;
    // A potential first-slot swap holds Hot as well. This conservative acquisition
    // is confined to sampled structural movement, not ordinary value updates.
    if (!locks.Try({{route.bucket, fast}}) || (!fast && !tier.Valid(route, hash))) return false;
    const auto slot = FindHeld(*route.bucket, key);
    const auto epoch = Epoch();
    if (slot < 0 || !Normalize(*route.bucket, epoch)) return false;
    return ReorderHeld(*route.bucket, u32(slot), epoch, foreground);
  }
  struct Heat { u32 maximum{}, minimum{}, cold_slot{}; bool free{}, valid{true}; };
  Heat HeatOf(const Bucket& bucket, u64 epoch) const {
    Heat result;
    const auto& counters = *bucket.counters;
    const auto previous = CounterEpoch(counters);
    if (previous & kCounterBusy) { result.valid = false; return result; }
    const auto delta = epoch > previous ? epoch - previous : 0;
    const auto mask = bucket.Mask();
    result.free = mask != kFullMask;
    result.minimum = result.free ? 0 : std::numeric_limits<u32>::max();
    if (result.free) result.cold_slot = std::countr_zero((~mask) & kFullMask);
    auto occupied = mask;
    while (occupied) {
      const auto slot = std::countr_zero(occupied); occupied &= occupied - 1;
      const auto score = Scale(ApproxRead(counters.counts[slot]), delta, options_.decay_factor);
      result.maximum = std::max(result.maximum, score);
      if (!result.free && score < result.minimum) { result.minimum = score; result.cold_slot = slot; }
    }
    result.valid = previous == CounterEpoch(counters);
    return result;
  }
  bool Eligible(u32 score, const Heat& fast) const {
    return fast.valid && (score > fast.maximum || score > options_.cross_multiplier * fast.minimum);
  }
  void Enqueue(ThreadState& state, WorkItem item) {
    if (!worker_count_ || !Tracking()) return;
    std::unique_lock lock(state.mutex, std::try_to_lock);
    if (!lock.owns_lock()) { LocalIncrement(state.dropped); LocalIncrement(state.busy_dropped); return; }
    if (state.count == state.ring.size()) { LocalIncrement(state.dropped); LocalIncrement(state.full_dropped); return; }
    if (state.pending.contains(item.key)) { LocalIncrement(state.deduplicated); return; }
    const bool was_empty = state.count == 0;
    state.pending.insert(item.key);
    state.ring[state.tail] = item; state.tail = (state.tail + 1) % state.ring.size(); ++state.count;
    queue_pending_.fetch_add(1, std::memory_order_relaxed);
    LocalIncrement(state.enqueued);
    lock.unlock();
    if (was_empty) {
      if (worker_count_ == 1) wake_cv_.notify_one(); else wake_cv_.notify_all();
    }
  }
  void Observe(ThreadState& state, const Lookup& found, u64 hash, bool fast) {
    if (!Tracking() || !Sample(state)) return;
    // A live worker refreshes the shared epoch at every bounded batch. With no
    // worker, sampled foreground operations retain wall-clock decay themselves.
    if (!worker_count_) Tick();
    auto& tier = fast ? *fast_ : *slow_;
    auto& bucket = *found.route.bucket;
    if (found.version != bucket.KeyState() || (!fast && !tier.Valid(found.route, hash))) return;
    const auto epoch = Epoch();
    if (!Normalize(bucket, epoch)) return;
    const auto slot = u32(found.slot);
    auto& counter = bucket.counters->counts[slot];
    const auto old = ApproxRead(counter);
    // Deliberately not fetch_add: lost increments are acceptable approximate
    // heat, but all accesses remain atomic under the C++ memory model.
    ApproxWrite(counter, old == std::numeric_limits<u32>::max() ? old : old + 1);
    LocalIncrement(state.samples);
    if (found.version != bucket.KeyState() || (!fast && !tier.Valid(found.route, hash))) return;
    const auto score = Score(bucket, slot, epoch);
    bool reorder = slot > 0 && (score > ((Bucket::Mask(found.version) & 1) ? Score(bucket, 0, epoch) : 0) ||
        score > options_.intra_multiplier * Score(bucket, slot - 1, epoch));
    if (reorder) Reorder(tier, found.route, hash, found.stored_key, fast, &state);
    if (!fast && worker_count_) {
      auto route = fast_->Locate(hash);
      const auto version = route.bucket->KeyState();
      if (!(version & 1) && Eligible(score, HeatOf(*route.bucket, epoch)) &&
          version == route.bucket->KeyState()) {
        // stored_key, not the ephemeral tagged Query header used by byte APIs.
        Enqueue(state, {found.stored_key, hash});
      }
    }
  }
  bool Promote(u64 key, u64 hash) {
    for (u32 attempt = 0; attempt < 8; ++attempt) {
      auto f = fast_->Locate(hash), s = slow_->Locate(hash);
      const auto fv = f.bucket->KeyState(), sv = s.bucket->KeyState();
      if ((fv | sv) & 1) return false;
      const auto source_slot = FindHeld(*s.bucket, key);
      if (source_slot < 0 || FindHeld(*f.bucket, key) >= 0) return false;
      const auto epoch = Epoch();
      auto heat = HeatOf(*f.bucket, epoch);
      const auto score = Score(*s.bucket, u32(source_slot), epoch);
      if (!Eligible(score, heat)) return false;
      const bool victim = !heat.free;
      const auto victim_key = victim ? Read(f.bucket->slots[heat.cold_slot].key) : 0;
      const auto victim_hash = victim ? Hash(victim_key) : 0;
      Route destination = victim ? slow_->Locate(victim_hash) : Route{};
      StructuralLocks locks;
      if (!locks.Try({{f.bucket, true}, {s.bucket, false}, {destination.bucket, false}})) return false;
      if (!slow_->Valid(s, hash) ||
          (victim && !slow_->Valid(destination, victim_hash))) continue;
      if (!Normalize(*f.bucket, epoch) || !Normalize(*s.bucket, epoch) ||
          (victim && !Normalize(*destination.bucket, epoch))) return false;
      const auto source = FindHeld(*s.bucket, key);
      if (source < 0 || FindHeld(*f.bucket, key) >= 0) return false;
      heat = HeatOf(*f.bucket, epoch);
      const auto hot_count = Score(*s.bucket, u32(source), epoch);
      if (!Eligible(hot_count, heat)) return false;
      if (heat.free != !victim) continue;
      const auto target = heat.cold_slot;
      if (victim && !Equal(Read(f.bucket->slots[target].key), victim_key)) continue;
      if (victim && destination.bucket != s.bucket && destination.bucket->Mask() == kFullMask) {
        locks.Release();
        slow_->Split(victim_hash, destination.entry, epoch, options_.decay_factor);
        continue;
      }
      const auto hot_key = Read(s.bucket->slots[source].key), hot_value = Read(s.bucket->slots[source].value);
      const auto cold_value = victim ? Read(f.bucket->slots[target].value) : 0;
      const auto cold_count = victim ? Score(*f.bucket, target, epoch) : 0;
      // All participating Key Locks remain odd while ownership changes.
      // Clear validity before publishing the new location: no duplicate live KV.
      Erase(*s.bucket, u32(source));
      if (victim) Erase(*f.bucket, target);
      Put(*f.bucket, target, hot_key, hot_value, hot_count);
      if (victim) {
        const auto free = std::countr_zero((~destination.bucket->Mask()) & kFullMask);
        Put(*destination.bucket, free, victim_key, cold_value, cold_count);
        demotions_.fetch_add(1, std::memory_order_relaxed);
      }
      ReorderHeld(*f.bucket, target, epoch);
      promotions_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }
  bool Maintain(u64 key, u64 hash) {
    if (!Tracking()) return false;
    Tick();
    const auto fast = Find(*fast_, hash, key, true);
    if (!fast.stable) return false;
    if (fast.slot >= 0) return Reorder(*fast_, fast.route, hash, fast.stored_key, true);
    if (!ValidFastMiss(fast, hash)) return false;
    return Promote(key, hash);
  }
  u32 PopBatch(ThreadState& state, std::array<WorkItem, 16>& items) {
    std::unique_lock lock(state.mutex, std::try_to_lock);
    if (!lock.owns_lock() || !state.count) return 0;
    const auto count = u32(std::min(state.count, items.size()));
    for (u32 i = 0; i < count; ++i) {
      items[i] = state.ring[state.head];
      state.head = (state.head + 1) % state.ring.size();
      state.pending.erase(items[i].key);
    }
    state.count -= count;
    in_flight_.fetch_add(count, std::memory_order_relaxed);
    queue_pending_.fetch_sub(count, std::memory_order_release); return count;
  }
  void RecordFailure() {
    std::lock_guard lock(failure_mutex_);
    if (!failure_) failure_ = std::current_exception();
  }
  void ExecuteBatch(const std::array<WorkItem, 16>& items, u32 count) {
    try {
      std::shared_lock gate(maintenance_gate_);
      // Requests name slow-tier promotion candidates. If another queue already
      // promoted this key, Promote revalidates and skips the stale request.
      // One bounded batch shares this gate. Freeze waits for its completion;
      // a batch obtaining the gate after Freeze skips every item but still
      // returns its complete in-flight accounting below. Stop still drains.
      for (u32 i = 0; i < count; ++i) {
        if (!tracking_active_.load(std::memory_order_acquire)) break;
        try { Promote(items[i].key, items[i].hash); }
        catch (...) { RecordFailure(); }
      }
    } catch (...) {
      RecordFailure();
    }
    in_flight_.fetch_sub(count, std::memory_order_release);
  }
  void Worker(u64 worker) {
    std::vector<ThreadState*> states;
    u64 generation = 0;
    for (;;) {
      // Every popped batch finishes its in-flight accounting, even when frozen.
      if (!tracking_active_.load(std::memory_order_acquire)) break;
      Tick(); bool did_work = false;
      const auto current_generation = states_generation_.load(std::memory_order_acquire);
      if (current_generation != generation) {
        states = States(); generation = current_generation;
      }
      for (size_t i = worker; i < states.size(); i += worker_count_) {
        std::array<WorkItem, 16> items;
        const auto count = PopBatch(*states[i], items);
        if (!count) continue;
        Tick(); did_work = true;
        ExecuteBatch(items, count);
      }
      if (stopped_.load(std::memory_order_acquire) && !queue_pending_.load(std::memory_order_acquire)) break;
      if (!did_work) {
        std::unique_lock lock(wake_mutex_);
        wake_cv_.wait_for(lock, std::chrono::microseconds(250));
      }
    }
  }
  void CheckFailure() const {
    std::lock_guard lock(failure_mutex_);
    if (failure_) std::rethrow_exception(failure_);
  }
  void JoinWorkers() {
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    workers_.clear();
  }
  void Stop() {
    std::lock_guard control(control_mutex_);
    if (stopped_.exchange(true, std::memory_order_acq_rel)) return;
    wake_cv_.notify_all();
    JoinWorkers();
  }
  void FreezeMaintenance() {
    // Obtain the allocating snapshot before changing state. With quiescent
    // foreground callers it includes every producer and remains complete.
    const auto states = States();
    {
      std::unique_lock gate(maintenance_gate_);
      tracking_active_.store(false, std::memory_order_release);
      for (auto* state : states) {
        std::lock_guard lock(state->mutex);
        const auto queued = state->count;
        state->head = state->tail = state->count = 0;
        state->pending.clear();
        queue_pending_.fetch_sub(queued, std::memory_order_release);
      }
    }
    // Never join under the gate: previously popped batches need a shared gate
    // to skip their items and complete the in-flight accounting.
    wake_cv_.notify_all();
    JoinWorkers();
    CheckFailure();
    if (queue_pending_.load() || in_flight_.load())
      throw std::logic_error("Timnah convergence left unfinished maintenance");
    converged_.store(true, std::memory_order_release);
  }
  Diagnostics FinishRound() {
    std::lock_guard control(control_mutex_);
    if (!Tracking()) return Snapshot();
    const auto before = Snapshot();
    const auto fast = before.fast_hits - previous_fast_hits_;
    const auto slow = before.slow_hits - previous_slow_hits_;
    previous_fast_hits_ = before.fast_hits;
    previous_slow_hits_ = before.slow_hits;
    const auto hits = fast + slow;
    if (!hits) {
      previous_round_valid_ = false;
      convergence_diff_.store(-1, std::memory_order_relaxed);
      return Snapshot();
    }
    const double fast_rate = double(fast) / hits, slow_rate = double(slow) / hits;
    const double difference = previous_round_valid_
        ? std::abs(fast_rate - last_round_fast_rate_.load(std::memory_order_relaxed)) +
          std::abs(slow_rate - last_round_slow_rate_.load(std::memory_order_relaxed))
        : -1;
    last_round_fast_rate_.store(fast_rate, std::memory_order_relaxed);
    last_round_slow_rate_.store(slow_rate, std::memory_order_relaxed);
    convergence_diff_.store(difference, std::memory_order_relaxed);
    const auto round = observed_rounds_.fetch_add(1, std::memory_order_relaxed);
    const bool freeze = previous_round_valid_ && options_.convergence_threshold > 0 &&
                        difference < options_.convergence_threshold;
    previous_round_valid_ = true;
    if (freeze) {
      FreezeMaintenance();
      LOG("Timnah convergence round={} fast_rate={:.9f} slow_rate={:.9f} l1_diff={:.9f} threshold={:.9f} action=freeze_heat_and_migration\n",
          round, fast_rate, slow_rate, difference, options_.convergence_threshold);
    }
    return Snapshot();
  }
  bool Drain(u64 timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      CheckFailure();
      if (!queue_pending_.load(std::memory_order_acquire) && !in_flight_.load(std::memory_order_acquire)) {
        CheckFailure(); return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) return false;
      wake_cv_.notify_all(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  Diagnostics Snapshot() const {
    CheckFailure(); Diagnostics result;
    result.size = size_.load(std::memory_order_relaxed);
    for (const auto* state : States()) {
      result.fast_hits += state->fast_hits.load(std::memory_order_relaxed);
      result.slow_hits += state->slow_hits.load(std::memory_order_relaxed);
      result.misses += state->misses.load(std::memory_order_relaxed);
      result.samples += state->samples.load(std::memory_order_relaxed);
      result.queue_enqueued += state->enqueued.load(std::memory_order_relaxed);
      result.queue_dropped += state->dropped.load(std::memory_order_relaxed);
      result.queue_busy_dropped += state->busy_dropped.load(std::memory_order_relaxed);
      result.queue_full_dropped += state->full_dropped.load(std::memory_order_relaxed);
      result.queue_deduplicated += state->deduplicated.load(std::memory_order_relaxed);
      result.reorders += state->reorders.load(std::memory_order_relaxed);
    }
    result.reorders += reorders_.load(); result.promotions = promotions_.load(); result.demotions = demotions_.load();
    result.slow_splits = slow_->SplitCount(); result.decay_events = decay_events_.load();
    result.queue_pending = queue_pending_.load(); result.in_flight = in_flight_.load();
    result.fast_segments = fast_->SegmentCount(); result.slow_segments = slow_->SegmentCount();
    result.fast_bucket_bytes = fast_->BucketBytes(); result.fast_counter_bytes = fast_->CounterBytes();
    result.slow_bucket_bytes = slow_->BucketBytes(); result.slow_counter_bytes = slow_->CounterBytes();
    result.variable_key_bytes = codec_ ? codec_->UsedBytes() : 0;
    result.epoch = Epoch(); result.tracking_active = Tracking();
    result.converged = converged_.load(std::memory_order_acquire);
    result.observed_rounds = observed_rounds_.load(std::memory_order_relaxed);
    result.last_round_fast_rate = last_round_fast_rate_.load(std::memory_order_relaxed);
    result.last_round_slow_rate = last_round_slow_rate_.load(std::memory_order_relaxed);
    result.convergence_diff = convergence_diff_.load(std::memory_order_relaxed);
    return result;
  }
  Inspection Inspect(u64 key) {
    std::unique_lock gate(maintenance_gate_);
    Tick(); const auto hash = Hash(key); Inspection result;
    for (int tier = 0; tier < 2; ++tier) {
      auto& table = tier == 0 ? *fast_ : *slow_;
      auto found = Find(table, hash, key, tier == 0);
      while (!found.stable) { std::this_thread::yield(); found = Find(table, hash, key, tier == 0); }
      if (found.slot >= 0) {
        ++result.copies;
        if (!result.found) {
          result.found = true; result.tier = tier; result.slot = u32(found.slot); result.value = found.value;
          result.counter = Score(*found.route.bucket, u32(found.slot), Epoch());
        }
      }
    }
    return result;
  }
  bool SetCounter(u64 key, u32 count) {
    std::unique_lock gate(maintenance_gate_);
    if (!Tracking()) return false;
    Tick(); const auto hash = Hash(key);
    for (auto* table : {fast_.get(), slow_.get()}) {
      auto route = table->Locate(hash);
      StructuralLocks locks;
      if (!locks.Try({{route.bucket, table == fast_.get()}}) || !table->Valid(route, hash)) continue;
      const auto slot = FindHeld(*route.bucket, key);
      if (slot < 0) continue;
      if (!Normalize(*route.bucket, Epoch())) return false;
      ApproxWrite(route.bucket->counters->counts[slot], count); return true;
    }
    return false;
  }
  bool Validate() {
    if (!Drain(30000)) return false;
    std::unique_lock gate(maintenance_gate_);
    std::unordered_set<u64> numeric;
    std::unordered_map<u64, std::vector<u64>> bytes;
    u64 count = 0;
    for (auto* table : {fast_.get(), slow_.get()}) for (auto* entry : table->EntriesSnapshot()) {
      for (u32 b = 0; b < kBuckets; ++b) {
        auto& bucket = entry->buckets[b];
        if (bucket.KeyState() & 1) return false;
        auto mask = bucket.Mask();
        while (mask) {
          const auto slot = std::countr_zero(mask); mask &= mask - 1;
          const auto key = Read(bucket.slots[slot].key), hash = Hash(key);
          const auto route = table->Locate(hash);
          if (route.entry != entry || route.bucket != &bucket) return false;
          if (codec_) {
            auto& group = bytes[hash];
            for (auto other : group) if (Equal(key, other)) return false;
            group.push_back(key);
          } else if (!numeric.insert(key).second) return false;
          ++count;
        }
      }
    }
    return count == size_.load();
  }

  Options options_;
  const u64 id_;
  const u64 sample_threshold_;
  std::shared_ptr<int> lifetime_{std::make_shared<int>(0)};
  std::unique_ptr<KeyCodec> codec_;
  std::unique_ptr<Tier> fast_, slow_;
  const std::chrono::steady_clock::time_point started_;
  const u64 worker_count_;
  mutable std::mutex states_mutex_, failure_mutex_;
  std::mutex control_mutex_;
  std::vector<std::unique_ptr<ThreadState>> states_;
  std::atomic<u64> states_generation_{0};
  std::exception_ptr failure_;
  std::shared_mutex maintenance_gate_;
  std::mutex wake_mutex_;
  std::condition_variable wake_cv_;
  std::vector<std::jthread> workers_;
  std::atomic<bool> stopped_{false};
  std::atomic<bool> tracking_active_{true}, converged_{false};
  std::atomic<u64> observed_rounds_{0};
  std::atomic<double> last_round_fast_rate_{0}, last_round_slow_rate_{0}, convergence_diff_{-1};
  // Only FinishRound, serialized by control_mutex_, writes these baselines.
  u64 previous_fast_hits_{0}, previous_slow_hits_{0};
  bool previous_round_valid_{false};
  std::atomic<u64> size_{0};
  alignas(64) std::atomic<u64> epoch_{0}, manual_epochs_{0}, decay_events_{0};
  alignas(64) std::atomic<u64> reorders_{0}, promotions_{0}, demotions_{0};
  alignas(64) std::atomic<u64> queue_pending_{0}, in_flight_{0};
};

Timnah::Timnah() : Timnah(Options{
    static_cast<u8>(std::min(FLAGS_timnah_fast_depth, 255u)),
    static_cast<u8>(std::min(FLAGS_timnah_slow_depth, 255u)),
    FLAGS_timnah_fast_node, FLAGS_timnah_slow_node,
    FLAGS_timnah_sample_rate, FLAGS_timnah_intra_multiplier, FLAGS_timnah_cross_multiplier,
    FLAGS_timnah_decay_ms, FLAGS_timnah_decay_factor, FLAGS_timnah_queue_capacity,
    true, false, FLAGS_timnah_huge_pages, FLAGS_timnah_convergence_threshold}) {}
Timnah::Timnah(Options options) : options_(options) {}
Timnah::~Timnah() = default;
auto Timnah::Create(const ParamType& param) -> Result {
  if (impl_ || options_.fast_depth < 1 || options_.fast_depth > 24 || options_.slow_depth < 1 || options_.slow_depth > 24 ||
      options_.fast_node < -1 || options_.slow_node < -1 || !options_.queue_capacity || !options_.decay_interval_ms || param.num_bg > 256 ||
      !std::isfinite(options_.sample_rate) || options_.sample_rate < 0 || options_.sample_rate > 1 ||
      !std::isfinite(options_.decay_factor) || options_.decay_factor <= 0 || options_.decay_factor > 1 ||
      !std::isfinite(options_.intra_multiplier) || options_.intra_multiplier < 1 ||
      !std::isfinite(options_.cross_multiplier) || options_.cross_multiplier < 1 ||
      !std::isfinite(options_.convergence_threshold) || options_.convergence_threshold < 0 ||
      options_.convergence_threshold > 2) return Result::FAILED;
  impl_ = std::make_unique<Impl>(options_, param.num_bg);
  const auto d = impl_->Snapshot();
  LOG("Timnah memory_initial fast_node={} slow_node={} fast_bucket_bytes={} fast_counter_bytes={} slow_bucket_bytes={} slow_counter_bytes={} sample_rate={} decay_ms={} decay_factor={} convergence_threshold={} queue_capacity={} huge_page_advice={}\n",
      options_.fast_node, options_.slow_node, d.fast_bucket_bytes, d.fast_counter_bytes,
      d.slow_bucket_bytes, d.slow_counter_bytes, options_.sample_rate, options_.decay_interval_ms,
      options_.decay_factor, options_.convergence_threshold, options_.queue_capacity, int(options_.huge_pages));
  return Result::SUCCESS;
}
auto Timnah::Stop() -> void { if (impl_) impl_->Stop(); }
auto Timnah::Insert(const u64& key, const u64& value) -> Result {
  return impl_ && !options_.variable_keys ? impl_->Insert(key, value) : Result::FAILED;
}
auto Timnah::Search(const u64& key, u64& value) -> Result {
  return impl_ && !options_.variable_keys ? impl_->Search(key, value) : Result::FAILED;
}
auto Timnah::Update(const u64& key, const u64& value) -> Result {
  return impl_ && !options_.variable_keys ? impl_->Update(key, value) : Result::FAILED;
}
auto Timnah::Remove(const u64& key) -> Result {
  return impl_ && !options_.variable_keys ? impl_->Remove(key) : Result::FAILED;
}
auto Timnah::InsertBytes(std::string_view key, u64 value) -> Result {
  if (!impl_ || !options_.variable_keys) return Result::FAILED;
  return impl_->Insert(impl_->codec_->MakeStored(key), value);
}
auto Timnah::SearchBytes(std::string_view key, u64& value) -> Result {
  if (!impl_ || !options_.variable_keys) return Result::FAILED;
  auto query = KeyCodec::MakeQuery(key); return impl_->Search(query.Token(), value);
}
auto Timnah::UpdateBytes(std::string_view key, u64 value) -> Result {
  if (!impl_ || !options_.variable_keys) return Result::FAILED;
  auto query = KeyCodec::MakeQuery(key); return impl_->Update(query.Token(), value);
}
auto Timnah::RemoveBytes(std::string_view key) -> Result {
  if (!impl_ || !options_.variable_keys) return Result::FAILED;
  auto query = KeyCodec::MakeQuery(key); return impl_->Remove(query.Token());
}
auto Timnah::Snapshot() const -> Diagnostics { return impl_ ? impl_->Snapshot() : Diagnostics{}; }
auto Timnah::Inspect(u64 key) -> Inspection { return impl_ && !options_.variable_keys ? impl_->Inspect(key) : Inspection{}; }
auto Timnah::Validate() -> bool { return impl_ && impl_->Validate(); }
auto Timnah::DrainMaintenance(u64 timeout_ms) -> bool { return impl_ && impl_->Drain(timeout_ms); }
auto Timnah::DebugSetCounter(u64 key, u32 count) -> bool { return impl_ && !options_.variable_keys && impl_->SetCounter(key, count); }
auto Timnah::DebugMaintain(u64 key) -> bool {
  if (!impl_ || options_.variable_keys) return false;
  std::shared_lock gate(impl_->maintenance_gate_); return impl_->Maintain(key, impl_->Hash(key));
}
auto Timnah::DebugAdvanceDecay(u64 epochs) -> void {
  if (impl_) {
    std::shared_lock gate(impl_->maintenance_gate_);
    if (impl_->Tracking()) { impl_->manual_epochs_.fetch_add(epochs); impl_->Tick(); }
  }
}
auto Timnah::Stat() -> string {
  if (!impl_) return "Timnah not created\n";
  const auto d = impl_->FinishRound(); const auto requests = d.fast_hits + d.slow_hits + d.misses;
  return fmt::format("Timnah size={} fast_hits={} slow_hits={} misses={} fast_hit_rate={:.6f} samples={} reorders={} promotions={} demotions={} slow_splits={} epoch={} decay_events={} tracking={} converged={} observed_rounds={} last_round_fast_rate={:.9f} last_round_slow_rate={:.9f} convergence_diff={:.9f} rates_frozen={} queue_pending={} in_flight={} queue_dropped={} queue_enqueued={} queue_busy_dropped={} queue_full_dropped={} queue_deduplicated={} fast_segments={} slow_segments={} fast_bucket_bytes={} fast_counter_bytes={} slow_bucket_bytes={} slow_counter_bytes={} variable_key_bytes={} fast_node={} slow_node={}\n",
      d.size, d.fast_hits, d.slow_hits, d.misses, requests ? double(d.fast_hits)/requests : 0.0,
      d.samples, d.reorders, d.promotions, d.demotions, d.slow_splits, d.epoch, d.decay_events,
      int(d.tracking_active), int(d.converged), d.observed_rounds, d.last_round_fast_rate,
      d.last_round_slow_rate, d.convergence_diff, int(!d.tracking_active),
      d.queue_pending, d.in_flight, d.queue_dropped, d.queue_enqueued,
      d.queue_busy_dropped, d.queue_full_dropped, d.queue_deduplicated, d.fast_segments, d.slow_segments,
      d.fast_bucket_bytes, d.fast_counter_bytes, d.slow_bucket_bytes, d.slow_counter_bytes,
      d.variable_key_bytes, options_.fast_node, options_.slow_node);
}
}  // namespace TimnahNameSpace
