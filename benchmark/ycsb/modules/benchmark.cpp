/// Benchmark implementation

#include "benchmark.hpp"

/// Implementation
// ----------------------------------------------------------------------------

/// Constructor
Benchmark::Benchmark(IndexInterface* _indexing) : indexing_(_indexing) {
  LOG("Benchmark index = {}\n", FLAGS_index);
  LOG("Generating trace... \n");
  StopWatch watch{};
  if (synthetic::IsProfileCode(FLAGS_workload)) {
    trace_ = make_unique<Trace>(synthetic::GetProfile(FLAGS_workload),
                               FLAGS_num_load, FLAGS_num_run, FLAGS_rndSeed);
  } else {
  RequestTypeRatio ratio{FLAGS_read_ratio, FLAGS_update_ratio,
                         FLAGS_insert_ratio, FLAGS_remove_ratio,
                         FLAGS_negative_ratio};
  trace_ =
      make_unique<Trace>(FLAGS_num_load, FLAGS_num_run, FLAGS_skewness, ratio);
  }
  LOG("Trace initialization time = {}\n", ReadableTime(watch.Elapsed()));
}

/// Destructor
Benchmark::~Benchmark() {}

auto Benchmark::PrepareRound(u64 round) -> void {
  trace_->PrepareRound(round);
}

/// Load data
/// Implementation: time -> load data -> collect statistics -> verify data -> report verification
auto Benchmark::EvalLoadPhase() -> void {
  LOG("Loading...\n");
  StopWatch watch{};

#pragma omp parallel for num_threads(FLAGS_num_load_threads) \
    schedule(dynamic, 100'000)
  for (u64 i = 0; i < FLAGS_num_load; i++) {
    auto request = trace_->GetLoadRequest(i);
    indexing_->Insert(request.key, request.value);
  }

  u64 elapsed = watch.Elapsed();
  double throughput{(FLAGS_num_load / 1e6) / (elapsed / 1e9)};
  LOG("Load time: {}\n", ReadableTime(elapsed));
  LOG("Load throughput: {:.3f} Mops/s\n", throughput);
  LOG("Load statistics: {}\n", indexing_->Stat());

  if (FLAGS_verify) {  /// Verify the loaded data
    LOG("Verifying loaded data...\n");
    StopWatch watch{};
#pragma omp parallel for num_threads(FLAGS_num_load_threads) \
    schedule(dynamic, 100'000)
    for (u64 i = 0; i < FLAGS_num_load; i++) {
      auto request = trace_->GetVerifyRequest(i);
      u64 outputValue;
      auto result = indexing_->Search(request.key, outputValue);
      if (result != Result::SUCCESS) {
        LOG("Load verification failed: key at position {} = {}\n", i, request.key);
        AbortHere();
      }
    }
    LOG("Verification time: {}\n", ReadableTime(watch.Elapsed()));
    LOG("Verification passed\n");
  }
}

/// Run the benchmark
auto Benchmark::EvalRunPhase() -> void {
  LOG("Running...\n");
  StopWatch watch;
  vector<LatencyStats> latStats(FLAGS_num_run_threads);

#pragma omp parallel num_threads(FLAGS_num_run_threads)
  {
    LatencyStats latStat;  /// Initialize latency statistics
    latStat.times.resize(FLAGS_num_run * FLAGS_latency_sample_rate * 2.5);
    latStat.times.resize(0);
    Dice<256> dice(FLAGS_latency_sample_rate);
#pragma omp barrier
    /// Run the benchmark
#pragma omp for schedule(dynamic, 100'000)
    for (size_t i = 0; i < FLAGS_num_run; i++) {
      bool isSample = FLAGS_latency ? dice.Roll() : false;
      if (isSample) {
        latStat.times.push_back(high_resolution_clock::now());
      }
      Serve(trace_->GetRunRequest(i));
      if (isSample) {
        latStat.times.push_back(high_resolution_clock::now());
      }
    }
    swap(latStats[omp_get_thread_num()], latStat);
  }

  /// Report statistics
  u64 elapsed = watch.Elapsed();
  double throughput{(FLAGS_num_run / 1e6) / (elapsed / 1e9)};
  LOG("Run time: {}\n", ReadableTime(elapsed));
  LOG("Run throughput: {:.3f} Mops/s\n", throughput);

  if (FLAGS_latency) {  /// Summarize latency statistics
    vector<u64> latencies;
    for (auto& latStat : latStats) {
      for (size_t i = 0; i < latStat.times.size(); i += 2) {
        latencies.push_back(
            duration_cast<nanoseconds>(latStat.times[i + 1] - latStat.times[i])
                .count());  /// Calculate latency
      }
    }
    LOG("Mean latency: {} ns\n",
        accumulate(latencies.begin(), latencies.end(), 0) / latencies.size());
    sort(latencies.begin(), latencies.end());
    /// Report latency at the 50%, 90%, 99%, 99.9%, 99.99%, and 99.999% percentiles
    for (double p : {0.5, 0.9, 0.99, 0.999, 0.9999, 0.99999}) {
      LOG("Tail latency {:.5}%: {} ns\n", p * 100,
          latencies[p * latencies.size() - 1]);
    }
  }
  LOG("Run statistics: {}\n", indexing_->Stat());
}

/// Execute one operation
auto Benchmark::Serve(Request request) -> Result {
  u64 outputValue;
  switch (request.type) {
    case RequestType::NegativeRead:
    case RequestType::Read: {
      return indexing_->Search(request.key, outputValue);
    } break;
    case RequestType::Update: {
      return indexing_->Update(request.key, request.value);
    } break;
    case RequestType::Insert: {
      return indexing_->Insert(request.key, request.value);
    } break;
    case RequestType::Remove: {
      return indexing_->Remove(request.key);
    } break;
    default:
      abort();
  }
}
