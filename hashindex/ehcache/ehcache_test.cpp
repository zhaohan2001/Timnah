#include "ehcache.h"

#include <atomic>
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using EHCacheNameSpace::EHCache;

namespace {
// Explicit checks stay enabled in the same Release build used by the benchmark.
void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void Join(std::vector<std::thread>& workers) {
  for (auto& worker : workers) worker.join();
  workers.clear();
}

EHCache::Options TestOptions(double sample_rate = 1.0) {
  EHCache::Options options;
  options.initial_depth = 1;
  options.dram_node = -1;
  options.slow_node = -1;
  options.fast_bytes = 4ull << 20;
  options.slow_initial_bytes = 8ull << 20;
  options.slow_growth_bytes = 4ull << 20;
  options.sample_rate = sample_rate;
  return options;
}

void Check(EHCache& table, u64 key, u64 expected) {
  u64 value = ~expected;
  Require(table.Search(key, value) == Result::SUCCESS,
          "Search failed for key " + std::to_string(key));
  Require(value == expected, "Wrong value for key " + std::to_string(key) +
                                ": expected " + std::to_string(expected) +
                                ", received " + std::to_string(value));
}

void Missing(EHCache& table, u64 key) {
  u64 value = 42;
  Require(table.Search(key, value) == Result::FAILED,
          "Removed/missing key was found: " + std::to_string(key));
}

u64 Counter(EHCache& table, const std::string& name) {
  const auto stats = table.Stat();
  const auto position = stats.find(name + "=");
  Require(position != std::string::npos,
          "Missing diagnostic counter " + name + " in " + stats);
  return std::stoull(stats.substr(position + name.size() + 1));
}

void LifecycleAndFullWidthKeys() {
  EHCache table(TestOptions());
  Missing(table, 0);
  Require(table.Insert(0, 0) == Result::FAILED, "Insert before Create succeeded");
  Require(table.Update(0, 0) == Result::FAILED, "Update before Create succeeded");
  Require(table.Remove(0) == Result::FAILED, "Remove before Create succeeded");
  Require(table.Create({}) == Result::SUCCESS, "Create failed");
  Require(table.Create({}) == Result::FAILED, "Second Create succeeded");

  const u64 keys[] = {0, 1, 1ull << 63, UINT64_MAX};
  for (const auto key : keys) {
    Require(table.Insert(key, ~key) == Result::SUCCESS, "Insert failed");
    Require(table.Insert(key, 19) == Result::FAILED, "Duplicate Insert succeeded");
    for (int warm = 0; warm < 32; ++warm) Check(table, key, ~key);
    Require(table.Update(key, key) == Result::SUCCESS, "Update failed");
    for (int probe = 0; probe < 32; ++probe) Check(table, key, key);
    Require(table.Remove(key) == Result::SUCCESS, "Remove failed");
    Require(table.Remove(key) == Result::FAILED, "Duplicate Remove succeeded");
    Require(table.Update(key, 0) == Result::FAILED, "Missing Update succeeded");
    for (int probe = 0; probe < 32; ++probe) Missing(table, key);
    Require(table.Insert(key, 7) == Result::SUCCESS, "Reinsert failed");
    for (int probe = 0; probe < 32; ++probe) Check(table, key, 7);
  }
  table.Stop();
  table.Stop();
}

void CachedMutationCoherence() {
  EHCache table(TestOptions());
  Require(table.Create({}) == Result::SUCCESS, "Create failed");
  constexpr u64 key = 711;
  Require(table.Insert(key, 0) == Result::SUCCESS, "Insert failed");
  // Sampling at 100% makes repeated reads exercise admitted cache entries.
  for (u64 version = 1; version <= 2000; ++version) {
    for (int warm = 0; warm < 8; ++warm) Check(table, key, version - 1);
    Require(table.Update(key, version) == Result::SUCCESS, "Cached Update failed");
    for (int probe = 0; probe < 8; ++probe) Check(table, key, version);
    Require(table.Remove(key) == Result::SUCCESS, "Cached Remove failed");
    Missing(table, key);
    Require(table.Insert(key, version) == Result::SUCCESS, "Cached reinsert failed");
    Check(table, key, version);
  }
  Require(Counter(table, "cache_hits") > 0, "Warm probes never hit the cache");
  Require(Counter(table, "cache_admissions") > 0, "Warm probes never admitted a key");
  Require(Counter(table, "cache_entries") > 0, "Cache did not retain a warm key");
}

void BoundedCacheGrowthEvictionAndCoherence() {
  auto options = TestOptions();
  options.fast_bytes = 1ull << 20;
  options.slow_initial_bytes = 1ull << 20;
  options.slow_growth_bytes = 1ull << 20;
  EHCache table(options);
  Require(table.Create({}) == Result::SUCCESS, "Create with bounded cache failed");
  constexpr u64 records = 100000, original_mask = 0x123456789abcdef0ull;
  constexpr u64 updated_mask = 0x0fedcba987654321ull;
  // One MiB cannot hold the 100k cache copies. Inserts populate every cache
  // shard, while subsequent scans force misses, incremental splits and LFU
  // replacement. Every copy must continue to agree with authoritative EH data.
  for (u64 key = 0; key < records; ++key)
    Require(table.Insert(key, key ^ original_mask) == Result::SUCCESS,
            "Cache-pressure Insert failed");
  for (int pass = 0; pass < 2; ++pass)
    for (u64 key = 0; key < records; ++key) Check(table, key, key ^ original_mask);
  Require(Counter(table, "cache_splits") > 0, "Cache never exercised linear-hash splits");
  Require(Counter(table, "cache_evictions") > 0, "Bounded cache never evicted a record");
  Require(Counter(table, "cache_entries") < records,
          "One MiB cache unexpectedly retained all 100k records");
  Require(Counter(table, "fast_reserved") == options.fast_bytes,
          "Fast-tier reservation changed under cache pressure");
  Require(Counter(table, "fast_used") <= options.fast_bytes,
          "Cache growth exceeded the total fast-tier budget");
  for (u64 key = 0; key < records; key += 7)
    Require(table.Update(key, key ^ updated_mask) == Result::SUCCESS,
            "Cache-pressure Update failed");
  for (u64 key = 0; key < records; key += 11) {
    Require(table.Remove(key) == Result::SUCCESS, "Cache-pressure Remove failed");
    Missing(table, key);
    Require(table.Insert(key, key + 17) == Result::SUCCESS,
            "Cache-pressure Reinsert failed");
  }
  for (u64 key = 0; key < records; ++key) {
    const u64 expected = key % 11 == 0 ? key + 17 :
                        key % 7 == 0 ? key ^ updated_mask : key ^ original_mask;
    Check(table, key, expected);
  }
  Require(Counter(table, "fast_used") <= options.fast_bytes,
          "Mutation under cache pressure exceeded the fast-tier budget");
  std::cout << table.Stat() << '\n';
}

void SplitGrowthAndDifferentialCRUD() {
  auto options = TestOptions();
  options.slow_initial_bytes = 64ull << 10;
  options.slow_growth_bytes = 64ull << 10;
  EHCache table(options);
  Require(table.Create({}) == Result::SUCCESS, "Create failed");
  std::unordered_map<u64, u64> expected;
  constexpr u64 initial_records = 40000;
  for (u64 key = 0; key < initial_records; ++key) {
    const auto value = key ^ 0xa55a123456789abcull;
    Require(table.Insert(key, value) == Result::SUCCESS, "Split-load Insert failed");
    expected.emplace(key, value);
  }
  for (const auto& [key, value] : expected) Check(table, key, value);
  for (u64 key = 0; key < initial_records; key += 2) {
    Require(table.Remove(key) == Result::SUCCESS, "Split-load Remove failed");
    expected.erase(key);
  }
  std::mt19937_64 random(1729);
  for (u32 iteration = 0; iteration < 60000; ++iteration) {
    const auto key = random() % 60000;
    const auto value = random();
    const auto found = expected.find(key);
    const bool exists = found != expected.end();
    switch (random() % 4) {
      case 0:
        Require(table.Insert(key, value) == (exists ? Result::FAILED : Result::SUCCESS),
                "Differential Insert disagreed with map");
        if (!exists) expected.emplace(key, value);
        break;
      case 1:
        Require(table.Update(key, value) == (exists ? Result::SUCCESS : Result::FAILED),
                "Differential Update disagreed with map");
        if (exists) found->second = value;
        break;
      case 2:
        Require(table.Remove(key) == (exists ? Result::SUCCESS : Result::FAILED),
                "Differential Remove disagreed with map");
        expected.erase(key);
        break;
      default:
        if (exists) Check(table, key, found->second);
        else Missing(table, key);
    }
  }
  for (u64 key = 0; key < 60000; ++key) {
    const auto found = expected.find(key);
    if (found == expected.end()) Missing(table, key);
    else Check(table, key, found->second);
  }
  Require(Counter(table, "eh_splits") > 0, "Stress load never split the backing EH");
  Require(Counter(table, "slow_growth_count") > 0, "Stress load never grew the slow pool");
  Require(Counter(table, "fast_used") <= Counter(table, "fast_reserved"),
          "Cache/metadata exceeded the fast-tier budget");
  std::cout << table.Stat();
}

void ConcurrentInsertAndSplitsAtRate(double sample_rate) {
  EHCache table(TestOptions(sample_rate));
  Require(table.Create({}) == Result::SUCCESS, "Create failed");
  constexpr u32 threads = 12, per_thread = 2000;
  std::barrier start(threads);
  std::atomic<u64> failures{0}, duplicate_winners{0};
  std::vector<std::thread> workers;
  for (u32 tid = 0; tid < threads; ++tid) workers.emplace_back([&, tid] {
    start.arrive_and_wait();
    if (table.Insert(UINT64_MAX, 42) == Result::SUCCESS) ++duplicate_winners;
    for (u64 index = 0; index < per_thread; ++index) {
      const auto key = tid * per_thread + index;
      u64 value{};
      if (table.Insert(key, key + 1) != Result::SUCCESS ||
          table.Search(key, value) != Result::SUCCESS || value != key + 1 ||
          table.Update(key, key + 2) != Result::SUCCESS) ++failures;
    }
  });
  Join(workers);
  Require(failures.load() == 0, "Concurrent split/insert/update failed");
  Require(duplicate_winners.load() == 1, "Duplicate Insert did not have exactly one winner");
  Check(table, UINT64_MAX, 42);
  for (u64 key = 0; key < threads * per_thread; ++key) Check(table, key, key + 2);
}

void ConcurrentInsertAndSplits() {
  for (const auto sample_rate : {0.0, 0.1, 1.0})
    ConcurrentInsertAndSplitsAtRate(sample_rate);
}

void ReadersObserveMonotonicHotKeyUpdatesAtRate(double sample_rate) {
  EHCache table(TestOptions(sample_rate));
  Require(table.Create({}) == Result::SUCCESS, "Create failed");
  constexpr u64 key = 521, updates = 50000;
  Require(table.Insert(key, 0) == Result::SUCCESS, "Insert failed");
  for (int warm = 0; warm < 100; ++warm) Check(table, key, 0);
  constexpr u32 readers = 8;
  std::barrier start(readers + 1);
  std::atomic<bool> done{false};
  std::atomic<u64> failures{0}, reads{0};
  std::vector<std::thread> workers;
  for (u32 tid = 0; tid < readers; ++tid) workers.emplace_back([&] {
    u64 last{};
    start.arrive_and_wait();
    do {
      u64 value{};
      if (table.Search(key, value) != Result::SUCCESS || value < last || value > updates)
        ++failures;
      last = value;
      ++reads;
    } while (!done.load(std::memory_order_acquire));
  });
  start.arrive_and_wait();
  for (u64 version = 1; version <= updates; ++version)
    if (table.Update(key, version) != Result::SUCCESS) ++failures;
  done.store(true, std::memory_order_release);
  Join(workers);
  Require(failures.load() == 0, "Cached read regressed or returned torn/missing value");
  Require(reads.load() >= readers, "Concurrent read loop was not exercised");
  for (int probe = 0; probe < 1000; ++probe) Check(table, key, updates);
}

void ReadersObserveMonotonicHotKeyUpdates() {
  for (const auto sample_rate : {0.0, 0.1, 1.0})
    ReadersObserveMonotonicHotKeyUpdatesAtRate(sample_rate);
}

void ConcurrentAdmissionsDeletesAndReinsertsAtRate(double sample_rate) {
  EHCache table(TestOptions(sample_rate));
  Require(table.Create({}) == Result::SUCCESS, "Create failed");
  constexpr u32 keys = 32, readers = 8, writers = 4;
  constexpr u64 versions = 3000, value_stride = 1ull << 32;
  for (u64 key = 0; key < keys; ++key) {
    Require(table.Insert(key, key) == Result::SUCCESS, "Insert failed");
    for (int warm = 0; warm < 32; ++warm) Check(table, key, key);
  }
  std::barrier start(readers + writers);
  std::atomic<u32> writers_remaining{writers};
  std::atomic<u64> failures{0}, reads{0};
  std::vector<std::thread> workers;
  for (u32 tid = 0; tid < readers; ++tid) workers.emplace_back([&, tid] {
    std::mt19937_64 random(1729 + tid);
    u64 last_version[keys]{};
    start.arrive_and_wait();
    do {
      const auto key = random() % keys;
      u64 value{};
      const auto result = table.Search(key, value);
      if (result == Result::SUCCESS) {
        const auto version = value / value_stride;
        if (value % value_stride != key || version < last_version[key] ||
            version > versions * 2) ++failures;
        last_version[key] = version;
      } else if (result != Result::FAILED) ++failures;
      ++reads;
    } while (writers_remaining.load(std::memory_order_acquire));
  });
  for (u32 tid = 0; tid < writers; ++tid) workers.emplace_back([&, tid] {
    start.arrive_and_wait();
    // A single writer owns each key, so observed versions cannot legitimately
    // go backwards, even when a reader overlaps deletion and re-insertion.
    for (u64 iteration = 1; iteration <= versions; ++iteration) {
      const auto first_version = iteration * 2 - 1;
      for (u64 key = tid; key < keys; key += writers) {
        if (table.Remove(key) != Result::SUCCESS ||
            table.Insert(key, first_version * value_stride + key) != Result::SUCCESS ||
            table.Update(key, (first_version + 1) * value_stride + key) != Result::SUCCESS)
          ++failures;
      }
    }
    writers_remaining.fetch_sub(1, std::memory_order_release);
  });
  Join(workers);
  Require(failures.load() == 0, "Stale admission after Update/Remove/Reinsert race");
  Require(reads.load() >= readers, "Admission race was not exercised");
  for (u64 key = 0; key < keys; ++key)
    for (int probe = 0; probe < 100; ++probe)
      Check(table, key, versions * 2 * value_stride + key);
}

void ConcurrentAdmissionsDeletesAndReinserts() {
  for (const auto sample_rate : {0.0, 0.1, 1.0})
    ConcurrentAdmissionsDeletesAndReinsertsAtRate(sample_rate);
}

void OptimisticReadersDuringCacheSplitsAndEvictions() {
  auto options = TestOptions(0.1);
  options.fast_bytes = 1ull << 20;
  options.slow_initial_bytes = 1ull << 20;
  options.slow_growth_bytes = 1ull << 20;
  EHCache table(options);
  Require(table.Create({}) == Result::SUCCESS, "Create for optimistic cache growth failed");
  constexpr u64 stable_keys = 1024, records = 100000;
  constexpr u32 readers = 4;
  for (u64 key = 0; key < stable_keys; ++key)
    Require(table.Insert(key, ~key) == Result::SUCCESS, "Stable-key Insert failed");
  std::barrier start(readers + 1);
  std::atomic<bool> done{false};
  std::atomic<u64> failures{0}, reads{0};
  std::vector<std::thread> workers;
  for (u32 tid = 0; tid < readers; ++tid) workers.emplace_back([&, tid] {
    std::mt19937_64 random(1729 + tid);
    start.arrive_and_wait();
    do {
      const auto key = random() % stable_keys;
      u64 value{};
      if (table.Search(key, value) != Result::SUCCESS || value != ~key) ++failures;
      ++reads;
    } while (!done.load(std::memory_order_acquire));
  });
  start.arrive_and_wait();
  for (u64 key = stable_keys; key < records; ++key)
    if (table.Insert(key, ~key) != Result::SUCCESS) ++failures;
  done.store(true, std::memory_order_release);
  Join(workers);
  Require(failures.load() == 0, "Optimistic Search failed during cache splits/evictions");
  Require(reads.load() >= readers, "Optimistic cache-growth readers were not exercised");
  Require(Counter(table, "cache_splits") > 0, "Concurrent cache growth did not split");
  Require(Counter(table, "cache_evictions") > 0, "Concurrent cache growth did not evict");
  for (u64 key = 0; key < stable_keys; ++key) Check(table, key, ~key);
}

void SamplingDisabledStillHasCorrectCRUD() {
  EHCache table(TestOptions(0.0));
  Require(table.Create({}) == Result::SUCCESS, "Create with sampling disabled failed");
  for (u64 key = 0; key < 5000; ++key)
    Require(table.Insert(key, ~key) == Result::SUCCESS, "Unsampled Insert failed");
  for (u64 key = 0; key < 5000; ++key) Check(table, key, ~key);
  for (u64 key = 0; key < 5000; key += 3) {
    Require(table.Update(key, key) == Result::SUCCESS, "Unsampled Update failed");
    Check(table, key, key);
    Require(table.Remove(key) == Result::SUCCESS, "Unsampled Remove failed");
    Missing(table, key);
  }
  Require(Counter(table, "cache_samples") == 0,
          "Sampling-disabled table still counted a cache sample");
}
}  // namespace

int main(int argc, char** argv) {
  const std::pair<const char*, void (*)()> tests[] = {
      {"LifecycleAndFullWidthKeys", LifecycleAndFullWidthKeys},
      {"CachedMutationCoherence", CachedMutationCoherence},
      {"BoundedCacheGrowthEvictionAndCoherence", BoundedCacheGrowthEvictionAndCoherence},
      {"SplitGrowthAndDifferentialCRUD", SplitGrowthAndDifferentialCRUD},
      {"ConcurrentInsertAndSplits", ConcurrentInsertAndSplits},
      {"ReadersObserveMonotonicHotKeyUpdates", ReadersObserveMonotonicHotKeyUpdates},
      {"ConcurrentAdmissionsDeletesAndReinserts", ConcurrentAdmissionsDeletesAndReinserts},
      {"OptimisticReadersDuringCacheSplitsAndEvictions", OptimisticReadersDuringCacheSplitsAndEvictions},
      {"SamplingDisabledStillHasCorrectCRUD", SamplingDisabledStillHasCorrectCRUD},
  };
  try {
    Require(argc <= 2, "Usage: ehcache_test [exact_test_name]");
    size_t ran = 0;
    for (const auto& [name, test] : tests) {
      if (argc == 2 && std::string(argv[1]) != name) continue;
      std::cout << "[ RUN      ] " << name << std::endl;
      test();
      std::cout << "[       OK ] " << name << std::endl;
      ++ran;
    }
    Require(ran != 0, "Unknown test name");
    std::cout << "[  PASSED  ] " << ran << " tests" << std::endl;
  } catch (const std::exception& error) {
    std::cerr << "[  FAILED  ] " << error.what() << std::endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
