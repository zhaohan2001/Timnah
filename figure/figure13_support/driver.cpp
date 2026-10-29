// Ordinary whole-round measurement over isolated incremental Timnah variants.
// The copied full Hot Lock implementation is byte-identical to production.
#include "common.hpp"
#include "parameter.hpp"
#include "trace.hpp"
#include "timnah.h"
#include "percentile.hpp"

DEFINE_string(index, "Timnah", "Figure 13 isolated Timnah incremental design");

namespace {
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct alignas(64) Counts { u64 successful{}, failed{}, reads{}, updates{}; };
struct RoundResult {
  u64 elapsed_ns{}, successful{}, failed{}, reads{}, updates{}, samples{}, p50{}, p99{};
};
Result Serve(IndexInterface& index, Request request) {
  u64 value;
  switch (request.type) {
    case RequestType::Read:
    case RequestType::NegativeRead: return index.Search(request.key, value);
    case RequestType::Update: return index.Update(request.key, request.value);
    case RequestType::Insert: return index.Insert(request.key, request.value);
    case RequestType::Remove: return index.Remove(request.key);
  }
  std::abort();
}
RoundResult RunRound(IndexInterface& index, const Trace& trace) {
  // Mirror native Benchmark::EvalRunPhase's complete-round timing, allocation,
  // latency clocks and request dispatch. Reduction checks are outside timing.
  StopWatch watch;
  std::vector<std::vector<high_resolution_clock::time_point>> samples(FLAGS_num_run_threads);
  std::vector<Counts> counters(FLAGS_num_run_threads);
#pragma omp parallel num_threads(FLAGS_num_run_threads)
  {
    Counts local;
    std::vector<high_resolution_clock::time_point> times;
    times.resize(FLAGS_num_run * FLAGS_latency_sample_rate * 2.5);
    times.resize(0);
    Dice<256> dice(FLAGS_latency_sample_rate);
#pragma omp barrier
#pragma omp for schedule(dynamic, 100000)
    for (u64 position = 0; position < FLAGS_num_run; ++position) {
      const bool sampled = FLAGS_latency ? dice.Roll() : false;
      if (sampled) times.push_back(high_resolution_clock::now());
      const auto request = trace.GetRunRequest(position);
      const auto result = Serve(index, request);
      if (sampled) times.push_back(high_resolution_clock::now());
      local.successful += result == Result::SUCCESS;
      local.failed += result != Result::SUCCESS;
      local.reads += request.type == RequestType::Read;
      local.updates += request.type == RequestType::Update;
    }
    samples[omp_get_thread_num()].swap(times);
    counters[omp_get_thread_num()] = local;
  }
  RoundResult result;
  result.elapsed_ns = watch.Elapsed();
  std::vector<u64> latencies;
  for (const auto& times : samples) {
    Require(times.size() % 2 == 0, "Incomplete latency sample pair");
    for (size_t i = 0; i < times.size(); i += 2)
      latencies.push_back(duration_cast<nanoseconds>(times[i + 1] - times[i]).count());
  }
  for (const auto& item : counters) {
    result.successful += item.successful; result.failed += item.failed;
    result.reads += item.reads; result.updates += item.updates;
  }
  Require(!latencies.empty(), "No latency samples were collected");
  std::sort(latencies.begin(), latencies.end());
  result.samples = latencies.size();
  result.p50 = latencies[figure13::PercentileIndex(0.5, latencies.size())];
  result.p99 = latencies[figure13::PercentileIndex(0.99, latencies.size())];
  Require(result.failed == 0, "An index operation failed");
  Require(result.successful == FLAGS_num_run && result.reads + result.updates == FLAGS_num_run,
          "The round operation counts are inconsistent");
  Require(result.elapsed_ns > 0, "The round elapsed time is nonpositive");
  return result;
}
void Memory(const TimnahNameSpace::Timnah& index, const char* phase) {
  const auto d = index.Snapshot();
  fmt::print("Figure13Memory phase={} fast_bucket_bytes={} fast_counter_bytes={} "
             "slow_bucket_bytes={} slow_counter_bytes={} fast_segments={} slow_segments={} "
             "fast_node=0 slow_node=2 metadata_node=0 "
             "fast_bucket_capacity_bytes=268435456 fast_counter_capacity_bytes=67108864 "
             "budget_scope=match_current_Timnah_bucket_and_counter_capacities "
             "reported_payload_scope=bucket_and_counter_bytes_only "
             "metadata_and_arena_reservation_included=false "
             "placement_policy={}\n", phase, d.fast_bucket_bytes, d.fast_counter_bytes,
             d.slow_bucket_bytes, d.slow_counter_bytes, d.fast_segments, d.slow_segments,
             FIGURE13_VARIANT == 0 ? "single_directory_first_4096_segments_fast_remainder_slow"
                                   : "separate_fixed_fast_and_growable_slow_tables");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Parameter::Init(argc, argv);
    Require(FLAGS_index == "Timnah", "Figure 13 requires index Timnah");
    Require(FLAGS_workload == "a" || FLAGS_workload == "c" || FLAGS_workload == "ts" || FLAGS_workload == "w",
            "Figure 13 supports only workloads a, c, ts, w");
    Require(FLAGS_num_threads == 30 && FLAGS_num_load_threads == 30 && FLAGS_num_run_threads == 30,
            "Figure 13 requires 30 foreground threads");
    Require(FLAGS_num_rounds == 10 && FLAGS_num_load >= 10000 && FLAGS_num_run >= 10000,
            "Figure 13 requires ten rounds and at least 10000 records/operations");
    Require(FLAGS_num_bg == (FIGURE13_VARIANT >= 3 ? 1u : 0u), "Incorrect background worker count");
    for (const auto& option : std::array<std::pair<const char*, const char*>, 4>{
             {{"timnah_fast_depth", "12"}, {"timnah_slow_depth", "14"},
              {"timnah_fast_node", "0"}, {"timnah_slow_node", "2"}}}) {
      std::string actual;
      Require(gflags::GetCommandLineOption(option.first, &actual) && actual == option.second,
              "Figure 13 memory comparison requires default depths 12/14 and nodes 0/2");
    }
    Require(FLAGS_latency && FLAGS_latency_sample_rate == 0.01 && FLAGS_bind_core && !FLAGS_verify &&
            !FLAGS_shift && FLAGS_shuffle && FLAGS_keySize == 8 && FLAGS_valueSize == 8,
            "Figure 13 requires ordinary sampled latency, CPU binding and 8-byte shuffled records");
    Require(numa_run_on_node(0) == 0, "Cannot bind execution to CPU node 0");
    omp_set_dynamic(0);
    TimnahNameSpace::Timnah index;
    Require(index.Create(Parameter::GetIndexParam()) == Result::SUCCESS, "Index creation failed");
    fmt::print("Figure13Config variant_id={} workload={} num_threads=30 num_load_threads=30 "
               "num_run_threads=30 num_rounds=10 num_bg={} num_load={} num_run={} "
               "tiered={} kv_lock={} data_migration={} hot_lock={} "
               "latency=true latency_sample_rate=0.01 cpu_node=0 fast_node=0 slow_node=2 "
               "full_hot_source=unchanged_production_snapshot "
               "metric=whole_round_successful_operations/elapsed_seconds "
               "latency_unit=ns quantile_method=floor_probability_times_samples_minus_one "
               "timing=native_round_allocation_barrier_dispatch_sampling_and_operation_wall_time "
               "excluded=trace_generation_latency_analysis_statistics_stop_destructor\n",
               FIGURE13_VARIANT_ID, FLAGS_workload, FLAGS_num_bg, FLAGS_num_load, FLAGS_num_run,
               int(FIGURE13_VARIANT >= 1), int(FIGURE13_VARIANT >= 2),
               int(FIGURE13_VARIANT >= 3), int(FIGURE13_VARIANT >= 4));
    Memory(index, "initial");
    std::unique_ptr<Trace> trace;
    if (synthetic::IsProfileCode(FLAGS_workload))
      trace = std::make_unique<Trace>(synthetic::GetProfile(FLAGS_workload),
                                      FLAGS_num_load, FLAGS_num_run, FLAGS_rndSeed);
    else {
      const RequestTypeRatio ratios{FLAGS_read_ratio, FLAGS_update_ratio, FLAGS_insert_ratio,
                                    FLAGS_remove_ratio, FLAGS_negative_ratio};
      trace = std::make_unique<Trace>(FLAGS_num_load, FLAGS_num_run, FLAGS_skewness, ratios);
    }
    u64 successful = 0, failed = 0;
#pragma omp parallel for num_threads(FLAGS_num_load_threads) schedule(dynamic, 100000) reduction(+:successful,failed)
    for (u64 i = 0; i < FLAGS_num_load; ++i) {
      const auto request = trace->GetLoadRequest(i);
      const auto result = index.Insert(request.key, request.value);
      successful += result == Result::SUCCESS;
      failed += result != Result::SUCCESS;
    }
    Require(failed == 0 && successful == FLAGS_num_load, "Bulk loading did not insert all records");
    const auto load_statistics = index.Stat();
    Require(index.Snapshot().size == FLAGS_num_load, "Load cardinality mismatch");
    fmt::print("Figure13Load operations={} successful_operations={} failed_operations=0 size={}\n",
               FLAGS_num_load, successful, index.Snapshot().size);
    fmt::print("Figure13NativeStatistics round=load {}", load_statistics);
    Memory(index, "loaded");
    std::fflush(stdout);
    for (u64 round = 0; round < FLAGS_num_rounds; ++round) {
      if (synthetic::IsProfileCode(FLAGS_workload)) trace->PrepareRound(round);
      const auto result = RunRound(index, *trace);
      const auto statistics = index.Stat();
      const auto diagnostics = index.Snapshot();
      Require(diagnostics.size == FLAGS_num_load, "Run cardinality mismatch");
      if (FIGURE13_VARIANT < 3)
        Require(diagnostics.samples == 0 && diagnostics.reorders == 0 && diagnostics.promotions == 0 &&
                diagnostics.demotions == 0 && diagnostics.decay_events == 0,
                "Maintenance-disabled variant performed heat tracking or migration");
      fmt::print("Figure13Round round={} operations={} successful_operations={} failed_operations={} "
                 "elapsed_ns={} throughput_Mops={:.15f} p50_latency_ns={} p99_latency_ns={} "
                 "latency_samples={} search_operations={} update_operations={} size={}\n",
                 round, FLAGS_num_run, result.successful, result.failed, result.elapsed_ns,
                 double(result.successful) * 1000 / result.elapsed_ns,
                 result.p50, result.p99, result.samples, result.reads, result.updates, diagnostics.size);
      fmt::print("Figure13NativeStatistics round={} {}", round, statistics);
      std::fflush(stdout);
    }
    index.Stop();
    Memory(index, "complete");
    Require(index.Snapshot().size == FLAGS_num_load, "Final cardinality mismatch");
    fmt::print("Figure13Complete variant_id={} workload={} rounds=10 operations={} "
               "successful_operations={} failed_operations=0 size={} verified_rounds=10\n",
               FIGURE13_VARIANT_ID, FLAGS_workload, FLAGS_num_run * 10,
               FLAGS_num_run * 10, index.Snapshot().size);
    std::fflush(stdout);
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "Figure13Error {}\n", error.what());
    return 1;
  }
}
