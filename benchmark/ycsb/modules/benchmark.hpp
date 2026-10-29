/// Benchmark driver, based on pibench

#pragma once

#include "common.hpp"
#include "parameter.hpp"
#include "request.hpp"
#include "trace.hpp"

class Benchmark {
 public:
  Benchmark(IndexInterface* _hashing);
  ~Benchmark();
  /// Load warmup data
  auto EvalLoadPhase() -> void;
  /// Run the benchmark
  auto EvalRunPhase() -> void;
  // Generates the next dynamic trace outside the existing timed request loop.
  auto PrepareRound(u64 round) -> void;

 private:
  // ~~~~Helper functions~~~~
  /// Execute one operation
  auto Serve(Request request) -> Result;

  // ~~~~Data~~~~
  /// Hash index under test
  IndexInterface* indexing_{nullptr};
  /// Workload generator
  unique_ptr<Trace> trace_{nullptr};
  /// Statistics definitions
  struct LatencyStats {
    // u64 operationCount{0};
    vector<high_resolution_clock::time_point> times;
  };
};
