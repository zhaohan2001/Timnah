#include "dash.h"

using DashNameSpace::Dash;
namespace {
Dash::Options TestOptions() { return {1, -1, -1, 64 * 1024, 128 * 1024, 128 * 1024}; }

void Check(Dash& table, u64 key, u64 expected) {
  u64 value = ~expected;
  ASSERT_EQ(table.Search(key, value), Result::SUCCESS) << "key=" << key;
  ASSERT_EQ(value, expected) << "key=" << key;
}

u64 Counter(Dash& table, const std::string& name) {
  const auto stats = table.Stat();
  const auto pos = stats.find(name + "=");
  if (pos == std::string::npos) {
    ADD_FAILURE() << "Missing counter " << name << " in " << stats;
    return 0;
  }
  return std::stoull(stats.substr(pos + name.size() + 1));
}

// Choose deterministic collision sets using the implemented home mapping instead of relying on random tests to reach the path.
// All records map to the left initial directory segment; fingerprints may be zero, and there is no sentinel-key restriction.
std::vector<u64> KeysForBucket(u32 bucket, size_t count, u64 start = 1) {
  std::vector<u64> result;
  for (u64 key = start; result.size() < count; ++key) {
    const auto h = HashFunction(key);
    if ((h >> 63) == 0 && ((h >> 8) & 63) == bucket) result.push_back(key);
  }
  return result;
}

TEST(Dash, LifecycleZeroAndMaxKeysValues) {
  Dash table(TestOptions());
  u64 value = 91;
  EXPECT_EQ(table.Search(0, value), Result::FAILED);
  EXPECT_EQ(table.Insert(0, 0), Result::FAILED);
  ASSERT_EQ(table.Create({}), Result::SUCCESS);
  EXPECT_EQ(table.Create({}), Result::FAILED);
  ASSERT_EQ(table.Insert(0, 0), Result::SUCCESS);
  ASSERT_EQ(table.Insert(UINT64_MAX, UINT64_MAX), Result::SUCCESS);
  EXPECT_EQ(table.Insert(0, 17), Result::FAILED);
  Check(table, 0, 0);
  Check(table, UINT64_MAX, UINT64_MAX);
  ASSERT_EQ(table.Update(0, UINT64_MAX), Result::SUCCESS);
  ASSERT_EQ(table.Update(UINT64_MAX, 0), Result::SUCCESS);
  Check(table, 0, UINT64_MAX);
  Check(table, UINT64_MAX, 0);
  EXPECT_EQ(table.Update(123456, 99), Result::FAILED);
  EXPECT_EQ(table.Remove(123456), Result::FAILED);
  ASSERT_EQ(table.Remove(0), Result::SUCCESS);
  EXPECT_EQ(table.Remove(0), Result::FAILED);
  EXPECT_EQ(table.Search(0, value), Result::FAILED);
  ASSERT_EQ(table.Insert(0, 7), Result::SUCCESS);
  Check(table, 0, 7);
  EXPECT_EQ(Counter(table, "size"), 2);
  table.Stop();
  table.Stop();
}

TEST(Dash, RejectInvalidConfiguration) {
  Dash zero({0, -1, -1}), deep({25, -1, -1}), bad_node({1, -2, -1});
  EXPECT_EQ(zero.Create({}), Result::FAILED);
  EXPECT_EQ(deep.Create({}), Result::FAILED);
  EXPECT_EQ(bad_node.Create({}), Result::FAILED);
}

TEST(Dash, ForwardAndBackwardDisplacementWithWraparound) {
  // Forward: bucket 63/0 are full; bucket 0 contains its own-home keys and
  // bucket 1 has space. Move one 0 -> 1, then insert into the newly free slot.
  {
    Dash table(TestOptions());
    ASSERT_EQ(table.Create({}), Result::SUCCESS);
    const auto next_keys = KeysForBucket(0, 8);
    const auto keys = KeysForBucket(63, 25);
    for (auto key : next_keys) ASSERT_EQ(table.Insert(key, ~key), Result::SUCCESS);
    for (auto key : keys) ASSERT_EQ(table.Insert(key, ~key), Result::SUCCESS);
    EXPECT_GT(Counter(table, "displacements"), 0);
    EXPECT_EQ(Counter(table, "stash_inserts"), 0);
    for (auto key : next_keys) Check(table, key, ~key);
    for (auto key : keys) Check(table, key, ~key);
  }
  // Backward: 0/1/2 are full and 0 contains alternate-home records of 63.
  // Forward movement is blocked by full bucket 2, so move one 0 -> 63.
  {
    Dash table(TestOptions());
    ASSERT_EQ(table.Create({}), Result::SUCCESS);
    const auto previous_keys = KeysForBucket(63, 8);
    const auto next_keys = KeysForBucket(1, 28);
    const auto keys = KeysForBucket(0, 11);
    for (auto key : previous_keys) ASSERT_EQ(table.Insert(key, key + 3), Result::SUCCESS);
    for (auto key : next_keys) ASSERT_EQ(table.Insert(key, key + 3), Result::SUCCESS);
    for (auto key : keys) ASSERT_EQ(table.Insert(key, key + 3), Result::SUCCESS);
    EXPECT_GT(Counter(table, "displacements"), 0);
    EXPECT_EQ(Counter(table, "stash_inserts"), 0);
    for (auto key : previous_keys) Check(table, key, key + 3);
    for (auto key : next_keys) Check(table, key, key + 3);
    for (auto key : keys) Check(table, key, key + 3);
  }
}

TEST(Dash, StashOverflowMetadataDeleteReuseAndSplit) {
  Dash table(TestOptions());
  ASSERT_EQ(table.Create({}), Result::SUCCESS);
  const auto keys = KeysForBucket(7, 100);
  // 40 records exceed the 28 home/probe slots, require stash (including more
  // than four overflow fingerprints), but fit within two 14-slot stash buckets.
  for (u32 i = 0; i < 40; ++i) ASSERT_EQ(table.Insert(keys[i], i), Result::SUCCESS);
  EXPECT_GT(Counter(table, "stash_inserts"), 4);
  EXPECT_EQ(Counter(table, "splits"), 0);
  for (u32 i = 0; i < 40; ++i) Check(table, keys[i], i);
  u64 value{};
  EXPECT_EQ(table.Search(keys[99], value), Result::FAILED);
  // Exercise deletion of both normal and stash records and metadata reuse.
  for (u32 i = 1; i < 40; i += 2) ASSERT_EQ(table.Remove(keys[i]), Result::SUCCESS);
  for (u32 i = 1; i < 40; i += 2) {
    EXPECT_EQ(table.Search(keys[i], value), Result::FAILED);
    ASSERT_EQ(table.Insert(keys[i], i + 1000), Result::SUCCESS);
  }
  // More than 56 records in this home/probe/stash collision group must split.
  for (u32 i = 40; i < 90; ++i) ASSERT_EQ(table.Insert(keys[i], i), Result::SUCCESS);
  EXPECT_GT(Counter(table, "splits"), 0);
  for (u32 i = 0; i < 90; ++i) Check(table, keys[i], i < 40 && (i & 1) ? i + 1000 : i);
  for (u32 i = 0; i < 90; ++i) ASSERT_EQ(table.Remove(keys[i]), Result::SUCCESS);
  EXPECT_EQ(Counter(table, "size"), 0);
  for (u32 i = 0; i < 90; ++i) EXPECT_EQ(table.Search(keys[i], value), Result::FAILED);
}

TEST(Dash, CollidingStashHintsWithSameFingerprint) {
  Dash table(TestOptions());
  ASSERT_EQ(table.Create({}), Result::SUCCESS);
  std::vector<u64> keys;
  constexpr u32 home = 11, fingerprint = 0x5a, count = 48, first_stash = 28;
  // Use the same home, fingerprint, and initial directory segment. Of 48 records, 20 enter the stash,
  // exceeding the eight overflow-fingerprint slots provided by two adjacent ordinary buckets.
  for (u64 key = 1; keys.size() < count; ++key) {
    const auto hash = HashFunction(key);
    if ((hash >> 63) == 0 && ((hash >> 8) & 63) == home &&
        (hash & 255) == fingerprint) keys.push_back(key);
  }
  std::vector<u64> values(count);
  std::vector<bool> present(count, true);
  for (u32 i = 0; i < count; ++i) {
    values[i] = i;
    ASSERT_EQ(table.Insert(keys[i], values[i]), Result::SUCCESS);
  }
  EXPECT_EQ(Counter(table, "stash_inserts"), count - first_stash);
  EXPECT_EQ(Counter(table, "splits"), 0);
  auto verify_all = [&]() {
    for (u32 i = 0; i < count; ++i) {
      u64 value{};
      ASSERT_EQ(table.Search(keys[i], value), present[i] ? Result::SUCCESS : Result::FAILED)
          << "slot=" << i;
      if (present[i]) ASSERT_EQ(value, values[i]) << "slot=" << i;
    }
  };
  for (u32 i = first_stash; i < count; ++i) {
    values[i] = i + 1000;
    ASSERT_EQ(table.Update(keys[i], values[i]), Result::SUCCESS);
  }
  verify_all();
  // First delete records inserted later (usually represented only by overflow_count), then interleave deletion with earlier fingerprint records
  // to verify that representative replacement under fingerprint collisions does not hide remaining records.
  const std::array<u32, 20> order{
      47, 35, 29, 45, 32, 43, 28, 41, 31, 40,
      30, 46, 33, 44, 34, 42, 36, 39, 37, 38};
  for (auto i : order) {
    ASSERT_EQ(table.Remove(keys[i]), Result::SUCCESS);
    present[i] = false;
    verify_all();
  }
  EXPECT_EQ(Counter(table, "size"), first_stash);
  // Reinsert in reverse order to change stash allocation from the original insertion order, then delete in a different order again.
  for (u32 i = count; i-- > first_stash;) {
    values[i] = i + 2000;
    ASSERT_EQ(table.Insert(keys[i], values[i]), Result::SUCCESS);
    present[i] = true;
    verify_all();
  }
  for (u32 step = 0; step < count - first_stash; ++step) {
    const u32 i = first_stash + (step * 7) % (count - first_stash);
    ASSERT_EQ(table.Remove(keys[i]), Result::SUCCESS);
    present[i] = false;
    verify_all();
  }
  EXPECT_EQ(Counter(table, "size"), first_stash);
}

TEST(Dash, FullKeyAndValueChecksWithZeroFingerprintAndStashOverflow) {
  Dash table(TestOptions());
  ASSERT_EQ(table.Create({}), Result::SUCCESS);
  std::vector<u64> keys;
  for (u64 key = 1; keys.size() < 49; ++key) {
    const auto h = HashFunction(key);
    if ((h >> 63) == 0 && ((h >> 8) & 63) == 17 && (h & 255) == 0)
      keys.push_back(key);
  }
  // A non-present key shares all routing bits and the zero fingerprint with
  // 48 stored keys. Twenty records require stash, exceeding all hint slots.
  for (u32 i = 0; i < 48; ++i)
    ASSERT_EQ(table.Insert(keys[i], keys[i] ^ 0xcafe123456789abcULL), Result::SUCCESS);
  EXPECT_GT(Counter(table, "stash_inserts"), 8);
  EXPECT_EQ(Counter(table, "splits"), 0);
  constexpr u64 sentinel = 0x123456789abcdef0ULL;
  u64 value = sentinel;
  EXPECT_EQ(table.Search(keys.back(), value), Result::FAILED);
  EXPECT_EQ(value, sentinel);
  for (u32 i = 0; i < 48; ++i)
    Check(table, keys[i], keys[i] ^ 0xcafe123456789abcULL);
  // Reuse allocated slots and hints while preserving full key/value checks.
  for (u32 i = 0; i < 48; i += 3) {
    ASSERT_EQ(table.Remove(keys[i]), Result::SUCCESS);
    value = sentinel;
    EXPECT_EQ(table.Search(keys[i], value), Result::FAILED);
    EXPECT_EQ(value, sentinel);
    ASSERT_EQ(table.Insert(keys[i], ~keys[i]), Result::SUCCESS);
  }
  for (u32 i = 0; i < 48; ++i)
    Check(table, keys[i], i % 3 == 0 ? ~keys[i] : keys[i] ^ 0xcafe123456789abcULL);
}

TEST(Dash, DifferentialAgainstReferenceMap) {
  Dash table(TestOptions());
  ASSERT_EQ(table.Create({}), Result::SUCCESS);
  std::unordered_map<u64, u64> expected;
  for (u64 key = 0; key < 25000; ++key) {
    ASSERT_EQ(table.Insert(key, key ^ 0xfeedbeef), Result::SUCCESS);
    expected[key] = key ^ 0xfeedbeef;
  }
  std::mt19937_64 rng(1729);
  for (u32 i = 0; i < 100000; ++i) {
    const u64 key = rng() % 40000, value = rng();
    const auto it = expected.find(key);
    const bool exists = it != expected.end();
    switch (rng() % 4) {
      case 0:
        ASSERT_EQ(table.Insert(key, value), exists ? Result::FAILED : Result::SUCCESS);
        if (!exists) expected[key] = value;
        break;
      case 1:
        ASSERT_EQ(table.Update(key, value), exists ? Result::SUCCESS : Result::FAILED);
        if (exists) expected[key] = value;
        break;
      case 2:
        ASSERT_EQ(table.Remove(key), exists ? Result::SUCCESS : Result::FAILED);
        expected.erase(key);
        break;
      default:
        u64 output{};
        ASSERT_EQ(table.Search(key, output), exists ? Result::SUCCESS : Result::FAILED);
        if (exists) ASSERT_EQ(output, it->second);
    }
  }
  for (u64 key = 0; key < 40000; ++key) {
    u64 output{};
    const auto it = expected.find(key);
    ASSERT_EQ(table.Search(key, output), it == expected.end() ? Result::FAILED : Result::SUCCESS);
    if (it != expected.end()) ASSERT_EQ(output, it->second);
  }
  EXPECT_EQ(Counter(table, "size"), expected.size());
  EXPECT_GT(Counter(table, "splits"), 0);
}

TEST(Dash, ThirtyWritersAndDuplicateRace) {
  Dash table(TestOptions());
  ASSERT_EQ(table.Create({}), Result::SUCCESS);
  constexpr u32 threads = 30, per_thread = 1500;
  std::atomic<u64> failures{0}, duplicate_winners{0};
  std::barrier start(threads);
  std::vector<std::jthread> workers;
  for (u32 tid = 0; tid < threads; ++tid) workers.emplace_back([&, tid] {
    start.arrive_and_wait();
    if (table.Insert(UINT64_MAX, 42) == Result::SUCCESS) ++duplicate_winners;
    for (u64 i = 0; i < per_thread; ++i) {
      const u64 key = tid * per_thread + i;
      u64 value{};
      if (table.Insert(key, key + 1) != Result::SUCCESS ||
          table.Search(key, value) != Result::SUCCESS || value != key + 1) ++failures;
      if (table.Update(key, key + 2) != Result::SUCCESS) ++failures;
    }
  });
  workers.clear();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(duplicate_winners.load(), 1);
  EXPECT_EQ(Counter(table, "size"), threads * per_thread + 1);
  Check(table, UINT64_MAX, 42);
  for (u64 key = 0; key < threads * per_thread; ++key) Check(table, key, key + 2);
  for (u32 tid = 0; tid < threads; ++tid) workers.emplace_back([&, tid] {
    for (u64 i = 0; i < per_thread; ++i) {
      const u64 key = tid * per_thread + i;
      if (table.Remove(key) != Result::SUCCESS || table.Insert(key, key + 3) != Result::SUCCESS)
        ++failures;
    }
  });
  workers.clear();
  EXPECT_EQ(failures.load(), 0);
  for (u64 key = 0; key < threads * per_thread; ++key) Check(table, key, key + 3);
}

TEST(Dash, OptimisticReadersDuringSplitsUpdatesAndChurn) {
  Dash table(TestOptions());
  ASSERT_EQ(table.Create({}), Result::SUCCESS);
  constexpr u64 stable = 1024, churn_per_writer = 64;
  constexpr u32 threads = 6, inserts_per_writer = 3000;
  for (u64 key = 0; key < stable; ++key) ASSERT_EQ(table.Insert(key, key), Result::SUCCESS);
  for (u64 i = 0; i < threads * churn_per_writer; ++i)
    ASSERT_EQ(table.Insert(stable + i, (stable + i) ^ 0xbeef), Result::SUCCESS);
  std::atomic<bool> done{false};
  std::atomic<u64> failures{0}, reads{0};
  std::vector<std::jthread> readers;
  for (u32 tid = 0; tid < threads; ++tid) readers.emplace_back([&, tid] {
    u64 i = tid;
    while (!done.load()) {
      const auto key = i++ % stable;
      u64 value{};
      if (table.Search(key, value) != Result::SUCCESS || (value != key && value != ~key)) ++failures;
      const auto changing = stable + i % (threads * churn_per_writer);
      const auto result = table.Search(changing, value);
      if (result == Result::SUCCESS && value != (changing ^ 0xbeef)) ++failures;
      if (result != Result::SUCCESS && result != Result::FAILED) ++failures;
      ++reads;
    }
  });
  std::vector<std::jthread> writers;
  for (u32 tid = 0; tid < threads; ++tid) writers.emplace_back([&, tid] {
    for (u64 i = 0; i < inserts_per_writer; ++i) {
      const auto key = 100000 + tid * inserts_per_writer + i;
      if (table.Insert(key, key + 9) != Result::SUCCESS) ++failures;
      const auto hot = i % stable;
      if (table.Update(hot, (i & 1) ? hot : ~hot) != Result::SUCCESS) ++failures;
      const auto changing = stable + tid * churn_per_writer + i % churn_per_writer;
      if (table.Remove(changing) != Result::SUCCESS ||
          table.Insert(changing, changing ^ 0xbeef) != Result::SUCCESS) ++failures;
    }
  });
  writers.clear();
  done.store(true);
  readers.clear();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_GT(reads.load(), 0);
  EXPECT_EQ(Counter(table, "size"), stable + threads * churn_per_writer + threads * inserts_per_writer);
  for (u64 i = 0; i < threads * inserts_per_writer; ++i) Check(table, 100000 + i, 100000 + i + 9);
  for (u64 i = 0; i < threads * churn_per_writer; ++i) Check(table, stable + i, (stable + i) ^ 0xbeef);
}

TEST(Dash, FixedFastBudgetMetadataSpillAndSlowOnlyGrowth) {
  constexpr u64 fast_bytes = 24 * 1024, initial_slow_bytes = 64 * 1024;
  Dash table({1, -1, -1, fast_bytes, initial_slow_bytes, 64 * 1024});
  ASSERT_EQ(table.Create({}), Result::SUCCESS);
  ASSERT_EQ(Counter(table, "fast_reserved"), fast_bytes);
  ASSERT_EQ(Counter(table, "slow_reserved"), initial_slow_bytes);
  ASSERT_LE(Counter(table, "fast_used"), fast_bytes);
  const auto initial_fast_kv = Counter(table, "fast_kv");
  ASSERT_GT(initial_fast_kv, 0);
  ASSERT_EQ(Counter(table, "split_fast_kv"), 0);
  ASSERT_EQ(Counter(table, "split_slow_kv"), 0);

  // Small inclusive pools force both metadata spill and actual segment splits.
  // Every split-born KV must use the expandable slow pool, even if some fast
  // capacity remains reserved; the initial static fast KV allocation is fixed.
  constexpr u64 records = 60000, original_xor = 0x123456789abcdef0ull;
  constexpr u64 updated_xor = 0x0fedcba987654321ull;
  for (u64 key = 0; key < records; ++key)
    ASSERT_EQ(table.Insert(key, key ^ original_xor), Result::SUCCESS) << key;
  for (u64 key = 0; key < records; ++key) Check(table, key, key ^ original_xor);

  EXPECT_GT(Counter(table, "splits"), 0);
  EXPECT_EQ(Counter(table, "size"), records);
  EXPECT_EQ(Counter(table, "fast_reserved"), fast_bytes);
  EXPECT_LE(Counter(table, "fast_used"), fast_bytes);
  EXPECT_GT(Counter(table, "slow_reserved"), initial_slow_bytes);
  EXPECT_LE(Counter(table, "slow_used"), Counter(table, "slow_reserved"));
  EXPECT_GT(Counter(table, "slow_metadata"), 0);
  EXPECT_GT(Counter(table, "slow_growth_count"), 0);
  EXPECT_EQ(Counter(table, "fast_kv"), initial_fast_kv);
  EXPECT_EQ(Counter(table, "split_fast_kv"), 0);
  EXPECT_GT(Counter(table, "split_slow_kv"), 0);

  // CRUD must remain correct after spills and multiple slow-pool expansions.
  for (u64 key = 0; key < records; key += 7)
    ASSERT_EQ(table.Remove(key), Result::SUCCESS) << key;
  for (u64 key = 0; key < records; key += 11) if (key % 7 != 0)
    ASSERT_EQ(table.Update(key, key ^ updated_xor), Result::SUCCESS) << key;
  for (u64 key = 0; key < records; key += 7) {
    u64 value{};
    EXPECT_EQ(table.Search(key, value), Result::FAILED) << key;
    ASSERT_EQ(table.Insert(key, key + 17), Result::SUCCESS) << key;
  }
  for (u64 key = 0; key < records; ++key) {
    const auto expected = key % 7 == 0 ? key + 17 :
        key % 11 == 0 ? key ^ updated_xor : key ^ original_xor;
    Check(table, key, expected);
  }
  EXPECT_EQ(Counter(table, "size"), records);
  EXPECT_EQ(Counter(table, "fast_reserved"), fast_bytes);
  EXPECT_LE(Counter(table, "fast_used"), fast_bytes);
  EXPECT_EQ(Counter(table, "fast_kv"), initial_fast_kv);
  EXPECT_EQ(Counter(table, "split_fast_kv"), 0);
}

}  // namespace
