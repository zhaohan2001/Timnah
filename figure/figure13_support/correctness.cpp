// Isolated concurrency regression checks for genuine incremental lock designs.
#include "common.hpp"
#include "timnah.h"
#include "storage.h"
#include "percentile.hpp"

namespace {
constexpr u64 kKeys = 24000;
constexpr u64 kSaltA = 0xaaaaaaaaaaaaaaaaULL;
constexpr u64 kSaltB = 0x5555555555555555ULL;
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}
int main() {
  try {
    // Percentiles are selected from actual sampled operation durations, with
    // a deterministic order-statistic rule independently checked here.
    std::array<u64, 100> deterministic;
    std::iota(deterministic.begin(), deterministic.end(), u64{1});
    Require(deterministic[figure13::PercentileIndex(0.5, 100)] == 50 &&
            deterministic[figure13::PercentileIndex(0.99, 100)] == 99 &&
            figure13::PercentileIndex(0.5, 1) == 0,
            "Percentile order-statistic rule is incorrect");
    {
      // The Key, ordinary Value and Hot versions are physically distinct.
      // The ablation source selects only Key for Version Lock variants, one
      // Value word for K/V and Migration, and Hot only for fast-tier slot zero.
      using namespace TimnahNameSpace::detail;
      Bucket bucket;
      Require(bucket.TryValueLock(), "Cannot acquire Value Lock");
      Require(bucket.KeyState() == 0 && Read(bucket.hot_version) == 0,
              "Value Lock unexpectedly changed Key or Hot Lock");
      Require(bucket.TryKeyLock() && bucket.TryHotLock(),
              "Key/Hot locks are not independent of Value Lock");
      Require((bucket.KeyState() & 1) && (Read(bucket.value_version) & 1) &&
              (Read(bucket.hot_version) & 1), "Independent lock state is invalid");
      bucket.HotUnlock(); bucket.KeyUnlock(); bucket.ValueUnlock();
      Require(bucket.KeyState() == 2 && Read(bucket.value_version) == 2 &&
              Read(bucket.hot_version) == 2, "Version increments are invalid");
    }
    omp_set_dynamic(0);
    TimnahNameSpace::Timnah::Options options;
    options.fast_depth = 1;
    options.slow_depth = 1;
    options.fast_node = -1;
    options.slow_node = -1;
    options.prefault = false;
    options.huge_pages = false;
    options.sample_rate = 1.0;
    options.decay_interval_ms = 1;
    options.convergence_threshold = 0;
    TimnahNameSpace::Timnah index(options);
    IndexInterface::ParamType parameters{};
    parameters.num_bg = FIGURE13_VARIANT >= 3 ? 1 : 0;
    Require(index.Create(parameters) == Result::SUCCESS, "Create failed");
    u64 errors = 0;
#pragma omp parallel for num_threads(30) schedule(dynamic, 100) reduction(+:errors)
    for (u64 key = 1; key <= kKeys; ++key)
      errors += index.Insert(key, key ^ kSaltA) != Result::SUCCESS;
    Require(errors == 0, "Concurrent insertion failed");
    Require(index.Snapshot().size == kKeys && index.Validate(), "Initial structure/cardinality invalid");
    const auto inspected = index.Inspect(1);
    Require(inspected.found && inspected.copies == 1 && inspected.value == (1 ^ kSaltA),
            "Quiescent inspection failed or found duplicate copies");

    // Shared hottest key plus a broad key set exercises value/key-version
    // conflicts, hot-slot isolation and concurrent background promotion.
    std::atomic<u64> concurrent_errors{0};
#pragma omp parallel num_threads(30)
    {
      const u64 worker = omp_get_thread_num();
      for (u64 operation = 0; operation < 12000; ++operation) {
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
    Require(concurrent_errors == 0, "Mixed updates/searches returned missing or invalid values");
    Require(index.DrainMaintenance(), "Maintenance failed to drain");
    Require(index.Snapshot().size == kKeys && index.Validate(), "Post-update structure/cardinality invalid");

    // Concurrent new insertions force growable-tier splitting while other
    // workers update/search already published records.
#pragma omp parallel num_threads(30)
    {
      const u64 worker = omp_get_thread_num();
      if (worker < 15) {
        for (u64 key = kKeys + 1 + worker; key <= 2 * kKeys; key += 15)
          if (index.Insert(key, key ^ kSaltA) != Result::SUCCESS) ++concurrent_errors;
      } else {
        for (u64 operation = 0; operation < 10000; ++operation) {
          const u64 key = 1 + ((operation * 101 + worker * 239) % kKeys);
          if (operation % 2) {
            if (index.Update(key, key ^ kSaltB) != Result::SUCCESS) ++concurrent_errors;
          } else {
            u64 value = 0;
            if (index.Search(key, value) != Result::SUCCESS ||
                (value != (key ^ kSaltA) && value != (key ^ kSaltB))) ++concurrent_errors;
          }
        }
      }
    }
    Require(concurrent_errors == 0, "Concurrent splits/read/update failed");
    Require(index.DrainMaintenance() && index.Validate(), "Post-split structure invalid");
    Require(index.Snapshot().size == 2 * kKeys, "Post-split cardinality mismatch");

#pragma omp parallel for num_threads(30) schedule(dynamic, 100) reduction(+:errors)
    for (u64 key = 2; key <= 2 * kKeys; key += 2)
      errors += index.Remove(key) != Result::SUCCESS;
    Require(errors == 0, "Concurrent deletion failed");
    index.Stop();
    Require(index.Validate() && index.Snapshot().size == kKeys, "Post-delete structure/cardinality invalid");
#pragma omp parallel for num_threads(30) schedule(dynamic, 100) reduction(+:errors)
    for (u64 key = 1; key <= 2 * kKeys; ++key) {
      u64 value = 0;
      const bool found = index.Search(key, value) == Result::SUCCESS;
      errors += found != bool(key % 2);
      if (found) errors += value != (key ^ kSaltA) && value != (key ^ kSaltB);
    }
    Require(errors == 0, "Post-delete lookup/value verification failed");
    const auto d = index.Snapshot();
    if (FIGURE13_VARIANT < 3)
      Require(d.samples == 0 && d.reorders == 0 && d.promotions == 0 && d.decay_events == 0,
              "Disabled maintenance produced activity");
    else
      Require(d.samples > 0 && d.reorders > 0 && d.promotions > 0 && d.decay_events > 0,
              "Enabled maintenance was not exercised by the stress test");
    fmt::print("Figure13Correctness variant_id={} loaded_keys={} mixed_operations=360000 "
               "concurrent_split_insertions={} split_phase_existing_operations=150000 "
               "deleted_keys={} final_keys={} failed_operations=0 structure_valid=true "
               "samples={} reorders={} promotions={} decay_events={} "
               "arena_size_bytes={} tier_size_bytes={} bucket_size_bytes={}\n",
               FIGURE13_VARIANT_ID, kKeys, kKeys, kKeys, kKeys,
               d.samples, d.reorders, d.promotions, d.decay_events,
               sizeof(TimnahNameSpace::detail::Arena), sizeof(TimnahNameSpace::detail::Tier),
               sizeof(TimnahNameSpace::detail::Bucket));
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "Figure13CorrectnessError variant_id={} {}\n", FIGURE13_VARIANT_ID, error.what());
    return 1;
  }
}
