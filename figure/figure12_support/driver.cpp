// Figure 12 lifecycle measurement. Links existing production index libraries.
// No index implementation, production executable, or hot path is modified.
#include "common.hpp"
#include "parameter.hpp"
#include "trace.hpp"

namespace {
constexpr u64 kStagesPerRound = 10;
constexpr u64 kSearchRounds = 10;
using Clock = std::chrono::steady_clock;

enum class Phase { BulkLoading, Executing, MassDeletion };

const char* PhaseName(Phase phase) {
  switch (phase) {
    case Phase::BulkLoading: return "bulk_loading";
    case Phase::Executing: return "executing";
    case Phase::MassDeletion: return "mass_deletion";
  }
  std::abort();
}

void Require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

struct alignas(64) WorkerCounts {
  u64 successful{}, failed{};
};

struct StageRecord {
  u64 operations{}, successful{}, failed{}, elapsed_ns{};
};

// Same dispatch and Request-by-value convention as Benchmark::Serve().
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

u64 SizeFromStatistics(const std::string& statistics) {
  const auto found = statistics.find(" size=");
  Require(found != std::string::npos, "Native statistics do not contain size");
  const auto first = found + 6;
  const auto end = statistics.find_first_not_of("0123456789", first);
  Require(end != first, "Native statistics contain an invalid size");
  return std::stoull(statistics.substr(first, end - first));
}

void ReportLatency(const std::vector<std::vector<high_resolution_clock::time_point>>& samples,
                   u64 round) {
  if (!FLAGS_latency) return;
  std::vector<u64> latencies;
  size_t total_samples = 0;
  for (const auto& worker : samples) total_samples += worker.size() / 2;
  latencies.reserve(total_samples);
  for (const auto& worker : samples) {
    Require(worker.size() % 2 == 0, "A latency sample pair is incomplete");
    for (size_t i = 0; i < worker.size(); i += 2)
      latencies.push_back(duration_cast<nanoseconds>(worker[i + 1] - worker[i]).count());
  }
  Require(!latencies.empty(), "No latency samples were collected");
  const double average = std::accumulate(latencies.begin(), latencies.end(), 0.0) /
                         static_cast<double>(latencies.size());
  std::sort(latencies.begin(), latencies.end());
  fmt::print("Figure12Latency phase_round={} sampled_pairs={} average_ns={:.9f}",
             round, latencies.size(), average);
  for (double quantile : {0.5, 0.9, 0.99, 0.999, 0.9999, 0.99999}) {
    const size_t rank = std::max<size_t>(1, static_cast<size_t>(quantile * latencies.size()));
    fmt::print(" p{:.5f}_ns={}", quantile, latencies[rank - 1]);
  }
  fmt::print("\n");
}

std::array<StageRecord, kStagesPerRound> RunPhase(
    IndexInterface& index, const Trace& trace, Phase phase, u64 phase_round = 0) {
  const bool search = phase == Phase::Executing;
  const u64 total_operations = search ? FLAGS_num_run : FLAGS_num_load;
  const u64 stage_operations = total_operations / kStagesPerRound;
  const u32 threads = search ? FLAGS_num_run_threads : FLAGS_num_load_threads;
  std::vector<WorkerCounts> counts(threads);
  std::array<StageRecord, kStagesPerRound> records{};
  std::vector<std::vector<high_resolution_clock::time_point>> latency_samples(threads);
  Clock::time_point started;

#pragma omp parallel num_threads(threads) shared(started, records, counts, latency_samples)
  {
    const unsigned worker = omp_get_thread_num();
    std::vector<high_resolution_clock::time_point> local_latency;
    // Native ycsb allocates the same per-worker buffer once per complete round,
    // then resets its size. Keep its per-read sampling and clock/vector costs.
    if (search) {
      local_latency.resize(FLAGS_num_run * FLAGS_latency_sample_rate * 2.5);
      local_latency.resize(0);
    }
    Dice<256> dice(FLAGS_latency_sample_rate);

    for (u64 stage = 0; stage < kStagesPerRound; ++stage) {
      WorkerCounts local;
      const u64 begin = stage * stage_operations;
      const u64 end = begin + stage_operations;
#pragma omp barrier
#pragma omp single
      { started = Clock::now(); }
      // This single's implicit start barrier and the omp-for completion barrier
      // are part of elapsed wall time. Counting, logging, native Stat, trace
      // generation and post-delete checks are outside all Stage timers.
#pragma omp for schedule(dynamic, 100000)
      for (u64 position = begin; position < end; ++position) {
        Result result;
        if (search) {
          const bool sampled = FLAGS_latency ? dice.Roll() : false;
          if (sampled) local_latency.push_back(high_resolution_clock::now());
          result = Serve(index, trace.GetRunRequest(position));
          if (sampled) local_latency.push_back(high_resolution_clock::now());
        } else {
          const auto request = trace.GetLoadRequest(position);
          result = phase == Phase::BulkLoading
                       ? index.Insert(request.key, request.value)
                       : index.Remove(request.key);
        }
        if (result == Result::SUCCESS) ++local.successful;
        else ++local.failed;
      }
#pragma omp single
      {
        records[stage].elapsed_ns =
            duration_cast<nanoseconds>(Clock::now() - started).count();
      }
      counts[worker] = local;
#pragma omp barrier
#pragma omp single
      {
        auto& record = records[stage];
        record.operations = stage_operations;
        for (const auto& item : counts) {
          record.successful += item.successful;
          record.failed += item.failed;
        }
      }
    }
    if (search) std::swap(latency_samples[worker], local_latency);
  }

  // Fail before accepting any phase containing unsuccessful operations.
  for (const auto& record : records) {
    Require(record.elapsed_ns > 0, "A Stage has a nonpositive elapsed time");
    Require(record.successful + record.failed == stage_operations,
            "A Stage operation count is inconsistent");
    Require(record.failed == 0, "A Stage contains unsuccessful index operations");
  }
  // This reporting is deliberately deferred until all ten Stage timers finish.
  // Workers never wait for output between measured chunks.
  if (search) ReportLatency(latency_samples, phase_round);
  return records;
}

void ReportPhase(IndexInterface& index,
                 const std::array<StageRecord, kStagesPerRound>& records,
                 Phase phase, u64 lifecycle_round, u64 phase_round,
                 u64 expected_size) {
  u64 operations = 0, successful = 0, failed = 0;
  for (u64 i = 0; i < kStagesPerRound; ++i) {
    const auto& record = records[i];
    const double seconds = static_cast<double>(record.elapsed_ns) / 1e9;
    const double throughput = static_cast<double>(record.successful) * 1e3 /
                              static_cast<double>(record.elapsed_ns);
    fmt::print("Figure12Stage stage={} phase={} lifecycle_round={} phase_round={} "
               "stage_in_round={} operations={} successful_operations={} "
               "failed_operations={} elapsed_ns={} elapsed_seconds={:.15f} "
               "throughput_Mops={:.15f}\n",
               lifecycle_round * kStagesPerRound + i + 1, PhaseName(phase),
               lifecycle_round, phase_round, i + 1, record.operations,
               record.successful, record.failed, record.elapsed_ns, seconds,
               throughput);
    operations += record.operations;
    successful += record.successful;
    failed += record.failed;
  }
  // Exactly one native Stat per real phase boundary: notably, Timnah::Stat
  // advances convergence and must never be invoked at sub-round boundaries.
  const auto statistics = index.Stat();
  const auto size = SizeFromStatistics(statistics);
  Require(size == expected_size, "Native cardinality differs from the expected lifecycle size");
  fmt::print("Figure12Phase phase={} lifecycle_round={} phase_round={} "
             "attempted_operations={} successful_operations={} failed_operations={} "
             "size={} expected_size={} cardinality_verified=true\n",
             PhaseName(phase), lifecycle_round, phase_round, operations,
             successful, failed, size, expected_size);
  fmt::print("Figure12NativeStatistics phase={} phase_round={} {}",
             PhaseName(phase), phase_round, statistics);
  if (statistics.empty() || statistics.back() != '\n') fmt::print("\n");
  std::fflush(stdout);
}

u64 VerifyAllDeleted(IndexInterface& index, const Trace& trace) {
  u64 remaining = 0;
#pragma omp parallel for num_threads(FLAGS_num_run_threads) schedule(dynamic, 100000) reduction(+:remaining)
  for (u64 i = 0; i < FLAGS_num_load; ++i) {
    u64 value;
    remaining += index.Search(trace.GetLoadRequest(i).key, value) == Result::SUCCESS;
  }
  return remaining;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Parameter::Init(argc, argv);
    Require(FLAGS_index == "Timnah" || FLAGS_index == "Dash" || FLAGS_index == "CHash",
            "Figure 12 supports only Timnah, Dash, and CHash");
    Require(FLAGS_workload == "c" && FLAGS_read_ratio == 1.0 &&
                FLAGS_update_ratio == 0.0 && FLAGS_insert_ratio == 0.0 &&
                FLAGS_remove_ratio == 0.0 && FLAGS_negative_ratio == 0.0,
            "Figure 12 requires YCSB C with only successful searches");
    Require(FLAGS_num_rounds == kSearchRounds && FLAGS_num_run >= 10000 &&
                FLAGS_num_load >= 10000 && FLAGS_num_run % kStagesPerRound == 0 &&
                FLAGS_num_load % kStagesPerRound == 0,
            "Figure 12 needs ten rounds and operation counts >= 10000 divisible by ten");
    Require(FLAGS_num_run_threads == 30 && FLAGS_num_load_threads == 30 &&
                FLAGS_num_threads == 30,
            "Figure 12 requires 30 foreground threads");
    Require(FLAGS_num_bg == (FLAGS_index == "Timnah" ? 1u : 0u),
            "Figure 12 needs one Timnah worker and zero workers for other indexes");
    Require(FLAGS_latency && FLAGS_latency_sample_rate == 0.01,
            "Figure 12 preserves ordinary latency sampling: true at rate 0.01");
    Require(FLAGS_bind_core && !FLAGS_verify && !FLAGS_shift && FLAGS_shuffle &&
                FLAGS_keySize == 8 && FLAGS_valueSize == 8,
            "Figure 12 requires CPU binding, shuffled load, 8-byte keys/values, no shift or native verification");
    Require(numa_run_on_node(0) == 0, "Cannot bind to CPU node 0");
    omp_set_dynamic(0);

    std::unique_ptr<IndexInterface> index{GetIndexPtr()};
    Require(static_cast<bool>(index), "The production index factory returned null");
    Require(index->Create(Parameter::GetIndexParam()) == Result::SUCCESS,
            "Index creation failed");
    fmt::print("Figure12Config index={} workload=c num_threads=30 num_load_threads=30 "
               "num_run_threads=30 num_bg={} num_rounds=10 lifecycle_rounds=12 "
               "stages_per_round=10 num_load={} num_run={} key_bytes=8 value_bytes=8 "
               "latency=true latency_sample_rate=0.01 cpu_node=0 "
               "metric=completed_operations/elapsed_seconds "
               "index_implementation=unchanged_production_shared_libraries "
               "timing=operation_chunk_wall_time_including_worker_barriers "
               "excluded=trace_generation_statistics_logging_latency_analysis_stop_validation_destructor\n",
               FLAGS_index, FLAGS_num_bg, FLAGS_num_load, FLAGS_num_run);
    const RequestTypeRatio ratios{FLAGS_read_ratio, FLAGS_update_ratio,
                                  FLAGS_insert_ratio, FLAGS_remove_ratio,
                                  FLAGS_negative_ratio};
    Trace trace(FLAGS_num_load, FLAGS_num_run, FLAGS_skewness, ratios);

    auto records = RunPhase(*index, trace, Phase::BulkLoading);
    ReportPhase(*index, records, Phase::BulkLoading, 0, 0, FLAGS_num_load);
    for (u64 round = 0; round < kSearchRounds; ++round) {
      records = RunPhase(*index, trace, Phase::Executing, round);
      ReportPhase(*index, records, Phase::Executing, round + 1, round, FLAGS_num_load);
    }
    records = RunPhase(*index, trace, Phase::MassDeletion);
    ReportPhase(*index, records, Phase::MassDeletion, 11, 0, 0);
    // Stop and complete-key absence verification do not contribute throughput.
    index->Stop();
    const auto remaining = VerifyAllDeleted(*index, trace);
    Require(remaining == 0, "Some original keys remain searchable after deletion");
    fmt::print("Figure12Verification checked_keys={} remaining_keys=0 "
               "expected_remaining_keys=0 outside_timing=true\n", FLAGS_num_load);
    fmt::print("Figure12Complete stages=120 lifecycle_rounds=12 search_rounds=10 "
               "loaded_keys={} searched_keys={} deleted_keys={} remaining_keys=0 "
               "final_size=0 verified_absent={} metric=completed_operations/elapsed_seconds "
               "operations={} successful_operations={} failed_operations=0\n",
               FLAGS_num_load, FLAGS_num_run * kSearchRounds, FLAGS_num_load, FLAGS_num_load,
               FLAGS_num_load * 2 + FLAGS_num_run * kSearchRounds,
               FLAGS_num_load * 2 + FLAGS_num_run * kSearchRounds);
    std::fflush(stdout);
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "Figure12Error {}\n", error.what());
    return 1;
  }
}
