#include "parameter.hpp"
#include "trace.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace {
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// Stable digest of logical fields, without struct padding or host byte order.
void DigestField(u64& digest, u64 value) {
  for (unsigned byte = 0; byte < 8; ++byte) {
    digest ^= (value >> (8 * byte)) & 0xff;
    digest *= 1099511628211ULL;
  }
}

void DigestRequest(u64& digest, const Request& request) {
  DigestField(digest, request.key);
  DigestField(digest, request.value);
  DigestField(digest, static_cast<u64>(request.type));
}

struct MockIndex {
  std::unordered_map<u64, u64> records;

  bool Insert(const Request& request) {
    return records.emplace(request.key, request.value).second;
  }

  bool Search(const Request& request) const {
    return records.find(request.key) != records.end();
  }
};
}  // namespace

int main(int argc, char** argv) {
  try {
    Parameter::Init(argc, argv);
    Require(FLAGS_num_load >= 2 && FLAGS_num_load <= 1000000,
            "probe requires num_load in [2, 1000000]");
    Require(FLAGS_num_run >= 10000 && FLAGS_num_run <= 1000000,
            "probe requires num_run in [10000, 1000000]");
    Require(FLAGS_read_ratio == 0.95 && FLAGS_insert_ratio == 0.05 &&
                FLAGS_update_ratio == 0 && FLAGS_remove_ratio == 0 &&
                FLAGS_negative_ratio == 0,
            "probe expects ratio-only D, or identical custom weights");

    // Compare separate invocations: each process first touches the generator's
    // thread-local engines here, after the same srand and with one OMP thread.
    // Do not change production seeding or trace generation for this test.
    std::srand(1729);
    omp_set_dynamic(0);
    omp_set_num_threads(1);
    Trace trace(FLAGS_num_load, FLAGS_num_run, FLAGS_skewness,
                {FLAGS_read_ratio, FLAGS_update_ratio, FLAGS_insert_ratio,
                 FLAGS_remove_ratio, FLAGS_negative_ratio});

    MockIndex index;
    std::unordered_set<u64> preload_keys;
    std::unordered_set<u64> insert_keys;
    u64 digest = 14695981039346656037ULL;
    for (u64 i = 0; i < FLAGS_num_load; ++i) {
      const Request& request = trace.GetLoadRequest(i);
      Require(request.type == RequestType::Insert, "load must be Insert");
      Require(preload_keys.insert(request.key).second,
              "load keys must be unique");
      Require(index.Insert(request), "mock load must succeed");
      DigestRequest(digest, request);
    }

    u64 reads = 0;
    u64 inserts = 0;
    for (u64 i = 0; i < FLAGS_num_run; ++i) {
      const Request& request = trace.GetRunRequest(i);
      DigestRequest(digest, request);
      if (request.type == RequestType::Read) {
        ++reads;
        Require(preload_keys.count(request.key) == 1,
                "read must target an existing preload key");
      } else if (request.type == RequestType::Insert) {
        ++inserts;
        Require(preload_keys.count(request.key) == 0,
                "insert must be outside the preload key set");
        Require(insert_keys.insert(request.key).second,
                "insert keys must be unique within the trace");
      } else {
        Require(false, "ratio-only D must contain only Read/Insert");
      }
    }
    Require(reads + inserts == FLAGS_num_run, "operation count mismatch");
    const double insert_fraction = static_cast<double>(inserts) / FLAGS_num_run;
    Require(std::abs(insert_fraction - 0.05) < 0.01,
            "generated Insert fraction must be near 5 percent");

    std::array<u64, 2> successful_inserts{};
    std::array<u64, 2> duplicate_inserts{};
    std::array<u64, 2> successful_reads{};
    std::array<u64, 2> sizes{};
    for (unsigned round = 0; round < 2; ++round) {
      for (u64 i = 0; i < FLAGS_num_run; ++i) {
        const Request& request = trace.GetRunRequest(i);
        if (request.type == RequestType::Read) {
          Require(index.Search(request), "replayed read must succeed");
          ++successful_reads[round];
        } else if (index.Insert(request)) {
          ++successful_inserts[round];
        } else {
          ++duplicate_inserts[round];
        }
      }
      sizes[round] = index.records.size();
    }
    Require(successful_inserts[0] == inserts && duplicate_inserts[0] == 0,
            "first round must successfully insert every new trace key");
    Require(successful_inserts[1] == 0 && duplicate_inserts[1] == inserts,
            "second round must replay duplicate Insert attempts");
    Require(successful_reads[0] == reads && successful_reads[1] == reads,
            "all reads in both rounds must succeed");
    Require(sizes[0] == FLAGS_num_load + inserts && sizes[1] == sizes[0],
            "replaying the common trace must not grow the index again");

    // Compare this deterministic line between fresh preset/custom processes.
    std::cout << "TRACE_RESULT digest=" << digest << " reads=" << reads
              << " inserts=" << inserts
              << " first_insert_success=" << successful_inserts[0]
              << " first_insert_duplicate=" << duplicate_inserts[0]
              << " second_insert_success=" << successful_inserts[1]
              << " second_insert_duplicate=" << duplicate_inserts[1]
              << " round0_size=" << sizes[0]
              << " round1_size=" << sizes[1] << '\n';
    std::cout << "PASS ratio-only trace generation and replay\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "FAIL ratio-only trace probe: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
