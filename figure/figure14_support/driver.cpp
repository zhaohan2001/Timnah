// Whole-round measurements over byte-identical production Timnah sources.
// Only the existing fast-tier initial-depth option changes across capacities.
#include "common.hpp"
#include "parameter.hpp"
#include "trace.hpp"
#include "timnah.h"
#include "hashindex/chash/chash.h"
#include "percentile.hpp"

DEFINE_string(index, "Timnah", "Figure 14 isolated full Timnah implementation");
DEFINE_string(figure14_variant, "Timnah-A", "Figure 14 method: Timnah-A, Timnah-B, Timnah-C, or CHash");

namespace {
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
std::string Option(const char* name) {
  std::string value;
  Require(gflags::GetCommandLineOption(name, &value), "A required Timnah option is missing");
  return value;
}
u64 NativeField(const std::string& statistics, const char* field) {
  const std::string needle = std::string(field) + "=";
  const auto position = statistics.find(needle);
  Require(position != std::string::npos &&
          (position == 0 || statistics[position - 1] == ' ' || statistics[position - 1] == '\n'),
          "A required native statistic is missing");
  return std::stoull(statistics.substr(position + needle.size()));
}
struct alignas(64) Counts {
  u64 successful{}, failed{}, reads{}, updates{}, inserts{}, duplicate_rejections{}, unexpected{};
};
struct RoundResult {
  u64 elapsed_ns{}, successful{}, failed{}, reads{}, updates{}, inserts{};
  u64 duplicate_rejections{}, samples{}, p50{}, p99{};
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
RoundResult RunRound(IndexInterface& index, const Trace& trace, u64 round) {
  const bool replayed_inserts = FLAGS_workload == "d" && round > 0;
  // Include native-style allocation, OpenMP setup/barrier, dispatch and sampled
  // clocks. Quantile analysis and native Stat run after this timed region.
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
      local.inserts += request.type == RequestType::Insert;
      const bool expected_duplicate = replayed_inserts && request.type == RequestType::Insert;
      local.duplicate_rejections += expected_duplicate && result == Result::FAILED;
      local.unexpected += expected_duplicate ? result != Result::FAILED : result != Result::SUCCESS;
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
  u64 unexpected = 0;
  for (const auto& item : counters) {
    result.successful += item.successful; result.failed += item.failed;
    result.reads += item.reads; result.updates += item.updates; result.inserts += item.inserts;
    result.duplicate_rejections += item.duplicate_rejections; unexpected += item.unexpected;
  }
  Require(!latencies.empty(), "No latency samples were collected");
  std::sort(latencies.begin(), latencies.end());
  result.samples = latencies.size();
  result.p50 = latencies[figure14::PercentileIndex(0.5, latencies.size())];
  result.p99 = latencies[figure14::PercentileIndex(0.99, latencies.size())];
  Require(unexpected == 0 && result.failed == result.duplicate_rejections,
          "An index operation returned an unexpected result");
  Require(result.successful + result.failed == FLAGS_num_run &&
          result.reads + result.updates + result.inserts == FLAGS_num_run,
          "The round operation counts are inconsistent");
  Require(result.elapsed_ns > 0, "The round elapsed time is nonpositive");
  return result;
}
void Memory(const TimnahNameSpace::Timnah& index, const char* phase,
            u64 fast_bucket_capacity, u64 fast_depth) {
  const auto d = index.Snapshot();
  Require(d.fast_segments == (u64{1} << fast_depth) &&
          d.fast_bucket_bytes == fast_bucket_capacity &&
          d.fast_counter_bytes == fast_bucket_capacity / 4,
          "The actual fast-tier allocation does not match the selected capacity");
  // The unmodified fixed-tier constructor requests one contiguous bucket and
  // counter allocation. Arena's existing minimum block is 64 MiB. The two
  // directories have independent metadata arenas with a 2 MiB block minimum.
  const u64 counter_reserved = std::max(fast_bucket_capacity / 4, u64{64} << 20);
  fmt::print("Figure14Memory phase={} fast_bucket_bytes={} fast_counter_bytes={} "
             "slow_bucket_bytes={} slow_counter_bytes={} fast_segments={} slow_segments={} "
             "fast_node=0 slow_node=2 metadata_node=0 fast_depth={} slow_depth=14 "
             "fast_bucket_capacity_bytes={} fast_counter_capacity_bytes={} "
             "fast_bucket_reserved_bytes_inferred={} fast_counter_reserved_bytes_inferred={} "
             "fast_metadata_arena_minimum_bytes=2097152 slow_metadata_arena_minimum_bytes=2097152 "
             "budget_scope=fast_bucket_bytes_only counters_and_metadata_included=false "
             "reserved_scope=inferred_from_unchanged_fixed_tier_contiguous_allocation "
             "placement_policy=fixed_fast_node0_growable_slow_node2_metadata_node0\n",
             phase, d.fast_bucket_bytes, d.fast_counter_bytes,
             d.slow_bucket_bytes, d.slow_counter_bytes, d.fast_segments, d.slow_segments,
             fast_depth, fast_bucket_capacity, fast_bucket_capacity / 4,
             fast_bucket_capacity, counter_reserved);
}
void Memory(IndexInterface& index, const char* phase) {
  const auto statistics = index.Stat();
  Require(NativeField(statistics, "fast_reserved") == (u64{256} << 20) &&
          NativeField(statistics, "fast_used") <= (u64{256} << 20),
          "CHash exceeded its existing metadata-plus-KV fast-pool budget");
  fmt::print("Figure14Memory phase={} fast_node=0 slow_node=2 fast_pool_capacity_bytes=268435456 "
             "budget_scope=existing_metadata_plus_KV_pool counters_and_metadata_included=true "
             "fast_reserved={} slow_reserved={} fast_used={} slow_used={} "
             "fast_metadata={} slow_metadata={} fast_kv={} slow_kv={} "
             "split_fast_kv={} split_slow_kv={} slow_growth_count={}\n",
             phase, NativeField(statistics, "fast_reserved"), NativeField(statistics, "slow_reserved"),
             NativeField(statistics, "fast_used"), NativeField(statistics, "slow_used"),
             NativeField(statistics, "fast_metadata"), NativeField(statistics, "slow_metadata"),
             NativeField(statistics, "fast_kv"), NativeField(statistics, "slow_kv"),
             NativeField(statistics, "split_fast_kv"), NativeField(statistics, "split_slow_kv"),
             NativeField(statistics, "slow_growth_count"));
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Parameter::Init(argc, argv);
    const bool is_chash = FLAGS_figure14_variant == "CHash";
    Require(FLAGS_index == (is_chash ? "CHash" : "Timnah"), "Incorrect index for the selected Figure 14 variant");
    Require(FLAGS_workload == "a" || FLAGS_workload == "b" || FLAGS_workload == "c" || FLAGS_workload == "d",
            "Figure 14 supports only workloads a, b, c, d");
    const u64 fast_depth = FLAGS_figure14_variant == "Timnah-A" ? 12 :
                           FLAGS_figure14_variant == "Timnah-B" ? 11 :
                           FLAGS_figure14_variant == "Timnah-C" ? 10 : 0;
    Require(fast_depth != 0 || is_chash, "Unknown Figure 14 capacity variant");
    const u64 fast_bucket_capacity = is_chash ? 0 : (u64{1} << fast_depth) * 256 * 256;
    Require(is_chash || (Option("timnah_fast_depth") == std::to_string(fast_depth) &&
            Option("timnah_slow_depth") == "14" && Option("timnah_fast_node") == "0" &&
            Option("timnah_slow_node") == "2"), "Incorrect Figure 14 depth or NUMA placement");
    Require(!is_chash || (Option("chash_initial_depth") == "16" &&
            Option("chash_fast_mib") == "256" && Option("chash_slow_initial_mib") == "1024" &&
            Option("chash_slow_growth_mib") == "64" && Option("chash_dram_node") == "0" &&
            Option("chash_slow_node") == "2"), "Figure 14 requires unchanged CHash options");
    Require(FLAGS_num_threads == 30 && FLAGS_num_load_threads == 30 && FLAGS_num_run_threads == 30,
            "Figure 14 requires 30 foreground threads");
    Require(FLAGS_num_rounds == 10 && FLAGS_num_load >= 10000 && FLAGS_num_run >= 10000 &&
            FLAGS_num_bg == (is_chash ? 0u : 1u),
            "Figure 14 requires ten rounds, the selected background count and at least 10000 records/operations");
    Require(FLAGS_latency && FLAGS_latency_sample_rate == 0.01 && FLAGS_bind_core && !FLAGS_verify &&
            !FLAGS_shift && FLAGS_shuffle && FLAGS_keySize == 8 && FLAGS_valueSize == 8,
            "Figure 14 requires ordinary sampled latency, CPU binding and 8-byte shuffled records");
    Require(is_chash || (std::stod(Option("timnah_sample_rate")) == 0.1 &&
            std::stod(Option("timnah_convergence_threshold")) == 0.002 &&
            Option("timnah_decay_ms") == "1000" &&
            std::stod(Option("timnah_decay_factor")) == 0.5 &&
            std::stod(Option("timnah_intra_multiplier")) == 2.0 &&
            std::stod(Option("timnah_cross_multiplier")) == 2.0 &&
            Option("timnah_queue_capacity") == "1024" && Option("timnah_huge_pages") == "true"),
            "Figure 14 requires unchanged default maintenance options");
    Require(numa_run_on_node(0) == 0, "Cannot bind execution to CPU node 0");
    omp_set_dynamic(0);
    omp_set_num_threads(30);
    std::unique_ptr<IndexInterface> owner;
    if (is_chash) owner = std::make_unique<CHashNameSpace::CHash>();
    else owner = std::make_unique<TimnahNameSpace::Timnah>();
    auto& index = *owner;
    auto* timnah = is_chash ? nullptr : static_cast<TimnahNameSpace::Timnah*>(owner.get());
    Require(index.Create(Parameter::GetIndexParam()) == Result::SUCCESS, "Index creation failed");
    fmt::print("Figure14Config variant={} index={} workload={} num_threads=30 num_load_threads=30 "
               "trace_generation_threads=30 "
               "num_run_threads=30 num_rounds=10 num_bg={} num_load={} num_run={} "
               "dram_bucket_mib={} fast_depth={} slow_depth=14 "
               "latency=true latency_sample_rate=0.01 cpu_node=0 fast_node=0 slow_node=2 "
               "index_source=unchanged_production_snapshot convergence_threshold=0.002 "
               "chash_fast_pool_mib={} chash_budget_scope=existing_metadata_plus_KV_pool "
               "metric=whole_round_attempted_operations/elapsed_seconds "
               "latency_unit=ns quantile_method=floor_probability_times_samples_minus_one "
               "timing=native_round_allocation_barrier_dispatch_sampling_and_operation_wall_time "
               "d_mode=ratio_only_replay_same_insert_keys "
               "excluded=trace_generation_latency_analysis_statistics_stop_destructor\n",
               FLAGS_figure14_variant, FLAGS_index, FLAGS_workload, FLAGS_num_bg, FLAGS_num_load, FLAGS_num_run,
               fast_bucket_capacity >> 20, fast_depth, is_chash ? 256 : 0);
    if (timnah) Memory(*timnah, "initial", fast_bucket_capacity, fast_depth);
    else Memory(index, "initial");
    const RequestTypeRatio ratios{FLAGS_read_ratio, FLAGS_update_ratio, FLAGS_insert_ratio,
                                  FLAGS_remove_ratio, FLAGS_negative_ratio};
    Trace trace(FLAGS_num_load, FLAGS_num_run, FLAGS_skewness, ratios);
    u64 trace_reads = 0, trace_updates = 0, trace_inserts = 0;
    for (u64 i = 0; i < FLAGS_num_run; ++i) {
      const auto request = trace.GetRunRequest(i);
      trace_reads += request.type == RequestType::Read;
      trace_updates += request.type == RequestType::Update;
      trace_inserts += request.type == RequestType::Insert;
    }
    Require(trace_reads + trace_updates + trace_inserts == FLAGS_num_run,
            "The trace contains an unsupported operation");
    fmt::print("Figure14Trace workload={} operations={} search_operations={} update_operations={} "
               "insert_operations={} replay_same_trace=true\n",
               FLAGS_workload, FLAGS_num_run, trace_reads, trace_updates, trace_inserts);
    u64 successful = 0, failed = 0;
#pragma omp parallel for num_threads(FLAGS_num_load_threads) schedule(dynamic, 100000) reduction(+:successful,failed)
    for (u64 i = 0; i < FLAGS_num_load; ++i) {
      const auto request = trace.GetLoadRequest(i);
      const auto result = index.Insert(request.key, request.value);
      successful += result == Result::SUCCESS;
      failed += result != Result::SUCCESS;
    }
    Require(failed == 0 && successful == FLAGS_num_load, "Bulk loading did not insert all records");
    const auto load_statistics = index.Stat();
    Require(NativeField(load_statistics, "size") == FLAGS_num_load, "Load cardinality mismatch");
    fmt::print("Figure14Load operations={} successful_operations={} failed_operations=0 size={}\n",
               FLAGS_num_load, successful, NativeField(load_statistics, "size"));
    fmt::print("Figure14NativeStatistics round=load {}", load_statistics);
    if (timnah) Memory(*timnah, "loaded", fast_bucket_capacity, fast_depth);
    else Memory(index, "loaded");
    std::fflush(stdout);
    u64 total_successful = 0, total_failed = 0;
    for (u64 round = 0; round < FLAGS_num_rounds; ++round) {
      const auto result = RunRound(index, trace, round);
      const auto statistics = index.Stat();
      const auto size = NativeField(statistics, "size");
      Require(size == FLAGS_num_load + trace_inserts, "Run cardinality mismatch");
      Require(result.reads == trace_reads && result.updates == trace_updates && result.inserts == trace_inserts,
              "The round operation counts differ from the fixed trace");
      Require(result.duplicate_rejections == (round > 0 ? trace_inserts : 0),
              "The repeated-insert rejection count is incorrect");
      total_successful += result.successful; total_failed += result.failed;
      fmt::print("Figure14Round round={} operations={} successful_operations={} failed_operations={} "
                 "expected_duplicate_insert_rejections={} unexpected_failures=0 "
                 "elapsed_ns={} throughput_Mops={:.15f} p50_latency_ns={} p99_latency_ns={} "
                 "latency_samples={} search_operations={} update_operations={} insert_operations={} size={}\n",
                 round, FLAGS_num_run, result.successful, result.failed, result.duplicate_rejections,
                 result.elapsed_ns, double(FLAGS_num_run) * 1000 / result.elapsed_ns,
                 result.p50, result.p99, result.samples, result.reads, result.updates, result.inserts, size);
      fmt::print("Figure14NativeStatistics round={} {}", round, statistics);
      std::fflush(stdout);
    }
    index.Stop();
    if (timnah) Memory(*timnah, "complete", fast_bucket_capacity, fast_depth);
    else Memory(index, "complete");
    const auto final_size = timnah ? timnah->Snapshot().size : NativeField(index.Stat(), "size");
    Require(final_size == FLAGS_num_load + trace_inserts, "Final cardinality mismatch");
    fmt::print("Figure14Complete variant={} workload={} rounds=10 operations={} "
               "successful_operations={} failed_operations={} expected_duplicate_insert_rejections={} "
               "unexpected_failures=0 size={} verified_rounds=10\n",
               FLAGS_figure14_variant, FLAGS_workload, FLAGS_num_run * 10,
               total_successful, total_failed, trace_inserts * 9, final_size);
    std::fflush(stdout);
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "Figure14Error {}\n", error.what());
    return 1;
  }
}
