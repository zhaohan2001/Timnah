/// Workload abstraction

#pragma once
#include "common.hpp"
#include "parameter.hpp"
#include "request.hpp"
#include "benchmark/synthetic/dynamic_trace.hpp"

// ----------------------------------------------------------------------------
/// Workload abstraction over generated requests or a YCSB trace
/// - Provide load and run access to operations in the trace

class Trace {
 public:
  enum class TraceType { LOAD = 0, RUN = 1 };

  // ~~~~Construction~~~~

  Trace(u64 num_load, u64 num_run, double skewness, RequestTypeRatio ratio) {
    RequestGen req_gen{num_load, skewness, ratio};

    // Allocate empty LOAD and RUN traces
    traces[static_cast<int>(TraceType::LOAD)].resize(num_load);
    traces[static_cast<int>(TraceType::RUN)].resize(num_run);

    // Generate the LOAD trace in parallel using key IDs 1 through N
#pragma omp parallel for schedule(dynamic, 10000)
    for (u64 i = 0; i < num_load; i++) {
      traces[static_cast<int>(TraceType::LOAD)].at(i) =
          req_gen.GenLoadRequest(i + 1);
    }

    // Apply the hotspot shift
    if (FLAGS_shuffle) ShuffleLoadTrace();
    if (FLAGS_shift) req_gen.ShiftHotness();

#pragma omp parallel for schedule(dynamic, 10000)
    // Generate the RUN trace in parallel
    for (u64 i = 0; i < num_run; i++) {
      traces[static_cast<int>(TraceType::RUN)].at(i) = req_gen.GenRunRequest();
    }

    LOG("Trace initialization completed: {} GB allocated\n",
        (traces[0].size() + traces[1].size()) * sizeof(Request) / 1024.0 /
            1024.0 / 1024.0);
  }

  // Preload observed keys and disjoint background keys through the same mapping.
  Trace(synthetic::Profile profile, u64 num_load, u64 num_run, u64 seed)
      : profile_(make_unique<synthetic::Profile>(std::move(profile))),
        real_num_load_(num_load), real_num_run_(num_run), real_seed_(seed) {
    traces[static_cast<int>(TraceType::LOAD)].resize(num_load);
#pragma omp parallel for schedule(dynamic, 10000)
    for (u64 i = 0; i < num_load; ++i) {
      traces[static_cast<int>(TraceType::LOAD)][i] =
          {HashFunction(i + 1), HashFunction((i + 1) ^ seed), RequestType::Insert};
    }
    if (FLAGS_shuffle) {
      mt19937_64 random{seed};
      shuffle(traces[0].begin(), traces[0].end(), random);
    }
    LOG("Real preload trace: records={} key_bytes=8 value_bytes=8\n", num_load);
  }

  auto PrepareRound(u64 round) -> void {
    if (!profile_) throw std::logic_error("PrepareRound requires an observed-trace profile");
    StopWatch watch;
    synthetic::AuditStats audit;
    traces[static_cast<int>(TraceType::RUN)] = synthetic::GenerateRound(
        *profile_, real_num_load_, real_num_run_, round, real_seed_,
        FLAGS_real_epoch_ops, FLAGS_real_drift_stride, &audit);
    LOG("Real trace round={} reads={} updates={} first_cycle={} last_cycle={} key_id_min={} key_id_max={} generation_ns={} generation_in_throughput=false\n",
        round, audit.reads, audit.updates, audit.first_epoch, audit.last_epoch,
        audit.min_key_id, audit.max_key_id, watch.Elapsed());
  }

  // ~~~~Requests~~~~

  auto GetLoadRequest(u64 index) const -> const Request& {
    return traces[static_cast<int>(TraceType::LOAD)][index];
  }

  auto GetRunRequest(u64 index) const -> const Request& {
    return traces[static_cast<int>(TraceType::RUN)][index];
  }

  auto GetVerifyRequest(u64 index) const -> const Request {
    Request op = traces[static_cast<int>(TraceType::LOAD)][index];
    op.type = RequestType::Read;
    return op;
  }

  // ~~~~Debugging~~~~

  auto VerifyDistribution() const -> vector<pair<u64, u64>> {
    unordered_map<u64, u64> counter;
    vector<pair<u64, u64>> ret;
    for (const auto& req : traces[static_cast<int>(TraceType::RUN)]) {
      ++counter[req.key];
    }
    for (const auto& [k, c] : counter) {
      ret.push_back({k, c});
    }
    sort(ret.begin(), ret.end(),
         [](const auto& a, const auto& b) { return a.second > b.second; });
    return ret;
  }

 private:
  // ~~~~Helpers~~~~
  /// Shuffle hotspot keys
  auto ShuffleLoadTrace() -> void {
    mt19937_64 g{static_cast<u64>(rand())};
    shuffle(traces[static_cast<int>(TraceType::LOAD)].begin(),
            traces[static_cast<int>(TraceType::LOAD)].end(), g);
  }

  // ~~~~Data~~~~
  array<vector<Request>, 2> traces;
  array<atomic<u64>, 2> breakPoints;
  constexpr static u64 batchSize{10'000};
  unique_ptr<synthetic::Profile> profile_;
  u64 real_num_load_{}, real_num_run_{}, real_seed_{};
};
