/// YCSB benchmark implementation.
#include "benchmark.hpp"
#include "common.hpp"
#include "parameter.hpp"

int main(int argc, char** argv) {
  // Parse parameters
  Parameter::Init(argc, argv);
  IndexInterface::ParamType param{Parameter::GetIndexParam()};

  // Bind CPU cores
  if (FLAGS_bind_core) numa_run_on_node(0);
  if (synthetic::IsProfileCode(FLAGS_workload))
    omp_set_num_threads(FLAGS_num_run_threads);

  // Create the index
  StopWatch watch;
  watch.Start();

  IndexInterface* index{GetIndexPtr()};
  index->Create(param);

  LOG("Hash index {} creation time: {}\n", FLAGS_index, ReadableTime(watch.Elapsed()));

  // Construct the benchmark
  Benchmark bench{index};

  // Load phase
  bench.EvalLoadPhase();

  // Run phase repeated N times
  for (u32 i = 0; i < FLAGS_num_rounds; i++) {
    if (synthetic::IsProfileCode(FLAGS_workload)) bench.PrepareRound(i);
    LOG("Round {}\n", i);
    bench.EvalRunPhase();
  }

  // Cleanup
  index->Stop();

  return 0;
}
