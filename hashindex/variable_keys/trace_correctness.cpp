// Compile the real generator in this test translation unit so its public
// accessors can be checked without starting an index or changing the driver.
#define main VariableKeysBenchmarkMain
#include "../../benchmark/variable_keys/main.cpp"
#undef main

#include <set>
#include <unordered_map>

namespace {
struct TraceAudit {
  u64 digest{14695981039346656037ULL};
  u64 reads{}, updates{}, inserts{};
  std::set<std::size_t> lengths;
};

void AuditByte(TraceAudit& audit, unsigned char value) {
  audit.digest = (audit.digest ^ value) * 1099511628211ULL;
}

void AuditWord(TraceAudit& audit, u64 value) {
  for (unsigned byte = 0; byte < 8; ++byte)
    AuditByte(audit, static_cast<unsigned char>(value >> (8 * byte)));
}

void AuditKey(TraceAudit& audit, std::string_view key) {
  AuditWord(audit, key.size());
  for (unsigned char byte : key) AuditByte(audit, byte);
}

void Configure(char workload, unsigned threads, u32 maximum, u32 minimum) {
  FLAGS_num_load = 4096;
  FLAGS_num_run = 100000;
  FLAGS_keySize = maximum;
  FLAGS_variable_min_key_size = minimum;
  FLAGS_valueSize = 8;
  FLAGS_rndSeed = 1729;
  FLAGS_skewness = 0.99;
  FLAGS_shift = false;
  FLAGS_shuffle = true;
  FLAGS_read_ratio = workload == 'a' ? 0.5 : workload == 'c' ? 1.0 : 0.95;
  FLAGS_update_ratio = workload == 'a' ? 0.5 : workload == 'b' ? 0.05 : 0.0;
  FLAGS_insert_ratio = workload == 'd' ? 0.05 : 0.0;
  FLAGS_remove_ratio = FLAGS_negative_ratio = 0;
  omp_set_num_threads(threads);
}

auto AuditTrace(char workload, unsigned threads, u32 maximum, u32 minimum)
    -> TraceAudit {
  Configure(workload, threads, maximum, minimum);
  ByteTrace trace;
  TraceAudit audit;
  std::unordered_map<std::string, const char*> loaded;
  std::set<std::string> inserted;
  u64 nul_bytes = 0, high_bytes = 0;
  for (u64 i = 0; i < FLAGS_num_load; ++i) {
    const auto key = trace.LoadKey(i);
    const auto& operation = trace.Load(i);
    Check(key.size() == operation.length && key.size() >= (minimum ? minimum : maximum) &&
              key.size() <= maximum && operation.type == RequestType::Insert,
          "A preload key has an incorrect length or operation type");
    Check(loaded.emplace(std::string(key), key.data()).second,
          "Generated preload keys are not unique");
    audit.lengths.insert(key.size());
    AuditKey(audit, key);
    AuditWord(audit, operation.value);
    for (unsigned char byte : key) {
      nul_bytes += byte == 0;
      high_bytes += byte >= 128;
    }
  }
  Check(nul_bytes > 0 && high_bytes > 0,
        "The diagnostic trace did not exercise binary key bytes");
  for (u64 i = 0; i < FLAGS_num_run; ++i) {
    const auto key = trace.RunKey(i);
    const auto& operation = trace.Run(i);
    Check(key.size() == operation.length && key.size() >= (minimum ? minimum : maximum) &&
              key.size() <= maximum, "A query key has an incorrect length");
    if (operation.type == RequestType::Read || operation.type == RequestType::Update) {
      const auto found = loaded.find(std::string(key));
      Check(found != loaded.end(), "A preloaded query does not match complete stored bytes");
      Check(found->second != key.data(), "Load and query keys alias the same buffer");
    } else if (operation.type == RequestType::Insert) {
      Check(!loaded.contains(std::string(key)) && inserted.insert(std::string(key)).second,
            "A generated insertion key is not new and unique");
    } else {
      throw std::runtime_error("An A/B/C/D trace contains an unexpected operation");
    }
    audit.reads += operation.type == RequestType::Read;
    audit.updates += operation.type == RequestType::Update;
    audit.inserts += operation.type == RequestType::Insert;
    AuditKey(audit, key);
    AuditWord(audit, operation.value);
    AuditByte(audit, static_cast<unsigned char>(operation.type));
  }
  Check(audit.reads + audit.updates + audit.inserts == FLAGS_num_run,
        "The trace audit did not cover every request");
  if (workload == 'c') Check(audit.reads == FLAGS_num_run, "YCSB C is not entirely reads");
  else if (workload == 'a')
    Check(audit.reads > 48500 && audit.reads < 51500 && audit.inserts == 0,
          "YCSB A operation frequencies differ from 50/50");
  else
    Check(audit.reads > 93500 && audit.reads < 96500,
          "YCSB B/D operation frequencies differ from 95/5");
  if (minimum && minimum < maximum)
    Check(audit.lengths.size() > 1, "The mixed-length trace has only one key length");
  else
    Check(audit.lengths == std::set<std::size_t>{maximum},
          "The fixed-size trace has unexpected key lengths");
  return audit;
}
}  // namespace

int main() {
  try {
    for (const char workload : {'a', 'b', 'c', 'd'}) {
      for (const auto [maximum, minimum] :
           {std::pair<u32, u32>{16, 0}, {17, 8}, {33, 11}}) {
        const auto scalar = AuditTrace(workload, 1, maximum, minimum);
        const auto parallel = AuditTrace(workload, 4, maximum, minimum);
        Check(scalar.digest == parallel.digest && scalar.reads == parallel.reads &&
                  scalar.updates == parallel.updates && scalar.inserts == parallel.inserts,
              "Generated byte traces depend on the generation thread count");
      }
    }
    std::cout << "Variable trace correctness passed: binary bytes, lengths, ownership, "
                 "A/B/C/D ratios, unique insertions and parallel determinism." << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Variable trace correctness failure: " << error.what() << std::endl;
    return 1;
  }
}
