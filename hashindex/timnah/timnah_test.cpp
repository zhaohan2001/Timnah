#include "timnah.h"
#include <chrono>
#include <thread>

using TimnahNameSpace::Timnah;
namespace {
constexpr u32 kSlots = 14;

Timnah::Options SmallOptions(double sample_rate = 0.0) {
  Timnah::Options options;
  options.fast_depth = 1;
  options.slow_depth = 1;
  options.fast_node = -1;
  options.slow_node = -1;
  options.sample_rate = sample_rate;
  options.convergence_threshold = 0.0;
  options.decay_interval_ms = 1000000000;
  options.queue_capacity = 64;
  options.prefault = false;
  return options;
}

IndexInterface::ParamType Parameters(u64 background = 0) {
  IndexInterface::ParamType param{};
  param.num_threads = 30;
  param.num_bg = background;
  return param;
}

void Check(Timnah& table, u64 key, u64 expected) {
  u64 value = ~expected;
  ASSERT_EQ(table.Search(key, value), Result::SUCCESS) << "key=" << key;
  ASSERT_EQ(value, expected) << "key=" << key;
}

// The completed implementation uses high bits for the directory and low eight
// bits for its 256 buckets. This crafts collisions rather than hoping to see one.
std::vector<u64> Keys(u32 count, u32 bucket = 7, u8 depth = 1,
                      u32 prefix = 0, u64 start = 1) {
  std::vector<u64> keys;
  for (u64 key = start; keys.size() < count; ++key) {
    const auto hash = HashFunction(key);
    if ((hash & 255) == bucket && (hash >> (64 - depth)) == prefix)
      keys.push_back(key);
  }
  return keys;
}

void FillFast(Timnah& table, const std::vector<u64>& keys) {
  ASSERT_GE(keys.size(), kSlots);
  for (u32 i = 0; i < kSlots; ++i) {
    ASSERT_EQ(table.Insert(keys[i], keys[i] ^ 0xa55a), Result::SUCCESS);
    const auto found = table.Inspect(keys[i]);
    ASSERT_TRUE(found.found);
    ASSERT_EQ(found.tier, 0);
    ASSERT_EQ(found.copies, 1);
  }
}

void TierQueries(Timnah& table, u64 fast, u64 slow, u32 fast_count, u32 slow_count) {
  for (u32 i = 0; i < fast_count; ++i) Check(table, fast, fast ^ 0xa55a);
  for (u32 i = 0; i < slow_count; ++i) Check(table, slow, ~slow);
}

void ExpectConvergedDiagnosticsFrozen(const Timnah::Diagnostics& before,
                                     const Timnah::Diagnostics& after) {
  EXPECT_TRUE(after.converged);
  EXPECT_FALSE(after.tracking_active);
  EXPECT_EQ(after.fast_hits, before.fast_hits);
  EXPECT_EQ(after.slow_hits, before.slow_hits);
  EXPECT_EQ(after.misses, before.misses);
  EXPECT_EQ(after.samples, before.samples);
  EXPECT_EQ(after.reorders, before.reorders);
  EXPECT_EQ(after.promotions, before.promotions);
  EXPECT_EQ(after.demotions, before.demotions);
  EXPECT_EQ(after.epoch, before.epoch);
  EXPECT_EQ(after.decay_events, before.decay_events);
  EXPECT_EQ(after.queue_enqueued, before.queue_enqueued);
  EXPECT_EQ(after.queue_dropped, before.queue_dropped);
  EXPECT_EQ(after.queue_busy_dropped, before.queue_busy_dropped);
  EXPECT_EQ(after.queue_full_dropped, before.queue_full_dropped);
  EXPECT_EQ(after.queue_deduplicated, before.queue_deduplicated);
  EXPECT_EQ(after.queue_pending, 0);
  EXPECT_EQ(after.in_flight, 0);
}

TEST(Timnah, LifecycleAndAllKeyValueBitPatterns) {
  Timnah table(SmallOptions());
  u64 value = 31;
  EXPECT_EQ(table.Search(0, value), Result::FAILED);
  EXPECT_EQ(table.Insert(0, 7), Result::FAILED);
  ASSERT_EQ(table.Create(Parameters()), Result::SUCCESS);
  EXPECT_EQ(table.Create(Parameters()), Result::FAILED);
  ASSERT_EQ(table.Insert(0, 0), Result::SUCCESS);
  ASSERT_EQ(table.Insert(UINT64_MAX, UINT64_MAX), Result::SUCCESS);
  EXPECT_EQ(table.Insert(0, 9), Result::FAILED);
  Check(table, 0, 0);
  Check(table, UINT64_MAX, UINT64_MAX);
  ASSERT_EQ(table.Update(0, UINT64_MAX), Result::SUCCESS);
  ASSERT_EQ(table.Update(UINT64_MAX, 0), Result::SUCCESS);
  Check(table, 0, UINT64_MAX);
  Check(table, UINT64_MAX, 0);
  EXPECT_EQ(table.Update(123456789, 4), Result::FAILED);
  EXPECT_EQ(table.Remove(123456789), Result::FAILED);
  ASSERT_EQ(table.Remove(0), Result::SUCCESS);
  EXPECT_EQ(table.Remove(0), Result::FAILED);
  EXPECT_EQ(table.Search(0, value), Result::FAILED);
  ASSERT_EQ(table.Insert(0, 17), Result::SUCCESS);
  EXPECT_EQ(table.Snapshot().size, 2);
  EXPECT_TRUE(table.Validate());
  table.Stop();
  table.Stop();
  // Stop retires maintenance workers, not the user's records.
  Check(table, 0, 17);
  ASSERT_EQ(table.Update(0, 18), Result::SUCCESS);
  Check(table, 0, 18);
}

TEST(Timnah, DifferentialCrudAndSlowOnlyResizing) {
  Timnah table(SmallOptions());
  ASSERT_EQ(table.Create(Parameters()), Result::SUCCESS);
  const auto initial = table.Snapshot();
  std::unordered_map<u64, u64> reference;
  for (u64 key = 0; key < 24000; ++key) {
    ASSERT_EQ(table.Insert(key, key ^ 0x123456), Result::SUCCESS);
    reference[key] = key ^ 0x123456;
  }
  std::mt19937_64 random(1729);
  for (u32 i = 0; i < 70000; ++i) {
    const u64 key = random() % 36000, value = random();
    const auto found = reference.find(key);
    const bool exists = found != reference.end();
    switch (random() % 4) {
      case 0:
        ASSERT_EQ(table.Insert(key, value), exists ? Result::FAILED : Result::SUCCESS);
        if (!exists) reference[key] = value;
        break;
      case 1:
        ASSERT_EQ(table.Update(key, value), exists ? Result::SUCCESS : Result::FAILED);
        if (exists) reference[key] = value;
        break;
      case 2:
        ASSERT_EQ(table.Remove(key), exists ? Result::SUCCESS : Result::FAILED);
        reference.erase(key);
        break;
      default: {
        u64 output{};
        ASSERT_EQ(table.Search(key, output), exists ? Result::SUCCESS : Result::FAILED);
        if (exists) ASSERT_EQ(output, found->second);
      }
    }
  }
  for (u64 key = 0; key < 36000; ++key) {
    const auto found = reference.find(key);
    u64 output{};
    ASSERT_EQ(table.Search(key, output), found == reference.end() ? Result::FAILED : Result::SUCCESS);
    if (found != reference.end()) ASSERT_EQ(output, found->second);
  }
  const auto final = table.Snapshot();
  EXPECT_EQ(final.size, reference.size());
  EXPECT_EQ(final.fast_segments, initial.fast_segments);
  EXPECT_EQ(final.fast_bucket_bytes, initial.fast_bucket_bytes);
  EXPECT_EQ(final.fast_counter_bytes, initial.fast_counter_bytes);
  EXPECT_GT(final.slow_segments, initial.slow_segments);
  EXPECT_GT(final.slow_splits, 0);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, ThirtyWritersAndCrossTierDuplicateRace) {
  Timnah table(SmallOptions());
  ASSERT_EQ(table.Create(Parameters(2)), Result::SUCCESS);
  constexpr u32 threads = 30, per_thread = 500;
  std::barrier start(threads);
  std::atomic<u64> failures{}, winners{};
  std::vector<std::jthread> workers;
  for (u32 tid = 0; tid < threads; ++tid) workers.emplace_back([&, tid] {
    start.arrive_and_wait();
    if (table.Insert(UINT64_MAX, 42) == Result::SUCCESS) ++winners;
    for (u64 i = 0; i < per_thread; ++i) {
      const auto key = tid * per_thread + i;
      u64 value{};
      if (table.Insert(key, key + 1) != Result::SUCCESS ||
          table.Search(key, value) != Result::SUCCESS || value != key + 1 ||
          table.Update(key, key + 2) != Result::SUCCESS) ++failures;
    }
  });
  workers.clear();
  EXPECT_EQ(winners.load(), 1);
  EXPECT_EQ(failures.load(), 0);
  for (u64 key = 0; key < threads * per_thread; ++key) Check(table, key, key + 2);
  Check(table, UINT64_MAX, 42);
  EXPECT_EQ(table.Snapshot().size, threads * per_thread + 1);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, ReadersRemainCorrectDuringUpdatesAndSlowSplits) {
  Timnah table(SmallOptions());
  ASSERT_EQ(table.Create(Parameters(2)), Result::SUCCESS);
  constexpr u64 stable = 1200, appended = 24000;
  for (u64 key = 0; key < stable; ++key)
    ASSERT_EQ(table.Insert(key, key), Result::SUCCESS);
  std::atomic<bool> done{false};
  std::atomic<u64> failures{}, reads{};
  std::vector<std::jthread> readers, writers;
  for (u32 tid = 0; tid < 6; ++tid) readers.emplace_back([&, tid] {
    u64 i = tid;
    while (!done.load()) {
      const u64 key = i++ % stable;
      u64 value{};
      if (table.Search(key, value) != Result::SUCCESS || (value != key && value != ~key))
        ++failures;
      ++reads;
    }
  });
  for (u32 tid = 0; tid < 6; ++tid) writers.emplace_back([&, tid] {
    for (u64 i = tid; i < appended; i += 6) {
      if (table.Insert(stable + i, i) != Result::SUCCESS ||
          table.Update(i % stable, (i & 1) ? i % stable : ~(i % stable)) != Result::SUCCESS)
        ++failures;
    }
  });
  writers.clear();
  done.store(true);
  readers.clear();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_GT(reads.load(), 0);
  for (u64 i = 0; i < appended; ++i) Check(table, stable + i, i);
  EXPECT_GT(table.Snapshot().slow_splits, 0);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, IntraBucketStrictThresholdAndHottestToFront) {
  Timnah table(SmallOptions());
  ASSERT_EQ(table.Create(Parameters()), Result::SUCCESS);
  const auto keys = Keys(4);
  for (auto key : keys) ASSERT_EQ(table.Insert(key, key), Result::SUCCESS);
  ASSERT_TRUE(table.DebugSetCounter(keys[0], 100));
  ASSERT_TRUE(table.DebugSetCounter(keys[1], 10));
  ASSERT_TRUE(table.DebugSetCounter(keys[2], 20));
  ASSERT_TRUE(table.DebugSetCounter(keys[3], 99));
  table.DebugMaintain(keys[2]);
  EXPECT_EQ(table.Inspect(keys[2]).slot, 2);  // Equality is not greater than 2x.
  ASSERT_TRUE(table.DebugSetCounter(keys[2], 21));
  table.DebugMaintain(keys[2]);
  EXPECT_EQ(table.Inspect(keys[2]).slot, 1);
  ASSERT_TRUE(table.DebugSetCounter(keys[3], 101));
  table.DebugMaintain(keys[3]);
  EXPECT_EQ(table.Inspect(keys[3]).slot, 0);  // Hottest jumps directly to front.
  EXPECT_GE(table.Snapshot().reorders, 2);
  for (auto key : keys) Check(table, key, key);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, CrossTierPromotionUsesEitherThresholdCondition) {
  // Case 0 meets hottest comparison only: 101 > 100, but 101 <= 2 * 100.
  // Case 1 meets coldest comparison only: 21 < 100, but 21 > 2 * 10.
  for (u32 condition = 0; condition < 2; ++condition) {
    Timnah table(SmallOptions());
    ASSERT_EQ(table.Create(Parameters()), Result::SUCCESS);
    const auto keys = Keys(kSlots + 1);
    FillFast(table, keys);
    const auto slow = keys.back();
    ASSERT_EQ(table.Insert(slow, ~slow), Result::SUCCESS);
    ASSERT_EQ(table.Inspect(slow).tier, 1);
    for (u32 i = 0; i < kSlots; ++i)
      ASSERT_TRUE(table.DebugSetCounter(keys[i], condition && i ? 10 : 100));
    if (condition) {
      ASSERT_TRUE(table.DebugSetCounter(slow, 20));
      table.DebugMaintain(slow);
      EXPECT_EQ(table.Inspect(slow).tier, 1);  // Both strict conditions false.
    }
    ASSERT_TRUE(table.DebugSetCounter(slow, condition ? 21 : 101));
    table.DebugMaintain(slow);
    const auto promoted = table.Inspect(slow);
    ASSERT_TRUE(promoted.found);
    EXPECT_EQ(promoted.tier, 0);
    EXPECT_EQ(promoted.value, ~slow);
    EXPECT_EQ(promoted.copies, 1);
    EXPECT_GT(table.Snapshot().promotions, 0);
    for (u32 i = 0; i < kSlots; ++i) {
      Check(table, keys[i], keys[i] ^ 0xa55a);
      EXPECT_EQ(table.Inspect(keys[i]).copies, 1);
    }
    EXPECT_TRUE(table.Validate());
  }
}

TEST(Timnah, MigrationToDifferentSlowBucketCanSplitWithoutLosingVictim) {
  auto options = SmallOptions();
  options.slow_depth = 2;
  Timnah table(options);
  ASSERT_EQ(table.Create(Parameters()), Result::SUCCESS);
  const auto left = Keys(2 * kSlots, 19, 2, 0);
  const auto right = Keys(1, 19, 2, 1);
  FillFast(table, left);
  for (u32 i = kSlots; i < left.size(); ++i)
    ASSERT_EQ(table.Insert(left[i], left[i] ^ 0xa55a), Result::SUCCESS);
  ASSERT_EQ(table.Insert(right[0], ~right[0]), Result::SUCCESS);
  for (u32 i = 0; i < kSlots; ++i)
    ASSERT_TRUE(table.DebugSetCounter(left[i], i ? 100 : 1));
  ASSERT_TRUE(table.DebugSetCounter(right[0], 1000));
  const auto before = table.Snapshot();
  table.DebugMaintain(right[0]);
  EXPECT_EQ(table.Inspect(right[0]).tier, 0);
  EXPECT_EQ(table.Inspect(left[0]).tier, 1);
  EXPECT_GT(table.Snapshot().slow_splits, before.slow_splits);
  EXPECT_EQ(table.Snapshot().fast_segments, before.fast_segments);
  for (auto key : left) {
    Check(table, key, key ^ 0xa55a);
    EXPECT_EQ(table.Inspect(key).copies, 1);
  }
  Check(table, right[0], ~right[0]);
  EXPECT_EQ(table.Inspect(right[0]).copies, 1);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, PeriodicEpochDecayReducesExistingHistory) {
  Timnah table(SmallOptions());
  ASSERT_EQ(table.Create(Parameters()), Result::SUCCESS);
  const auto keys = Keys(2);
  for (auto key : keys) ASSERT_EQ(table.Insert(key, key), Result::SUCCESS);
  ASSERT_TRUE(table.DebugSetCounter(keys[0], 128));
  ASSERT_TRUE(table.DebugSetCounter(keys[1], 32));
  const auto epoch = table.Snapshot().epoch;
  table.DebugAdvanceDecay(2);
  EXPECT_EQ(table.Snapshot().epoch, epoch + 2);
  EXPECT_EQ(table.Inspect(keys[0]).counter, 32);
  EXPECT_EQ(table.Inspect(keys[1]).counter, 8);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, TrackingSurvivesRepeatedStatsAndAdaptsAfterHotsetChanges) {
  Timnah table(SmallOptions(1.0));
  ASSERT_EQ(table.Create(Parameters(1)), Result::SUCCESS);
  const auto keys = Keys(kSlots + 1);
  FillFast(table, keys);
  ASSERT_EQ(table.Insert(keys.back(), ~keys.back()), Result::SUCCESS);
  for (u32 i = 0; i < kSlots; ++i) ASSERT_TRUE(table.DebugSetCounter(keys[i], 512));
  ASSERT_TRUE(table.DebugSetCounter(keys.back(), 1));
  for (u32 i = 0; i < 40; ++i) (void)table.Stat();
  EXPECT_TRUE(table.Snapshot().tracking_active);
  const auto samples = table.Snapshot().samples;
  table.DebugAdvanceDecay(6);
  for (u32 i = 0; i < 300; ++i) Check(table, keys.back(), ~keys.back());
  ASSERT_TRUE(table.DrainMaintenance(5000));
  EXPECT_GT(table.Snapshot().samples, samples);
  EXPECT_TRUE(table.Snapshot().tracking_active);
  EXPECT_EQ(table.Inspect(keys.back()).tier, 0);
  EXPECT_GT(table.Snapshot().promotions, 0);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, NoBackgroundWorkerDoesNotMigrateRecords) {
  auto options = SmallOptions(1.0);
  options.queue_capacity = 4;
  Timnah table(options);
  ASSERT_EQ(table.Create(Parameters()), Result::SUCCESS);
  const auto keys = Keys(kSlots + 1);
  FillFast(table, keys);
  ASSERT_EQ(table.Insert(keys.back(), ~keys.back()), Result::SUCCESS);
  for (u32 i = 0; i < 100; ++i) Check(table, keys.back(), ~keys.back());
  EXPECT_EQ(table.Inspect(keys.back()).tier, 1);
  EXPECT_EQ(table.Snapshot().promotions, 0);
  EXPECT_LE(table.Snapshot().queue_pending, options.queue_capacity);
  table.Stop();
  Check(table, keys.back(), ~keys.back());
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, BoundedThreadQueuesAndTwoWorkersDrainSafely) {
  auto options = SmallOptions(1.0);
  options.queue_capacity = 1;
  Timnah table(options);
  ASSERT_EQ(table.Create(Parameters(2)), Result::SUCCESS);
  const auto keys = Keys(kSlots + 12);
  FillFast(table, keys);
  for (u32 i = kSlots; i < keys.size(); ++i)
    ASSERT_EQ(table.Insert(keys[i], ~keys[i]), Result::SUCCESS);
  constexpr u32 threads = 12;
  std::barrier start(threads);
  std::atomic<u64> failures{};
  std::vector<std::jthread> readers;
  for (u32 tid = 0; tid < threads; ++tid) readers.emplace_back([&, tid] {
    start.arrive_and_wait();
    for (u32 j = 0; j < 1200; ++j) {
      const auto key = keys[kSlots + (tid + j / 100) % threads];
      u64 value{};
      if (table.Search(key, value) != Result::SUCCESS || value != ~key) ++failures;
    }
  });
  readers.clear();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_LE(table.Snapshot().queue_pending, (threads + 1) * options.queue_capacity);
  ASSERT_TRUE(table.DrainMaintenance(5000));
  const auto stats = table.Snapshot();
  EXPECT_EQ(stats.queue_pending, 0);
  EXPECT_EQ(stats.in_flight, 0);
  EXPECT_GT(stats.queue_enqueued, 0);
  EXPECT_GT(stats.promotions, 0);
  for (u32 i = 0; i < keys.size(); ++i) {
    Check(table, keys[i], i < kSlots ? keys[i] ^ 0xa55a : ~keys[i]);
    EXPECT_EQ(table.Inspect(keys[i]).copies, 1);
  }
  EXPECT_TRUE(table.Validate());
  table.Stop();
  table.Stop();
}

TEST(Timnah, HotSlotAndRegularValueUpdatesRemainConsistent) {
  Timnah table(SmallOptions());
  ASSERT_EQ(table.Create(Parameters(1)), Result::SUCCESS);
  const auto keys = Keys(2);
  for (auto key : keys) ASSERT_EQ(table.Insert(key, key), Result::SUCCESS);
  ASSERT_EQ(table.Inspect(keys[0]).slot, 0);
  ASSERT_EQ(table.Inspect(keys[1]).slot, 1);
  std::atomic<bool> done{false};
  std::atomic<u64> failures{}, reads{};
  std::vector<std::jthread> readers, writers;
  for (u32 tid = 0; tid < 4; ++tid) readers.emplace_back([&, tid] {
    while (!done.load()) {
      const auto key = keys[tid % 2];
      u64 value{};
      if (table.Search(key, value) != Result::SUCCESS || (value != key && value != ~key))
        ++failures;
      ++reads;
    }
  });
  for (u32 tid = 0; tid < 2; ++tid) writers.emplace_back([&, tid] {
    const auto key = keys[tid];
    for (u32 i = 0; i < 12000; ++i)
      if (table.Update(key, (i & 1) ? ~key : key) != Result::SUCCESS) ++failures;
  });
  writers.clear();
  done.store(true);
  readers.clear();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_GT(reads.load(), 0);
  for (auto key : keys) Check(table, key, ~key);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, ReadersAndUpdatesRaceWithCrossTierMigration) {
  auto options = SmallOptions(1.0);
  options.queue_capacity = 4;
  Timnah table(options);
  ASSERT_EQ(table.Create(Parameters(2)), Result::SUCCESS);
  const auto keys = Keys(3 * kSlots);
  for (auto key : keys) ASSERT_EQ(table.Insert(key, key), Result::SUCCESS);
  std::atomic<u64> failures{};
  std::barrier start(8);
  std::vector<std::jthread> workers;
  for (u32 tid = 0; tid < 8; ++tid) workers.emplace_back([&, tid] {
    start.arrive_and_wait();
    for (u32 j = 0; j < 5000; ++j) {
      const auto key = keys[(j / 40 + tid * 7) % keys.size()];
      if (tid < 2 && table.Update(key, (j & 1) ? key : ~key) != Result::SUCCESS)
        ++failures;
      u64 value{};
      if (table.Search(key, value) != Result::SUCCESS || (value != key && value != ~key))
        ++failures;
    }
  });
  workers.clear();
  ASSERT_TRUE(table.DrainMaintenance(5000));
  EXPECT_EQ(failures.load(), 0);
  for (auto key : keys) {
    const auto found = table.Inspect(key);
    EXPECT_TRUE(found.found);
    EXPECT_EQ(found.copies, 1);
    EXPECT_TRUE(found.value == key || found.value == ~key);
  }
  EXPECT_GT(table.Snapshot().promotions, 0);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, IndependentInstancesAndRepeatedWorkerLifetime) {
  for (u32 iteration = 0; iteration < 4; ++iteration) {
    Timnah first(SmallOptions(1.0)), second(SmallOptions(1.0));
    ASSERT_EQ(first.Create(Parameters(2)), Result::SUCCESS);
    ASSERT_EQ(second.Create(Parameters(1)), Result::SUCCESS);
    for (u64 key = 0; key < 100; ++key) {
      ASSERT_EQ(first.Insert(key, key + iteration), Result::SUCCESS);
      ASSERT_EQ(second.Insert(key, ~(key + iteration)), Result::SUCCESS);
    }
    first.Stop();
    first.Stop();
    for (u64 key = 0; key < 100; ++key) {
      Check(first, key, key + iteration);
      Check(second, key, ~(key + iteration));
    }
    EXPECT_TRUE(second.Snapshot().tracking_active);
    EXPECT_TRUE(first.Validate());
    EXPECT_TRUE(second.Validate());
  }
}

TEST(Timnah, VariableLengthBinaryKeysOwnTheirBytesAndSupportCrud) {
  auto options = SmallOptions();
  options.variable_keys = true;
  Timnah table(options);
  ASSERT_EQ(table.Create(Parameters(1)), Result::SUCCESS);
  std::vector<std::string> keys{"", std::string(1, '\0'), std::string("a\0b", 3),
                                std::string("a\0c", 3), std::string(4096, 'x')};
  for (u64 i = 0; i < 5000; ++i) keys.push_back("shared-prefix/" + std::to_string(i));
  for (u64 i = 0; i < keys.size(); ++i) {
    std::string temporary = keys[i];
    ASSERT_EQ(table.InsertBytes(temporary, i), Result::SUCCESS);
    temporary.assign(temporary.size(), 'z');  // Caller storage may disappear/change.
  }
  EXPECT_EQ(table.InsertBytes(keys[2], 9), Result::FAILED);
  EXPECT_EQ(table.Snapshot().size, keys.size());
  EXPECT_GT(table.Snapshot().variable_key_bytes, 0);
  for (u64 i = 0; i < keys.size(); ++i) {
    u64 value{};
    ASSERT_EQ(table.SearchBytes(keys[i], value), Result::SUCCESS);
    ASSERT_EQ(value, i);
    ASSERT_EQ(table.UpdateBytes(keys[i], ~i), Result::SUCCESS);
  }
  for (u64 i = 0; i < keys.size(); i += 2) {
    ASSERT_EQ(table.RemoveBytes(keys[i]), Result::SUCCESS);
    EXPECT_EQ(table.RemoveBytes(keys[i]), Result::FAILED);
  }
  for (u64 i = 0; i < keys.size(); ++i) {
    u64 value{};
    EXPECT_EQ(table.SearchBytes(keys[i], value), i % 2 ? Result::SUCCESS : Result::FAILED);
    if (i % 2) EXPECT_EQ(value, ~i);
    else ASSERT_EQ(table.InsertBytes(keys[i], UINT64_MAX), Result::SUCCESS);
  }
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, ConcurrentVariableLengthKeyDuplicateRaceAndUpdates) {
  auto options = SmallOptions();
  options.variable_keys = true;
  Timnah table(options);
  ASSERT_EQ(table.Create(Parameters(2)), Result::SUCCESS);
  constexpr u32 threads = 8, each = 300;
  const std::string duplicate("shared\0duplicate", 16);
  std::barrier start(threads);
  std::atomic<u64> failures{}, winners{};
  std::vector<std::jthread> workers;
  for (u32 tid = 0; tid < threads; ++tid) workers.emplace_back([&, tid] {
    start.arrive_and_wait();
    if (table.InsertBytes(duplicate, 99) == Result::SUCCESS) ++winners;
    for (u32 j = 0; j < each; ++j) {
      const auto number = tid * each + j;
      const std::string key = "thread/" + std::to_string(tid) + "/" + std::to_string(j);
      u64 value{};
      if (table.InsertBytes(key, number) != Result::SUCCESS ||
          table.SearchBytes(key, value) != Result::SUCCESS || value != number ||
          table.UpdateBytes(key, ~u64{number}) != Result::SUCCESS) ++failures;
    }
  });
  workers.clear();
  EXPECT_EQ(winners.load(), 1);
  EXPECT_EQ(failures.load(), 0);
  for (u32 tid = 0; tid < threads; ++tid) for (u32 j = 0; j < each; ++j) {
    const auto number = tid * each + j;
    const std::string key = "thread/" + std::to_string(tid) + "/" + std::to_string(j);
    u64 value{};
    ASSERT_EQ(table.SearchBytes(key, value), Result::SUCCESS);
    ASSERT_EQ(value, ~u64{number});
  }
  EXPECT_EQ(table.Snapshot().size, threads * each + 1);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, ConcurrentSnapshotsPreserveExactSingleWriterHitCounters) {
  Timnah table(SmallOptions());
  ASSERT_EQ(table.Create(Parameters(2)), Result::SUCCESS);
  const auto keys = Keys(2 * kSlots + 1);
  FillFast(table, keys);
  for (u32 i = kSlots; i < 2 * kSlots; ++i)
    ASSERT_EQ(table.Insert(keys[i], ~keys[i]), Result::SUCCESS);
  const auto baseline = table.Snapshot();
  constexpr u32 threads = 30, each = 1200;
  std::barrier start(threads + 1);
  std::atomic<bool> done{false};
  std::atomic<u64> failures{}, snapshots{};
  std::vector<std::jthread> workers;
  std::jthread observer([&] {
    start.arrive_and_wait();
    auto previous = baseline;
    do {
      const auto current = table.Snapshot();
      // The observer only reads these single-producer atomic counters. Reading
      // diagnostics must neither reset counts nor lose foreground increments.
      if (current.fast_hits < previous.fast_hits ||
          current.slow_hits < previous.slow_hits || current.misses < previous.misses)
        ++failures;
      previous = current;
      ++snapshots;
      std::this_thread::yield();
    } while (!done.load(std::memory_order_acquire));
  });
  for (u32 tid = 0; tid < threads; ++tid) workers.emplace_back([&, tid] {
    start.arrive_and_wait();
    const auto fast = keys[tid % kSlots];
    const auto slow = keys[kSlots + tid % kSlots];
    for (u32 i = 0; i < each; ++i) {
      u64 value{};
      if (table.Search(fast, value) != Result::SUCCESS || value != (fast ^ 0xa55a))
        ++failures;
      if (table.Search(slow, value) != Result::SUCCESS || value != ~slow)
        ++failures;
      if (table.Search(keys.back(), value) != Result::FAILED) ++failures;
    }
  });
  workers.clear();
  done.store(true, std::memory_order_release);
  observer.join();
  const auto final = table.Snapshot();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_GT(snapshots.load(), 0);
  EXPECT_EQ(final.fast_hits - baseline.fast_hits, u64{threads} * each);
  EXPECT_EQ(final.slow_hits - baseline.slow_hits, u64{threads} * each);
  EXPECT_EQ(final.misses - baseline.misses, u64{threads} * each);
  EXPECT_EQ(final.samples - baseline.samples, 0);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, RealTimeDecayProgressesWithAndWithoutBackgroundWorker) {
  for (const u64 background : {u64{0}, u64{1}}) {
    auto options = SmallOptions(1.0);
    options.decay_interval_ms = 10;
    Timnah table(options);
    ASSERT_EQ(table.Create(Parameters(background)), Result::SUCCESS);
    const auto target = Keys(1, 7).front();
    const auto probe = Keys(1, 8).front();
    ASSERT_EQ(table.Insert(target, ~target), Result::SUCCESS);
    ASSERT_EQ(table.Insert(probe, ~probe), Result::SUCCESS);
    constexpr u32 history = u32{1} << 20;
    ASSERT_TRUE(table.DebugSetCounter(target, history));
    const auto before = table.Snapshot();
    auto current = before;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    // BG=0 must update the real epoch from sampled foreground observations.
    // BG=1 deliberately performs no queries: an idle worker must still advance
    // real epochs. Snapshot itself must remain a read-only observation.
    while (current.epoch < before.epoch + 2 && std::chrono::steady_clock::now() < deadline) {
      if (!background) Check(table, probe, ~probe);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      current = table.Snapshot();
    }
    ASSERT_GE(current.epoch, before.epoch + 2) << "background=" << background;
    EXPECT_GT(current.decay_events, before.decay_events);
    if (!background) EXPECT_GT(current.samples, before.samples);
    else EXPECT_EQ(current.samples, before.samples);
    // No reads touched target since its history was set. Lazy physical decay
    // and eager physical decay must give the same effective counter bound.
    EXPECT_LE(table.Inspect(target).counter, history / 4);
    EXPECT_TRUE(current.tracking_active);
    table.Stop();
    Check(table, target, ~target);
    EXPECT_TRUE(table.Validate());
  }
}

TEST(Timnah, NonPowerOfTwoQueuesRegisterNewThreadsAndDrainAcrossWaves) {
  auto options = SmallOptions(1.0);
  options.queue_capacity = 3;
  Timnah table(options);
  ASSERT_EQ(table.Create(Parameters(2)), Result::SUCCESS);
  constexpr u32 waves = 4, threads = 6;
  const auto keys = Keys(kSlots + waves * threads + 1);
  FillFast(table, keys);
  for (u32 i = kSlots; i < keys.size(); ++i)
    ASSERT_EQ(table.Insert(keys[i], ~keys[i]), Result::SUCCESS);
  std::atomic<u64> failures{};
  for (u32 wave = 0; wave < waves; ++wave) {
    const auto before = table.Snapshot();
    for (u32 tid = 0; tid < threads; ++tid) {
      const auto key = keys[kSlots + wave * threads + tid];
      ASSERT_EQ(table.Inspect(key).tier, 1);
      ASSERT_TRUE(table.DebugSetCounter(key, 1000000 * (wave + 1)));
    }
    std::barrier start(threads);
    std::vector<std::jthread> readers;
    for (u32 tid = 0; tid < threads; ++tid) readers.emplace_back([&, tid, wave] {
      start.arrive_and_wait();
      const auto key = keys[kSlots + wave * threads + tid];
      for (u32 i = 0; i < 300; ++i) {
        u64 value{};
        if (table.Search(key, value) != Result::SUCCESS || value != ~key) ++failures;
      }
    });
    readers.clear();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_LE(table.Snapshot().queue_pending,
              u64{wave + 1} * threads * options.queue_capacity);
    ASSERT_TRUE(table.DrainMaintenance(5000)) << "wave=" << wave;
    const auto after = table.Snapshot();
    EXPECT_GT(after.queue_enqueued, before.queue_enqueued) << "wave=" << wave;
    EXPECT_EQ(after.queue_pending, 0);
    EXPECT_EQ(after.in_flight, 0);
  }
  // Register the main thread's producer queue only after all previous waves.
  // Then stop with foreground callers quiescent: cached worker registries must
  // include this newly registered queue before their shutdown decisions.
  const auto last = keys.back();
  ASSERT_TRUE(table.DebugSetCounter(last, 10000000));
  const auto enqueued = table.Snapshot().queue_enqueued;
  for (u32 attempt = 0; attempt < 100 && table.Snapshot().queue_enqueued == enqueued; ++attempt)
    Check(table, last, ~last);
  ASSERT_GT(table.Snapshot().queue_enqueued, enqueued);
  table.Stop();
  table.Stop();
  EXPECT_EQ(table.Snapshot().queue_pending, 0);
  EXPECT_EQ(table.Snapshot().in_flight, 0);
  for (u32 i = 0; i < keys.size(); ++i) {
    Check(table, keys[i], i < kSlots ? keys[i] ^ 0xa55a : ~keys[i]);
    EXPECT_EQ(table.Inspect(keys[i]).copies, 1);
  }
  EXPECT_TRUE(table.Validate());
}
TEST(Timnah, PageAdvicePreservesCapacityRoutingAndCrud) {
  for (const bool huge_pages : {false, true}) {
    auto options = SmallOptions();
    options.fast_depth = 2;
    options.huge_pages = huge_pages;
    Timnah table(options);
    ASSERT_EQ(table.Create(Parameters(1)), Result::SUCCESS);
    const auto initial = table.Snapshot();
    EXPECT_EQ(initial.fast_bucket_bytes, 4 * 256 * 256);
    EXPECT_EQ(initial.fast_counter_bytes, 4 * 256 * 64);
    constexpr u64 count = 30000;
    for (u64 key = 0; key < count; ++key)
      ASSERT_EQ(table.Insert(key, ~key), Result::SUCCESS);
    for (u64 key = 0; key < count; key += 3)
      ASSERT_EQ(table.Update(key, key ^ 0xa55a), Result::SUCCESS);
    for (u64 key = 0; key < count; key += 5)
      ASSERT_EQ(table.Remove(key), Result::SUCCESS);
    for (u64 key = 0; key < count; ++key) {
      u64 value{};
      ASSERT_EQ(table.Search(key, value), key % 5 ? Result::SUCCESS : Result::FAILED);
      if (key % 5) EXPECT_EQ(value, key % 3 ? ~key : key ^ 0xa55a);
    }
    const auto final = table.Snapshot();
    EXPECT_EQ(final.size, count * 4 / 5);
    EXPECT_EQ(final.fast_bucket_bytes, initial.fast_bucket_bytes);
    EXPECT_EQ(final.fast_counter_bytes, initial.fast_counter_bytes);
    EXPECT_GT(final.slow_splits, 0);
    EXPECT_TRUE(table.Validate());
  }
}

TEST(Timnah, ConvergenceUsesAdjacentRoundDeltasAndSnapshotIsReadOnly) {
  auto options = SmallOptions();
  options.convergence_threshold = 0.05;
  Timnah table(options);
  ASSERT_EQ(table.Create(Parameters()), Result::SUCCESS);
  const auto keys = Keys(kSlots + 1);
  FillFast(table, keys);
  ASSERT_EQ(table.Insert(keys.back(), ~keys.back()), Result::SUCCESS);
  // Loading records and repeatedly observing diagnostics are not query rounds.
  table.Stat();
  EXPECT_EQ(table.Snapshot().observed_rounds, 0);
  EXPECT_TRUE(table.Snapshot().tracking_active);

  TierQueries(table, keys.front(), keys.back(), 750, 250);
  EXPECT_EQ(table.Snapshot().observed_rounds, 0);
  table.Stat();
  const auto first = table.Snapshot();
  EXPECT_EQ(first.observed_rounds, 1);
  EXPECT_DOUBLE_EQ(first.last_round_fast_rate, 0.75);
  EXPECT_DOUBLE_EQ(first.last_round_slow_rate, 0.25);
  EXPECT_DOUBLE_EQ(first.convergence_diff, -1.0);
  EXPECT_FALSE(first.converged);

  // A cumulative implementation would see an L1 difference of only about
  // 0.004 here and stop. The actual new round is 25% fast / 75% slow, diff=1.
  TierQueries(table, keys.front(), keys.back(), 1, 3);
  for (u32 i = 0; i < 5; ++i) {
    const auto snapshot = table.Snapshot();
    EXPECT_EQ(snapshot.observed_rounds, first.observed_rounds);
    EXPECT_DOUBLE_EQ(snapshot.last_round_fast_rate, first.last_round_fast_rate);
    EXPECT_DOUBLE_EQ(snapshot.last_round_slow_rate, first.last_round_slow_rate);
    EXPECT_FALSE(snapshot.converged);
  }
  table.Stat();
  const auto second = table.Snapshot();
  EXPECT_EQ(second.observed_rounds, 2);
  EXPECT_DOUBLE_EQ(second.last_round_fast_rate, 0.25);
  EXPECT_DOUBLE_EQ(second.last_round_slow_rate, 0.75);
  EXPECT_DOUBLE_EQ(second.convergence_diff, 1.0);
  EXPECT_FALSE(second.converged);
  EXPECT_TRUE(second.tracking_active);

  TierQueries(table, keys.front(), keys.back(), 1, 3);
  table.Stat();
  const auto third = table.Snapshot();
  EXPECT_EQ(third.observed_rounds, 3);
  EXPECT_DOUBLE_EQ(third.convergence_diff, 0.0);
  EXPECT_TRUE(third.converged);
  EXPECT_FALSE(third.tracking_active);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, EmptyAndMissOnlyIntervalsRebuildConvergenceBaseline) {
  auto options = SmallOptions();
  options.convergence_threshold = 0.05;
  Timnah table(options);
  ASSERT_EQ(table.Create(Parameters()), Result::SUCCESS);
  const auto keys = Keys(kSlots + 2);
  FillFast(table, keys);
  ASSERT_EQ(table.Insert(keys[kSlots], ~keys[kSlots]), Result::SUCCESS);
  u64 value{};
  table.Stat();
  ASSERT_EQ(table.Search(keys.back(), value), Result::FAILED);
  table.Stat();
  EXPECT_EQ(table.Snapshot().observed_rounds, 0);
  EXPECT_FALSE(table.Snapshot().converged);

  TierQueries(table, keys.front(), keys[kSlots], 1, 3);
  for (u32 i = 0; i < 12; ++i)
    ASSERT_EQ(table.Search(keys.back(), value), Result::FAILED);
  table.Stat();
  auto state = table.Snapshot();
  EXPECT_EQ(state.observed_rounds, 1);
  // Negative lookups do not enter either tier's successful-hit denominator.
  EXPECT_DOUBLE_EQ(state.last_round_fast_rate, 0.25);
  EXPECT_DOUBLE_EQ(state.last_round_slow_rate, 0.75);
  EXPECT_DOUBLE_EQ(state.convergence_diff, -1.0);

  table.Stat();  // An empty interval breaks adjacency.
  EXPECT_EQ(table.Snapshot().observed_rounds, 1);
  TierQueries(table, keys.front(), keys[kSlots], 1, 3);
  table.Stat();
  state = table.Snapshot();
  EXPECT_EQ(state.observed_rounds, 2);
  EXPECT_DOUBLE_EQ(state.convergence_diff, -1.0);
  EXPECT_FALSE(state.converged);

  ASSERT_EQ(table.Search(keys.back(), value), Result::FAILED);
  table.Stat();  // A miss-only interval also breaks adjacency.
  EXPECT_EQ(table.Snapshot().observed_rounds, 2);
  TierQueries(table, keys.front(), keys[kSlots], 1, 3);
  table.Stat();
  state = table.Snapshot();
  EXPECT_EQ(state.observed_rounds, 3);
  EXPECT_DOUBLE_EQ(state.convergence_diff, -1.0);
  EXPECT_FALSE(state.converged);
  EXPECT_TRUE(state.tracking_active);

  TierQueries(table, keys.front(), keys[kSlots], 1, 3);
  table.Stat();
  state = table.Snapshot();
  EXPECT_EQ(state.observed_rounds, 4);
  EXPECT_DOUBLE_EQ(state.convergence_diff, 0.0);
  EXPECT_TRUE(state.converged);
  EXPECT_TRUE(table.Validate());
}

TEST(Timnah, ConvergenceThresholdComparisonIsStrictAtExactBoundary) {
  auto options = SmallOptions();
  options.convergence_threshold = 0.5;
  Timnah table(options);
  ASSERT_EQ(table.Create(Parameters()), Result::SUCCESS);
  const auto keys = Keys(kSlots + 1);
  FillFast(table, keys);
  ASSERT_EQ(table.Insert(keys.back(), ~keys.back()), Result::SUCCESS);
  TierQueries(table, keys.front(), keys.back(), 1, 3);
  table.Stat();
  EXPECT_DOUBLE_EQ(table.Snapshot().convergence_diff, -1.0);

  TierQueries(table, keys.front(), keys.back(), 2, 2);
  table.Stat();
  auto state = table.Snapshot();
  EXPECT_DOUBLE_EQ(state.convergence_diff, 0.5);
  EXPECT_FALSE(state.converged);
  EXPECT_TRUE(state.tracking_active);

  TierQueries(table, keys.front(), keys.back(), 3, 1);
  table.Stat();
  state = table.Snapshot();
  EXPECT_DOUBLE_EQ(state.convergence_diff, 0.5);
  EXPECT_FALSE(state.converged);
  EXPECT_TRUE(state.tracking_active);

  TierQueries(table, keys.front(), keys.back(), 3, 1);
  table.Stat();
  state = table.Snapshot();
  EXPECT_DOUBLE_EQ(state.convergence_diff, 0.0);
  EXPECT_TRUE(state.converged);
}

TEST(Timnah, ConvergenceWithoutBackgroundFreezesTrackingButPreservesCrud) {
  auto options = SmallOptions(1.0);
  options.convergence_threshold = 0.05;
  options.decay_interval_ms = 10;
  Timnah table(options);
  ASSERT_EQ(table.Create(Parameters()), Result::SUCCESS);
  const auto keys = Keys(kSlots + 2);
  FillFast(table, keys);
  ASSERT_EQ(table.Insert(keys[kSlots], ~keys[kSlots]), Result::SUCCESS);
  for (u32 round = 0; round < 2; ++round) {
    Check(table, keys.front(), keys.front() ^ 0xa55a);
    table.Stat();
  }
  const auto frozen = table.Snapshot();
  ASSERT_TRUE(frozen.converged);
  ASSERT_GT(frozen.samples, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  for (u32 i = 0; i < 200; ++i) {
    TierQueries(table, keys.front(), keys[kSlots], 1, 1);
    u64 value{};
    ASSERT_EQ(table.Search(keys.back(), value), Result::FAILED);
  }
  ASSERT_EQ(table.Update(keys.front(), 91), Result::SUCCESS);
  Check(table, keys.front(), 91);
  ASSERT_EQ(table.Remove(keys[kSlots]), Result::SUCCESS);
  ASSERT_EQ(table.Insert(keys[kSlots], 92), Result::SUCCESS);
  Check(table, keys[kSlots], 92);
  ASSERT_EQ(table.Insert(keys.back(), 93), Result::SUCCESS);
  Check(table, keys.back(), 93);
  EXPECT_EQ(table.Snapshot().size, kSlots + 2);
  ExpectConvergedDiagnosticsFrozen(frozen, table.Snapshot());
  EXPECT_TRUE(table.Validate());
  table.Stop();
  table.Stop();
}

TEST(Timnah, ConvergenceStopsRegisteredQueuesBeforeReturningAndFreezesWorkers) {
  auto options = SmallOptions(1.0);
  options.convergence_threshold = 2.0;
  options.decay_interval_ms = 10;
  options.queue_capacity = 3;
  Timnah table(options);
  ASSERT_EQ(table.Create(Parameters(2)), Result::SUCCESS);
  constexpr u32 threads = 8, each = 256;
  const auto keys = Keys(kSlots + threads + 1);
  FillFast(table, keys);
  for (u32 i = kSlots; i < keys.size(); ++i) {
    ASSERT_EQ(table.Insert(keys[i], ~keys[i]), Result::SUCCESS);
    ASSERT_TRUE(table.DebugSetCounter(keys[i], 1000000 + i));
  }
  // Each key is queried once before the worker can change the classification
  // of a future read. The first real interval is exactly 50% fast / 50% slow.
  TierQueries(table, keys.front(), keys[kSlots], 1, 1);
  table.Stat();
  const auto first = table.Snapshot();
  ASSERT_EQ(first.observed_rounds, 1);
  ASSERT_DOUBLE_EQ(first.last_round_fast_rate, 0.5);
  ASSERT_DOUBLE_EQ(first.last_round_slow_rate, 0.5);
  ASSERT_FALSE(first.converged);

  std::atomic<u64> failures{};
  std::barrier start(threads);
  std::vector<std::jthread> readers;
  for (u32 tid = 0; tid < threads; ++tid) readers.emplace_back([&, tid] {
    start.arrive_and_wait();
    for (u32 i = 0; i < each; ++i) {
      const auto key = keys[kSlots + 1 + tid];
      u64 value{};
      if (table.Search(key, value) != Result::SUCCESS || value != ~key) ++failures;
    }
  });
  readers.clear();
  ASSERT_EQ(failures.load(), 0);
  ASSERT_GT(table.Snapshot().queue_enqueued, 0);
  // Foreground callers are quiescent. Whatever distribution migration reached,
  // its L1 distance from (0.5,0.5) is <=1, strictly below threshold 2.
  table.Stat();
  const auto frozen = table.Snapshot();
  ASSERT_TRUE(frozen.converged);
  EXPECT_EQ(frozen.observed_rounds, 2);
  EXPECT_EQ(frozen.queue_pending, 0);
  EXPECT_EQ(frozen.in_flight, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  for (u32 repetition = 0; repetition < 20; ++repetition)
    for (u32 i = 0; i < keys.size(); ++i)
      Check(table, keys[i], i < kSlots ? keys[i] ^ 0xa55a : ~keys[i]);
  u64 value{};
  ASSERT_EQ(table.Search(UINT64_MAX, value), Result::FAILED);
  ASSERT_EQ(table.Update(keys.back(), 97), Result::SUCCESS);
  Check(table, keys.back(), 97);
  ASSERT_EQ(table.Remove(keys.back()), Result::SUCCESS);
  ASSERT_EQ(table.Insert(keys.back(), 98), Result::SUCCESS);
  Check(table, keys.back(), 98);
  ExpectConvergedDiagnosticsFrozen(frozen, table.Snapshot());
  EXPECT_TRUE(table.Validate());
  table.Stop();
  EXPECT_EQ(table.Snapshot().queue_pending, 0);
  EXPECT_EQ(table.Snapshot().in_flight, 0);
}

TEST(Timnah, MultiKeyBatchesDrainStopAndFreezeWithoutLeakingInFlightWork) {
  constexpr u32 producers = 8, keys_per_producer = 64;
  const auto keys = Keys(kSlots + producers * keys_per_producer);
  // Exercise three termination paths with independent workers and queues.
  // Capacity 17 allows a 16-item batch plus a partial tail. Each producer
  // queries different candidates, rather than deduplicating one repeated key.
  for (u32 finish = 0; finish < 3; ++finish) {
    SCOPED_TRACE(finish);  // 0: Drain then Stop; 1: Stop; 2: convergence Freeze.
    auto options = SmallOptions(1.0);
    options.queue_capacity = 17;
    options.convergence_threshold = finish == 2 ? 2.0 : 0.0;
    Timnah table(options);
    ASSERT_EQ(table.Create(Parameters(2)), Result::SUCCESS);
    FillFast(table, keys);
    for (u32 i = kSlots; i < keys.size(); ++i) {
      ASSERT_EQ(table.Insert(keys[i], ~keys[i]), Result::SUCCESS);
      ASSERT_EQ(table.Inspect(keys[i]).tier, 1);
      ASSERT_TRUE(table.DebugSetCounter(keys[i], 1000000 + i));
    }
    if (finish == 2) {
      // This first interval is exactly (0.5,0.5), irrespective of a promotion
      // after the second Search. Any subsequent nonempty interval has L1<=1,
      // strictly below 2, so freezing does not depend on worker scheduling.
      TierQueries(table, keys.front(), keys[kSlots], 1, 1);
      table.Stat();
      const auto first = table.Snapshot();
      ASSERT_EQ(first.observed_rounds, 1);
      ASSERT_DOUBLE_EQ(first.last_round_fast_rate, 0.5);
      ASSERT_FALSE(first.converged);
    }
    std::atomic<u64> failures{};
    std::barrier start(producers);
    std::vector<std::jthread> readers;
    for (u32 tid = 0; tid < producers; ++tid) readers.emplace_back([&, tid] {
      start.arrive_and_wait();
      for (u32 i = 0; i < keys_per_producer * 8; ++i) {
        const auto key = keys[kSlots + tid * keys_per_producer + i % keys_per_producer];
        u64 value{};
        if (table.Search(key, value) != Result::SUCCESS || value != ~key) ++failures;
      }
    });
    readers.clear();
    ASSERT_EQ(failures.load(), 0);
    ASSERT_GT(table.Snapshot().queue_enqueued, 0);
    // Foreground producers have joined before any lifecycle boundary.
    if (finish == 0) {
      ASSERT_TRUE(table.DrainMaintenance(5000));
      EXPECT_EQ(table.Snapshot().queue_pending, 0);
      EXPECT_EQ(table.Snapshot().in_flight, 0);
    }
    if (finish == 2) {
      table.Stat();
      ASSERT_TRUE(table.Snapshot().converged);
      ASSERT_FALSE(table.Snapshot().tracking_active);
      EXPECT_EQ(table.Snapshot().observed_rounds, 2);
    } else {
      // Stop is responsible for draining its own queued and popped batches.
      table.Stop();
    }
    const auto finished = table.Snapshot();
    EXPECT_EQ(finished.queue_pending, 0);
    EXPECT_EQ(finished.in_flight, 0);
    for (u32 i = 0; i < keys.size(); ++i)
      Check(table, keys[i], i < kSlots ? keys[i] ^ 0xa55a : ~keys[i]);
    EXPECT_TRUE(table.Validate());
    if (finish == 2) ExpectConvergedDiagnosticsFrozen(finished, table.Snapshot());
    table.Stop();
    table.Stop();
    EXPECT_EQ(table.Snapshot().queue_pending, 0);
    EXPECT_EQ(table.Snapshot().in_flight, 0);
  }
}

TEST(Timnah, ConvergenceIsPerInstanceAndZeroThresholdKeepsTrackingActive) {
  auto options = SmallOptions(1.0);
  options.convergence_threshold = 0.05;
  Timnah first(options), second(options), disabled(SmallOptions(1.0));
  ASSERT_EQ(first.Create(Parameters()), Result::SUCCESS);
  ASSERT_EQ(second.Create(Parameters(1)), Result::SUCCESS);
  ASSERT_EQ(disabled.Create(Parameters(1)), Result::SUCCESS);
  constexpr u64 key = 1234567;
  ASSERT_EQ(first.Insert(key, 11), Result::SUCCESS);
  ASSERT_EQ(second.Insert(key, 22), Result::SUCCESS);
  ASSERT_EQ(disabled.Insert(key, 33), Result::SUCCESS);
  for (u32 round = 0; round < 2; ++round) {
    Check(first, key, 11);
    first.Stat();
  }
  const auto frozen = first.Snapshot();
  ASSERT_TRUE(frozen.converged);
  EXPECT_FALSE(second.Snapshot().converged);
  EXPECT_TRUE(second.Snapshot().tracking_active);
  EXPECT_EQ(second.Snapshot().observed_rounds, 0);
  EXPECT_EQ(second.Snapshot().samples, 0);

  Check(second, key, 22);
  second.Stat();
  EXPECT_EQ(second.Snapshot().observed_rounds, 1);
  EXPECT_FALSE(second.Snapshot().converged);
  EXPECT_TRUE(second.Snapshot().tracking_active);
  for (u32 round = 0; round < 3; ++round) {
    Check(disabled, key, 33);
    disabled.Stat();
    EXPECT_FALSE(disabled.Snapshot().converged);
    EXPECT_TRUE(disabled.Snapshot().tracking_active);
  }
  EXPECT_EQ(disabled.Snapshot().samples, 3);
  Check(second, key, 22);
  second.Stat();
  EXPECT_TRUE(second.Snapshot().converged);
  Check(first, key, 11);
  ExpectConvergedDiagnosticsFrozen(frozen, first.Snapshot());
  EXPECT_TRUE(first.Validate());
  EXPECT_TRUE(second.Validate());
  EXPECT_TRUE(disabled.Validate());
}
}  // namespace
