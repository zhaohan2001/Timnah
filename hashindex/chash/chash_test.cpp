#include "chash.h"

using CHashNameSpace::CHash;
namespace {
CHash::Options TestOptions() { return {1, -1, -1, 64 * 1024, 128 * 1024, 128 * 1024}; }
void Check(CHash& table, u64 key, u64 expected) {
  u64 value = ~expected;
  ASSERT_EQ(table.Search(key, value), Result::SUCCESS) << key;
  ASSERT_EQ(value, expected) << key;
}


u64 Counter(CHash& table, const std::string& name) {
  const auto stats = table.Stat();
  const auto pos = stats.find(name + "=");
  if (pos == std::string::npos) {
    ADD_FAILURE() << "Missing counter " << name << " in " << stats;
    return 0;
  }
  return std::stoull(stats.substr(pos + name.size() + 1));
}

TEST(CHash, LifecycleAndZeroKeys) {
  CHash table(TestOptions());
  u64 v = 42;
  EXPECT_EQ(table.Search(0, v), Result::FAILED);
  ASSERT_EQ(table.Create({}), Result::SUCCESS);
  EXPECT_EQ(table.Create({}), Result::FAILED);
  EXPECT_EQ(table.Insert(0, 0), Result::SUCCESS);
  EXPECT_EQ(table.Insert(0, 5), Result::FAILED);
  Check(table, 0, 0);
  EXPECT_EQ(table.Update(0, UINT64_MAX), Result::SUCCESS);
  Check(table, 0, UINT64_MAX);
  EXPECT_EQ(table.Insert(UINT64_MAX, 0), Result::SUCCESS);
  Check(table, UINT64_MAX, 0);
  EXPECT_EQ(table.Remove(0), Result::SUCCESS);
  EXPECT_EQ(table.Remove(0), Result::FAILED);
  EXPECT_EQ(table.Update(0, 3), Result::FAILED);
  EXPECT_EQ(table.Search(0, v), Result::FAILED);
  EXPECT_EQ(table.Insert(0, 7), Result::SUCCESS);
  Check(table, 0, 7);
  table.Stop();
  table.Stop();
}

TEST(CHash, RejectInvalidConfiguration) {
  CHash zero({0, -1, -1}), deep({25, -1, -1}), bad_node({1, -2, -1});
  EXPECT_EQ(zero.Create({}), Result::FAILED);
  EXPECT_EQ(deep.Create({}), Result::FAILED);
  EXPECT_EQ(bad_node.Create({}), Result::FAILED);
}

TEST(CHash, FlushSplitDeleteReuseAndDifferential) {
  CHash table(TestOptions());
  ASSERT_EQ(table.Create({}), Result::SUCCESS);
  std::unordered_map<u64, u64> expected;
  // Start at depth 1 to exercise multiple bucket flushes, splits, and directory doublings.
  for (u64 key = 0; key < 30000; ++key) {
    ASSERT_EQ(table.Insert(key, key ^ 0xa55a), Result::SUCCESS);
    expected[key] = key ^ 0xa55a;
  }
  for (const auto& [k, v] : expected) Check(table, k, v);
  for (u64 key = 0; key < 30000; key += 2) {
    ASSERT_EQ(table.Remove(key), Result::SUCCESS);
    expected.erase(key);
  }
  std::mt19937_64 rng(1729);
  for (u32 i = 0; i < 100000; ++i) {
    const u64 key = rng() % 50000, value = rng();
    auto found = expected.find(key);
    const bool exists = found != expected.end();
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
        u64 out{};
        ASSERT_EQ(table.Search(key, out), exists ? Result::SUCCESS : Result::FAILED);
        if (exists) ASSERT_EQ(out, found->second);
    }
  }
  for (u64 key = 0; key < 50000; ++key) {
    u64 value{};
    const auto it = expected.find(key);
    ASSERT_EQ(table.Search(key, value), it == expected.end() ? Result::FAILED : Result::SUCCESS);
    if (it != expected.end()) ASSERT_EQ(value, it->second);
  }
  EXPECT_NE(table.Stat().find("splits="), std::string::npos);
}

TEST(CHash, ThirtyWritersAndDuplicateRace) {
  CHash table(TestOptions());
  ASSERT_EQ(table.Create({}), Result::SUCCESS);
  constexpr u32 threads = 30, per_thread = 2000;
  std::atomic<u64> failures{0}, duplicate_winners{0};
  std::barrier start(threads);
  std::vector<std::jthread> workers;
  for (u32 tid = 0; tid < threads; ++tid) workers.emplace_back([&, tid] {
    start.arrive_and_wait();
    if (table.Insert(UINT64_MAX, 42) == Result::SUCCESS) ++duplicate_winners;
    for (u64 i = 0; i < per_thread; ++i) {
      u64 key = tid * per_thread + i, value{};
      if (table.Insert(key, key + 1) != Result::SUCCESS ||
          table.Search(key, value) != Result::SUCCESS || value != key + 1) ++failures;
      if (table.Update(key, key + 2) != Result::SUCCESS) ++failures;
    }
  });
  workers.clear();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(duplicate_winners.load(), 1);
  for (u64 key = 0; key < threads * per_thread; ++key) Check(table, key, key + 2);
  Check(table, UINT64_MAX, 42);
  for (u32 tid = 0; tid < threads; ++tid) workers.emplace_back([&, tid] {
    for (u64 i = 0; i < per_thread; ++i) {
      u64 key = tid * per_thread + i;
      if (table.Remove(key) != Result::SUCCESS || table.Insert(key, key + 3) != Result::SUCCESS)
        ++failures;
    }
  });
  workers.clear();
  EXPECT_EQ(failures.load(), 0);
  for (u64 key = 0; key < threads * per_thread; ++key) Check(table, key, key + 3);
}

TEST(CHash, OptimisticReadersDuringSplitsAndUpdates) {
  CHash table(TestOptions());
  ASSERT_EQ(table.Create({}), Result::SUCCESS);
  constexpr u64 stable = 1000;
  for (u64 key = 0; key < stable; ++key) ASSERT_EQ(table.Insert(key, key), Result::SUCCESS);
  std::atomic<bool> done{false};
  std::atomic<u64> failures{0}, reads{0};
  std::vector<std::jthread> readers;
  for (u32 tid = 0; tid < 6; ++tid) readers.emplace_back([&, tid] {
    u64 i = tid;
    while (!done.load()) {
      u64 key = i++ % stable, value{};
      if (table.Search(key, value) != Result::SUCCESS || (value != key && value != ~key)) ++failures;
      ++reads;
    }
  });
  std::vector<std::jthread> writers;
  for (u32 tid = 0; tid < 6; ++tid) writers.emplace_back([&, tid] {
    for (u64 i = tid; i < 40000; i += 6) {
      if (table.Insert(stable + i, i) != Result::SUCCESS) ++failures;
      const auto key = i % stable;
      if (table.Update(key, (i & 1) ? key : ~key) != Result::SUCCESS) ++failures;
    }
  });
  writers.clear();
  done.store(true);
  readers.clear();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_GT(reads.load(), 0);
  for (u64 i = 0; i < 40000; ++i) Check(table, stable + i, i);
}

TEST(CHash, FixedFastBudgetMetadataSpillAndSlowOnlyGrowth) {
  constexpr u64 fast_bytes = 24 * 1024, initial_slow_bytes = 64 * 1024;
  CHash table({1, -1, -1, fast_bytes, initial_slow_bytes, 64 * 1024});
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
