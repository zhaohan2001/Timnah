#include "tiered_memory.h"
#include <gtest/gtest.h>
#include <vector>
#include <filesystem>

namespace {
using TierBudget::Allocator;
using TierBudget::Config;
using TierBudget::Memory;
constexpr size_t kPage = 4096;
Config Tiny() { return {-1, -1, kPage, kPage, kPage, false}; }
bool Aligned(const void* p) { return reinterpret_cast<uintptr_t>(p) % 64 == 0; }
void Consistent(const TierBudget::Stats& s) {
  EXPECT_LE(s.fast_used, s.fast_reserved);
  EXPECT_LE(s.slow_used, s.slow_reserved);
  EXPECT_EQ(s.fast_used, s.fast_metadata + s.fast_kv);
  EXPECT_EQ(s.slow_used, s.slow_metadata + s.slow_kv);
  EXPECT_EQ(s.split_fast_kv, 0);
  EXPECT_LE(s.split_slow_kv, s.slow_kv);
}

TEST(TierBudget, FixedReservationAndMetadataSpill) {
  Memory memory(Tiny());
  auto before = memory.GetStats();
  EXPECT_EQ(before.fast_reserved, kPage);
  EXPECT_EQ(before.slow_reserved, kPage);
  EXPECT_EQ(before.slow_metadata, 64);  // Intrusive mapping descriptor is budgeted.
  auto* a = memory.Metadata(65);
  auto* b = memory.Metadata(kPage - 128);
  auto* c = memory.Metadata(1);
  ASSERT_TRUE(a && b && c);
  EXPECT_TRUE(Aligned(a) && Aligned(b) && Aligned(c));
  auto s = memory.GetStats();
  EXPECT_EQ(s.fast_reserved, kPage);
  EXPECT_EQ(s.fast_used, kPage);
  EXPECT_EQ(s.fast_metadata, kPage);
  EXPECT_EQ(s.slow_metadata, 128);
  EXPECT_EQ(s.slow_growth_count, 0);
  Consistent(s);
}

TEST(TierBudget, StaticInitialPlacementAndSplitAlwaysSlow) {
  Memory memory(Tiny());
  for (size_t i = 0; i < 10; ++i) {
    auto* p = memory.InitialKV(33, i);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(Aligned(p));
  }
  auto s = memory.GetStats();
  EXPECT_EQ(s.fast_kv, 2 * 64);
  EXPECT_EQ(s.slow_kv, 8 * 64);
  EXPECT_NE(memory.SplitKV(128), nullptr);
  s = memory.GetStats();
  EXPECT_EQ(s.fast_kv, 2 * 64);
  EXPECT_EQ(s.split_fast_kv, 0);
  EXPECT_EQ(s.split_slow_kv, 128);
  EXPECT_EQ(s.slow_kv, 8 * 64 + 128);
  Consistent(s);
}

TEST(TierBudget, FastExhaustionDoesNotGrowFast) {
  Memory memory(Tiny());
  ASSERT_NE(memory.Metadata(kPage), nullptr);
  ASSERT_NE(memory.InitialKV(128, 0), nullptr);
  ASSERT_NE(memory.SplitKV(kPage - 64 - 128), nullptr);
  auto s = memory.GetStats();
  EXPECT_EQ(s.slow_used, kPage);
  ASSERT_NE(memory.SplitKV(1), nullptr);
  s = memory.GetStats();
  EXPECT_EQ(s.fast_reserved, kPage);
  EXPECT_EQ(s.fast_used, kPage);
  EXPECT_EQ(s.fast_kv, 0);
  EXPECT_EQ(s.slow_reserved, 2 * kPage);
  EXPECT_EQ(s.slow_growth_count, 1);
  EXPECT_EQ(s.slow_metadata, 2 * 64);
  Consistent(s);
}

TEST(TierBudget, LargeRequestGrowthIncludesHeaderAndPageAlignment) {
  Memory memory(Tiny());
  EXPECT_NE(memory.SplitKV(8193), nullptr);
  const auto s = memory.GetStats();
  EXPECT_EQ(s.fast_reserved, kPage);
  EXPECT_EQ(s.slow_reserved, kPage + 12288);
  EXPECT_EQ(s.slow_growth_count, 1);
  EXPECT_EQ(s.split_slow_kv, 8256);
  EXPECT_EQ(s.slow_metadata, 128);
  EXPECT_EQ(s.slow_used, 128 + 8256);
  Consistent(s);
}

TEST(TierBudget, ValidationZeroAndOverflow) {
  Memory memory(Tiny());
  EXPECT_EQ(memory.Metadata(0), nullptr);
  EXPECT_EQ(memory.InitialKV(0, 0), nullptr);
  EXPECT_EQ(memory.SplitKV(0), nullptr);
  EXPECT_THROW(memory.Metadata(std::numeric_limits<size_t>::max()), std::bad_alloc);
  EXPECT_THROW(memory.SplitKV(std::numeric_limits<size_t>::max() - 63), std::bad_alloc);
  Config config = Tiny();
  config.fast_node = -2;
  EXPECT_THROW({ Memory invalid(config); }, std::invalid_argument);
  config = Tiny();
  config.fast_bytes = 4095;
  EXPECT_THROW({ Memory invalid(config); }, std::invalid_argument);
  config = Tiny();
  config.slow_initial_bytes = 0;
  EXPECT_THROW({ Memory invalid(config); }, std::invalid_argument);
  config = Tiny();
  config.fast_bytes = 0;
  Memory zero_fast(config);
  EXPECT_NE(zero_fast.Metadata(64), nullptr);
  EXPECT_EQ(zero_fast.GetStats().fast_reserved, 0);
  EXPECT_EQ(zero_fast.GetStats().fast_used, 0);
}

TEST(TierBudget, STLStorageUsesMetadataBudget) {
  Memory memory(Tiny());
  Allocator<uint64_t> a(memory);
  Allocator<std::byte> rebound(a);
  EXPECT_EQ(a, Allocator<uint64_t>(&memory));
  EXPECT_EQ(a, rebound);
  {
    std::vector<uint64_t, Allocator<uint64_t>> values{a};
    for (uint64_t i = 0; i < 1000; ++i) values.push_back(i * 3);
    ASSERT_EQ(values.size(), 1000);
    for (uint64_t i = 0; i < 1000; ++i) EXPECT_EQ(values[i], i * 3);
    EXPECT_TRUE(Aligned(values.data()));
  }
  const auto s = memory.GetStats();
  EXPECT_GT(s.fast_metadata + s.slow_metadata, 1000 * sizeof(uint64_t));
  EXPECT_EQ(s.fast_kv + s.slow_kv, 0);
  Consistent(s);
  Allocator<uint64_t> empty;
  EXPECT_THROW(empty.allocate(1), std::bad_alloc);
}

TEST(TierBudget, PrefaultDescribeAndOrdinaryPlacement) {
  auto c = Tiny();
  c.prefault = true;
  Memory memory(c);
  auto* p = static_cast<uint64_t*>(memory.Metadata(64));
  ASSERT_NE(p, nullptr);
  *p = 1234;
  EXPECT_EQ(*p, 1234);
  EXPECT_NE(memory.Describe().find("fast_reserved=4096"), std::string::npos);
  EXPECT_NE(memory.Describe().find("split_fast_kv=0"), std::string::npos);
  EXPECT_NE(memory.Placement().find("policy_checked_mappings=0"), std::string::npos);
}

#ifdef __linux__
TEST(TierBudget, DestructorUnmapsBothPools) {
  void* fast = nullptr;
  void* slow = nullptr;
  {
    Memory memory(Tiny());
    fast = memory.Metadata(64);
    slow = memory.SplitKV(64);
  }
  unsigned char residency = 0;
  errno = 0;
  const int fast_rc = mincore(fast, kPage, &residency);
  const int fast_error = errno;
  // Slow payload follows its64-byte in-mapping descriptor.
  void* slow_base = static_cast<std::byte*>(slow) - 64;
  errno = 0;
  const int slow_rc = mincore(slow_base, kPage, &residency);
  const int slow_error = errno;
  EXPECT_EQ(fast_rc, -1);
  EXPECT_EQ(fast_error, ENOMEM);
  EXPECT_EQ(slow_rc, -1);
  EXPECT_EQ(slow_error, ENOMEM);
}
#endif
#ifdef __linux__
TEST(TierBudget, RealNodeZeroAndTwoBinding) {
  if (!std::filesystem::exists("/sys/devices/system/node/node2"))
    GTEST_SKIP() << "Requires node0 and node2";
  auto config = Tiny();
  config.fast_node = 0;
  config.slow_node = 2;
  config.prefault = true;
  Memory memory(config);
  ASSERT_NE(memory.Metadata(64), nullptr);
  ASSERT_NE(memory.SplitKV(64), nullptr);
  const auto placement = memory.Placement();
  EXPECT_NE(placement.find("policy_checked_mappings=2"), std::string::npos);
  if (placement.find("page_query=sampled") != std::string::npos) {
    EXPECT_NE(placement.find("all_sampled_pages_match=1"), std::string::npos) << placement;
  }
}
#endif

}  // namespace
