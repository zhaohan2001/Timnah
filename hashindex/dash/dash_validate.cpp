#include "dash.h"
#include "benchmark/ycsb/modules/trace.hpp"

#include <climits>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

DEFINE_uint64(dash_validate_records, 100000000,
              "Number of real YCSB load-trace records to insert and exhaustively verify");
DEFINE_uint32(dash_validate_threads, 30, "Number of validator worker threads");
DEFINE_uint64(dash_validate_misses, 1000000,
              "Number of disjoint negative-domain keys to verify");

namespace {
bool BindCpuNodeZero() {
#ifdef __linux__
  return numa_available() >= 0 && numa_run_on_node(0) == 0;
#else
  return false;
#endif
}

int Validate() {
  const u64 records = FLAGS_dash_validate_records;
  const u64 misses = FLAGS_dash_validate_misses;
  const u32 threads = FLAGS_dash_validate_threads;
  if (!records || !threads || threads > static_cast<u32>(INT_MAX))
    throw std::invalid_argument("records and threads must be positive; threads must fit int");
  if (records > (std::numeric_limits<u64>::max() >> 3) ||
      misses > std::numeric_limits<u64>::max() - (records << 3))
    throw std::invalid_argument("negative-domain key ID arithmetic would overflow");
  if (!BindCpuNodeZero())
    throw std::runtime_error("cannot bind validator to CPU NUMA node 0");

  omp_set_dynamic(0);
  omp_set_num_threads(static_cast<int>(threads));
  std::srand(FLAGS_rndSeed);
  std::cout << "dash_validate not_performance_benchmark=1 records=" << records
            << " threads=" << threads << " misses=" << misses
            << " cpu_node=0 shuffle=" << FLAGS_shuffle
            << " seed=" << FLAGS_rndSeed << std::endl;

  // Use precisely the repository's load generator, including shuffled insertion
  // order and independently generated values. A zero-sized RUN trace avoids
  // allocating another 100 million requests for this correctness-only program.
  Trace trace(records, 0, 0.99, {1, 0, 0, 0, 0});
  DashNameSpace::Dash table;
  if (table.Create({}) != Result::SUCCESS)
    throw std::runtime_error("Dash Create failed");

  u64 bind_failures = 0, insert_fail = 0, operation_exceptions = 0;
#pragma omp parallel reduction(+ : bind_failures, insert_fail, operation_exceptions)
  {
    if (!BindCpuNodeZero()) ++bind_failures;
#pragma omp for schedule(dynamic, 10000)
    for (u64 i = 0; i < records; ++i) {
      const auto& request = trace.GetLoadRequest(i);
      try {
        if (table.Insert(request.key, request.value) != Result::SUCCESS) ++insert_fail;
      } catch (...) {
        ++insert_fail;
        ++operation_exceptions;
      }
    }
  }
  std::cout << "dash_validate insertion_complete records=" << records
            << " insertfail=" << insert_fail << std::endl;

  u64 missing = 0, bad_value = 0, expected_checksum = 0, checksum = 0;
#pragma omp parallel reduction(+ : bind_failures, missing, bad_value, expected_checksum, checksum, operation_exceptions)
  {
    if (!BindCpuNodeZero()) ++bind_failures;
#pragma omp for schedule(dynamic, 10000)
    for (u64 i = 0; i < records; ++i) {
      const auto& request = trace.GetLoadRequest(i);
      // A deterministic non-value initial state makes a failed/partial write
      // observable; the checksum also consumes the actual returned value.
      u64 value = ~request.value;
      expected_checksum += request.value ^ HashFunction(request.key);
      try {
        const auto result = table.Search(request.key, value);
        if (result != Result::SUCCESS) ++missing;
        else if (value != request.value) ++bad_value;
      } catch (...) {
        ++missing;
        ++operation_exceptions;
      }
      checksum += value ^ HashFunction(request.key);
    }
  }

  u64 miss_false_positive = 0, miss_output_changed = 0, miss_checksum = 0;
  const u64 negative_base = records << 3;
  constexpr u64 sentinel = 0xd6e8feb86659fd93ull;
#pragma omp parallel reduction(+ : bind_failures, miss_false_positive, miss_output_changed, miss_checksum, operation_exceptions)
  {
    if (!BindCpuNodeZero()) ++bind_failures;
#pragma omp for schedule(dynamic, 10000)
    for (u64 i = 0; i < misses; ++i) {
      // KeyGen::KeyFromID(id, true, false) uses HashFunction(id + (N<<3)).
      // The loaded IDs are [1,N], so these IDs are disjoint without guessing
      // absent numeric keys in the hash output domain.
      const u64 key = HashFunction(i + 1 + negative_base);
      u64 value = sentinel;
      try {
        if (table.Search(key, value) != Result::FAILED) ++miss_false_positive;
      } catch (...) {
        ++miss_false_positive;
        ++operation_exceptions;
      }
      if (value != sentinel) ++miss_output_changed;
      miss_checksum += key ^ value;
    }
  }

  const auto layout = table.Stat();
  const auto size_position = layout.find("size=");
  if (size_position == std::string::npos)
    throw std::runtime_error("Dash statistics do not report size");
  const u64 size = std::stoull(layout.substr(size_position + 5));
  const bool passed = !bind_failures && !insert_fail && !missing && !bad_value &&
      !miss_false_positive && !miss_output_changed && !operation_exceptions &&
      size == records && checksum == expected_checksum;
  std::cout << layout << "dash_validate records=" << records
            << " threads=" << threads << " misses=" << misses
            << " insertfail=" << insert_fail << " missing=" << missing
            << " badvalue=" << bad_value
            << " missfalsepositive=" << miss_false_positive
            << " missoutputchanged=" << miss_output_changed
            << " operation_exceptions=" << operation_exceptions
            << " bindfail=" << bind_failures << " size=" << size
            << " checksum=" << checksum
            << " expected_checksum=" << expected_checksum
            << " miss_checksum=" << miss_checksum
            << " passed=" << passed << std::endl;
  table.Stop();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
}  // namespace

int main(int argc, char** argv) {
  gflags::SetUsageMessage("Exhaustive Dash key/value and negative-query validator; not a throughput benchmark");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  try {
    return Validate();
  } catch (const std::exception& error) {
    std::cerr << "dash_validate fatal=" << error.what() << std::endl;
    return EXIT_FAILURE;
  } catch (...) {
    std::cerr << "dash_validate fatal=unknown_exception" << std::endl;
    return EXIT_FAILURE;
  }
}
