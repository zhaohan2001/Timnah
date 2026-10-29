// Private correctness, capacity-accounting and ratio-only D replay checks.
#include "common.hpp"
#include "timnah.h"
#include "storage.h"
#include "hashindex/chash/chash.h"
#include "percentile.hpp"

namespace {
constexpr u64 kKeys = 24000;
constexpr u64 kInserts = 1200;
constexpr u64 kSaltA = 0xaaaaaaaaaaaaaaaaULL;
constexpr u64 kSaltB = 0x5555555555555555ULL;
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
u64 NativeSize(const std::string& statistics) {
  const auto position = statistics.find("size=");
  Require(position != std::string::npos, "Native size statistic is missing");
  return std::stoull(statistics.substr(position + 5));
}
void CheckCapacity(unsigned depth) {
  using namespace TimnahNameSpace::detail;
  const u64 segments = u64{1} << depth;
  const u64 buckets = segments * kBuckets * sizeof(Bucket);
  const u64 counters = segments * kBuckets * sizeof(CounterBucket);
  Tier tier(-1, depth, false, false, [](u64 key) { return HashFunction(key); }, false);
  const u64 expected_metadata = 64 + segments * (sizeof(Entry) + sizeof(std::atomic<Entry*>));
  const u64 expected_reserved = buckets + std::max(counters, u64{64} << 20) + (u64{2} << 20);
  Require(tier.SegmentCount() == segments && tier.DirectoryDepth() == depth &&
          tier.BucketBytes() == buckets && tier.CounterBytes() == counters &&
          tier.MetadataBytes() == expected_metadata && tier.ReservedBytes() == expected_reserved,
          "Unmodified fixed-tier accounting does not match the expected allocator granularity");
  fmt::print("Figure14CapacityCorrectness fast_depth={} fast_bucket_bytes={} fast_counter_bytes={} "
             "fast_metadata_used_bytes={} fast_total_reserved_bytes={} "
             "counter_reserved_bytes={} metadata_reserved_bytes=2097152 "
             "budget_scope=fast_bucket_bytes_only structure_valid=true\n",
             depth, buckets, counters, tier.MetadataBytes(), tier.ReservedBytes(),
             std::max(counters, u64{64} << 20));
}
void Exercise(IndexInterface& index, const std::string& variant, TimnahNameSpace::Timnah* timnah) {
  u64 errors = 0;
#pragma omp parallel for num_threads(30) schedule(dynamic, 100) reduction(+:errors)
  for (u64 key = 1; key <= kKeys; ++key)
    errors += index.Insert(key, key ^ kSaltA) != Result::SUCCESS;
  Require(errors == 0 && NativeSize(index.Stat()) == kKeys, "Parallel preload failed");
  std::atomic<u64> concurrent_errors{0};
#pragma omp parallel num_threads(30)
  {
    const u64 worker = omp_get_thread_num();
    for (u64 operation = 0; operation < 6000; ++operation) {
      const u64 key = operation % 2 == 0 ? 1 : 1 + ((operation * 31 + worker * 997) % kKeys);
      if ((operation + worker) % 2) {
        const u64 value = key ^ ((operation & 2) ? kSaltA : kSaltB);
        if (index.Update(key, value) != Result::SUCCESS) ++concurrent_errors;
      } else {
        u64 value = 0;
        if (index.Search(key, value) != Result::SUCCESS ||
            (value != (key ^ kSaltA) && value != (key ^ kSaltB))) ++concurrent_errors;
      }
    }
  }
  Require(concurrent_errors == 0, "Concurrent read/update returned an invalid value");
  // Exact fixed D trace: 95% existing-key searches, 5% distinct insert keys.
  // Repeat the identical stream for ten rounds, matching current ordinary D.
  for (u64 round = 0; round < 10; ++round) {
    u64 reads = 0, inserted = 0, rejected = 0, unexpected = 0;
#pragma omp parallel for num_threads(30) schedule(dynamic, 100) reduction(+:reads,inserted,rejected,unexpected)
    for (u64 operation = 0; operation < kInserts * 20; ++operation) {
      if (operation % 20 == 0) {
        const auto key = kKeys + 1 + operation / 20;
        const auto result = index.Insert(key, key ^ kSaltA);
        inserted += result == Result::SUCCESS;
        rejected += result == Result::FAILED;
        unexpected += round == 0 ? result != Result::SUCCESS : result != Result::FAILED;
      } else {
        const auto key = 1 + operation % kKeys;
        u64 value = 0;
        const auto result = index.Search(key, value);
        ++reads;
        unexpected += result != Result::SUCCESS ||
                      (value != (key ^ kSaltA) && value != (key ^ kSaltB));
      }
    }
    Require(unexpected == 0 && reads == 19 * kInserts &&
            inserted == (round == 0 ? kInserts : 0) && rejected == (round == 0 ? 0 : kInserts),
            "Ratio-only D replay semantics are incorrect");
    const auto statistics = index.Stat();
    Require(NativeSize(statistics) == kKeys + kInserts, "D replay changed cardinality unexpectedly");
    fmt::print("Figure14ReplayCorrectness variant={} round={} operations={} search_operations={} "
               "insert_operations={} successful_new_inserts={} expected_duplicate_insert_rejections={} "
               "unexpected_failures=0 size={}\n",
               variant, round, kInserts * 20, reads, kInserts, inserted, rejected, kKeys + kInserts);
  }
  if (timnah) Require(timnah->DrainMaintenance() && timnah->Validate(), "Timnah structure or maintenance is invalid");
  // Remove half the records and check every remaining value after quiescence.
#pragma omp parallel for num_threads(30) schedule(dynamic, 100) reduction(+:errors)
  for (u64 key = 2; key <= kKeys + kInserts; key += 2)
    errors += index.Remove(key) != Result::SUCCESS;
  index.Stop();
  Require(errors == 0 && NativeSize(index.Stat()) == (kKeys + kInserts) / 2,
          "Parallel deletion or final cardinality is invalid");
#pragma omp parallel for num_threads(30) schedule(dynamic, 100) reduction(+:errors)
  for (u64 key = 1; key <= kKeys + kInserts; ++key) {
    u64 value = 0;
    const bool found = index.Search(key, value) == Result::SUCCESS;
    errors += found != bool(key % 2);
    if (found) errors += value != (key ^ kSaltA) && value != (key ^ kSaltB);
  }
  Require(errors == 0, "Post-deletion lookup/value verification failed");
  if (timnah) Require(timnah->Validate(), "Post-deletion Timnah structure is invalid");
  fmt::print("Figure14Correctness variant={} foreground_threads=30 loaded_keys={} "
             "mixed_operations=180000 replay_rounds=10 replay_operations=240000 "
             "new_keys={} deleted_keys={} final_keys={} unexpected_failures=0 structure_valid=true\n",
             variant, kKeys, kInserts, (kKeys + kInserts) / 2, (kKeys + kInserts) / 2);
}
}  // namespace

int main() {
  try {
    omp_set_dynamic(0);
    std::array<u64, 100> deterministic;
    std::iota(deterministic.begin(), deterministic.end(), u64{1});
    Require(deterministic[figure14::PercentileIndex(0.5, 100)] == 50 &&
            deterministic[figure14::PercentileIndex(0.99, 100)] == 99 &&
            figure14::PercentileIndex(0.5, 1) == 0,
            "Percentile order-statistic rule is incorrect");
    bool rejected_empty = false;
    try { figure14::PercentileIndex(0.5, 0); }
    catch (const std::invalid_argument&) { rejected_empty = true; }
    Require(rejected_empty, "The percentile helper accepted an empty population");
    fmt::print("Figure14PercentileCorrectness p50=50 p99=99 tiny_population=0 empty_population=rejected\n");
    for (unsigned depth : {12u, 11u, 10u}) {
      CheckCapacity(depth);
      TimnahNameSpace::Timnah::Options options;
      options.fast_depth = depth;
      options.slow_depth = 1;
      options.fast_node = options.slow_node = -1;
      options.prefault = false;
      options.huge_pages = false;
      options.sample_rate = 1.0;
      options.decay_interval_ms = 1;
      options.convergence_threshold = 0;
      TimnahNameSpace::Timnah index(options);
      IndexInterface::ParamType parameters{};
      parameters.num_bg = 1;
      Require(index.Create(parameters) == Result::SUCCESS, "Timnah creation failed");
      const auto diagnostics = index.Snapshot();
      Require(diagnostics.fast_bucket_bytes == (u64{1} << depth) * 65536 &&
              diagnostics.fast_counter_bytes == (u64{1} << depth) * 16384,
              "Timnah capacity option was not applied");
      Exercise(index, depth == 12 ? "Timnah-A" : depth == 11 ? "Timnah-B" : "Timnah-C", &index);
    }
    // A deliberately small tier configuration also exercises growth/splitting.
    {
      TimnahNameSpace::Timnah::Options options;
      options.fast_depth = options.slow_depth = 1;
      options.fast_node = options.slow_node = -1;
      options.prefault = options.huge_pages = false;
      options.sample_rate = 1;
      options.decay_interval_ms = 1;
      options.convergence_threshold = 0;
      TimnahNameSpace::Timnah index(options);
      IndexInterface::ParamType parameters{};
      parameters.num_bg = 1;
      Require(index.Create(parameters) == Result::SUCCESS, "Small Timnah creation failed");
      Exercise(index, "Timnah-small-split", &index);
      Require(index.Snapshot().slow_splits > 0, "The small Timnah test did not exercise splitting");
    }
    {
      CHashNameSpace::CHash::Options options;
      options.initial_depth = 1;
      options.dram_node = options.slow_node = -1;
      options.fast_bytes = 256ull << 20;
      options.slow_initial_bytes = 16ull << 20;
      options.slow_growth_bytes = 4ull << 20;
      CHashNameSpace::CHash index(options);
      IndexInterface::ParamType parameters{};
      Require(index.Create(parameters) == Result::SUCCESS, "CHash creation failed");
      Exercise(index, "CHash", nullptr);
    }
    fmt::print("Figure14CorrectnessComplete variants=4 additional_split_configuration=1 "
               "capacity_settings=3 replay_rounds_per_configuration=10 all_checks_passed=true\n");
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "Figure14CorrectnessError {}\n", error.what());
    return 1;
  }
}
