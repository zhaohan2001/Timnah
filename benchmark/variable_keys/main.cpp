#include "common.hpp"
#include "parameter.hpp"
#include "request.hpp"
#include "benchmark/synthetic/profile.hpp"
#include "hashindex/variable_keys/byte_index.hpp"

#include <cstring>
#include <limits>
#include <string_view>

DEFINE_string(index, "Timnah", "Variable-key index: Timnah, Dash, CHash, or EHCache");
DEFINE_uint32(variable_min_key_size, 0, "Minimum generated key length; 0 uses keySize for every key");
DEFINE_bool(variable_trace_probe, false, "Generate and inspect the byte trace without creating an index");

namespace {

struct Operation {
  u64 value{};
  u32 length{};
  RequestType type{};
};

u64 NowNs() {
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void Check(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}

double Unit(std::mt19937_64& random) {
  return double(random() >> 11) * 0x1.0p-53;
}

class ByteTrace {
 public:
  ByteTrace() : stride_(FLAGS_keySize), minimum_(FLAGS_variable_min_key_size ?
      FLAGS_variable_min_key_size : FLAGS_keySize) {
    Check(stride_ >= 8 && stride_ <= 4096 && minimum_ >= 8 && minimum_ <= stride_,
          "Variable benchmark key lengths must be between 8 and 4096 bytes");
    Check(FLAGS_valueSize == 8, "Variable benchmark values must be 8 bytes");
    Check(FLAGS_num_load <= (std::numeric_limits<u64>::max() >> 4), "Key domain is too large");
    Check(FLAGS_num_run <= std::numeric_limits<u64>::max() - FLAGS_num_load - 1,
          "Insertion key domain overflows");
    Check(FLAGS_num_load <= std::numeric_limits<size_t>::max() / stride_ &&
          FLAGS_num_run <= std::numeric_limits<size_t>::max() / stride_,
          "Trace allocation size overflows");
    const u64 begin = NowNs();
    load_.resize(FLAGS_num_load);
    run_.resize(FLAGS_num_run);
    load_keys_.resize(FLAGS_num_load * stride_);
    run_keys_.resize(FLAGS_num_run * stride_);
    order_.resize(FLAGS_num_load);
#pragma omp parallel for schedule(static)
    for (u64 i = 0; i < FLAGS_num_load; ++i) {
      const u64 id = i + 1;
      const auto length = Length(id);
      Encode(load_keys_.data() + i * stride_, id, length);
      load_[i] = {HashFunction(id ^ FLAGS_rndSeed), length, RequestType::Insert};
      order_[i] = i;
    }
    if (FLAGS_shuffle) {
      std::mt19937_64 random(FLAGS_rndSeed);
      std::shuffle(order_.begin(), order_.end(), random);
    }
    const zipfian_int_distribution<u64>::param_type params(1, FLAGS_num_load, FLAGS_skewness);
    const double weights[5] = {FLAGS_read_ratio, FLAGS_update_ratio, FLAGS_insert_ratio,
                              FLAGS_remove_ratio, FLAGS_negative_ratio};
    double total = 0;
    for (const auto weight : weights) total += weight;
    const u64 shift = FLAGS_shift ? HashFunction(FLAGS_rndSeed) % FLAGS_num_load : 0;
    constexpr u64 block_size = 4096;
    const u64 blocks = (FLAGS_num_run - 1) / block_size + 1;
    u64 reads = 0, updates = 0, inserts = 0, removes = 0, negatives = 0;
#pragma omp parallel for schedule(static) reduction(+:reads,updates,inserts,removes,negatives)
    for (u64 block = 0; block < blocks; ++block) {
      std::mt19937_64 random(HashFunction(FLAGS_rndSeed ^ HashFunction(block + 0x5641524b4559ULL)));
      zipfian_int_distribution<u64> ranks(params);
      for (u64 i = block * block_size; i < std::min((block + 1) * block_size, u64(FLAGS_num_run)); ++i) {
        double pick = Unit(random) * total;
        u32 kind = 0;
        while (kind < 4 && pick >= weights[kind]) pick -= weights[kind++];
        const auto type = static_cast<RequestType>(kind);
        u64 id = type == RequestType::Insert ? FLAGS_num_load + i + 1 : ranks(random);
        if (type != RequestType::Insert) id = (id - 1 + shift) % FLAGS_num_load + 1;
        if (type == RequestType::NegativeRead) id += FLAGS_num_load << 3;
        const auto length = Length(id);
        Encode(run_keys_.data() + i * stride_, id, length);
        run_[i] = {random(), length, type};
        reads += type == RequestType::Read;
        updates += type == RequestType::Update;
        inserts += type == RequestType::Insert;
        removes += type == RequestType::Remove;
        negatives += type == RequestType::NegativeRead;
      }
    }
    LOG("Variable trace key_bytes={} minimum_key_bytes={} value_bytes=8 preload={} operations_per_round={} reads={} updates={} inserts={} removes={} negatives={} seed={} shared_trace=true generation_in_throughput=false generation_ns={} trace_bytes={}\n",
        stride_, minimum_, FLAGS_num_load, FLAGS_num_run, reads, updates, inserts, removes,
        negatives, FLAGS_rndSeed, NowNs() - begin,
        load_keys_.size() + run_keys_.size() + (load_.size() + run_.size()) * sizeof(Operation) + order_.size() * sizeof(u64));
  }

  const Operation& Load(u64 i) const { return load_[order_[i]]; }
  std::string_view LoadKey(u64 i) const {
    const auto id = order_[i];
    return {load_keys_.data() + id * stride_, load_[id].length};
  }
  const Operation& Run(u64 i) const { return run_[i]; }
  std::string_view RunKey(u64 i) const { return {run_keys_.data() + i * stride_, run_[i].length}; }

  void ValidateReferences() const {
    std::unordered_map<std::string_view, u64> loaded;
    loaded.reserve(load_.size());
    for (u64 i = 0; i < load_.size(); ++i)
      Check(loaded.emplace(LoadKey(i), Load(i).value).second,
            "Generated preload byte keys are not unique");
    u64 checksum = 14695981039346656037ULL;
    auto add = [&](u64 value) {
      for (unsigned byte = 0; byte < 8; ++byte) {
        checksum ^= (value >> (byte * 8)) & 255;
        checksum *= 1099511628211ULL;
      }
    };
    u64 existing = 0, absent = 0;
    for (u64 i = 0; i < run_.size(); ++i) {
      const auto key = RunKey(i);
      const auto type = Run(i).type;
      const bool expected = type != RequestType::Insert && type != RequestType::NegativeRead;
      Check(loaded.contains(key) == expected, "Generated query bytes disagree with the preloaded domain");
      existing += expected; absent += !expected;
      add(static_cast<u64>(type)); add(key.size()); add(Run(i).value);
      for (const unsigned char byte : key) { checksum ^= byte; checksum *= 1099511628211ULL; }
    }
    LOG("Variable trace validation: unique_loaded={} existing_references={} absent_references={} invalid_references=0 checksum={:016x}\n",
        loaded.size(), existing, absent, checksum);
  }

 private:
  u32 Length(u64 id) const {
    return minimum_ + (HashFunction(id ^ 0x4c454e475448ULL) % (stride_ - minimum_ + 1));
  }
  static void Encode(char* out, u64 id, u32 length) {
    // The first word is a bijection of the ID, so every generated key is unique.
    // Additional words are genuine stored bytes and participate in exact lookup.
    for (u32 offset = 0; offset < length; offset += sizeof(u64)) {
      const u64 word = offset ? HashFunction(id ^ HashFunction(offset)) : HashFunction(id);
      for (u32 byte = 0; byte < std::min<u32>(sizeof(u64), length - offset); ++byte)
        out[offset + byte] = static_cast<char>((word >> (byte * 8)) & 255);
    }
  }
  u32 stride_, minimum_;
  std::vector<Operation> load_, run_;
  std::vector<char> load_keys_, run_keys_;
  std::vector<u64> order_;
};

Result Serve(VariableKeys::Index& index, const Operation& request, std::string_view key) {
  u64 value = 0;
  switch (request.type) {
    case RequestType::Read:
    case RequestType::NegativeRead: return index.Search(key, value);
    case RequestType::Update: return index.Update(key, request.value);
    case RequestType::Insert: return index.Insert(key, request.value);
    case RequestType::Remove: return index.Remove(key);
  }
  return Result::FAILED;
}

void Load(VariableKeys::Index& index, const ByteTrace& trace) {
  u64 failures = 0;
  const u64 begin = NowNs();
#pragma omp parallel for num_threads(FLAGS_num_load_threads) schedule(dynamic,100000) reduction(+:failures)
  for (u64 i = 0; i < FLAGS_num_load; ++i)
    failures += index.Insert(trace.LoadKey(i), trace.Load(i).value) != Result::SUCCESS;
  const auto elapsed = NowNs() - begin;
  LOG("Load time: {:.6f} s\nLoad throughput: {:.3f} Mops/s\nVariable load operations={} failed_operations={} key_bytes={} key_payload_node={} key_arena_bytes={}\n",
      elapsed / 1e9, double(FLAGS_num_load) * 1000.0 / elapsed,
      FLAGS_num_load, failures, FLAGS_keySize, index.KeyNode(), index.KeyBytes());
  Check(!failures, "Variable-key bulk load failed");
  LOG("Load statistics: {}\n", index.Stat());
  if (FLAGS_verify) {
    u64 bad = 0;
#pragma omp parallel for num_threads(FLAGS_num_load_threads) schedule(dynamic,100000) reduction(+:bad)
    for (u64 i = 0; i < FLAGS_num_load; ++i) {
      u64 value = 0;
      const auto result = index.Search(trace.LoadKey(i), value);
      bad += result != Result::SUCCESS || value != trace.Load(i).value;
    }
    Check(!bad, "Variable-key preload verification failed");
    LOG("Variable-key preload verification passed.\n");
  }
}

void Run(VariableKeys::Index& index, const ByteTrace& trace, u32 round) {
  std::vector<std::vector<u64>> latencies(FLAGS_num_run_threads);
  if (FLAGS_latency) {
    const size_t expected = FLAGS_num_run / FLAGS_num_run_threads * FLAGS_latency_sample_rate * 3 + 1024;
    for (auto& values : latencies) values.reserve(expected);
  }
  u64 successful = 0, failed = 0, expected_duplicates = 0, expected_negatives = 0;
  const u64 begin = NowNs();
#pragma omp parallel num_threads(FLAGS_num_run_threads) reduction(+:successful,failed,expected_duplicates,expected_negatives)
  {
    Dice<256> dice(FLAGS_latency_sample_rate);
    auto& local = latencies[omp_get_thread_num()];
#pragma omp for schedule(dynamic,100000)
    for (u64 i = 0; i < FLAGS_num_run; ++i) {
      const bool sample = FLAGS_latency && dice.Roll();
      const u64 start = sample ? NowNs() : 0;
      const auto result = Serve(index, trace.Run(i), trace.RunKey(i));
      if (sample) local.push_back(NowNs() - start);
      const auto type = trace.Run(i).type;
      if (type == RequestType::NegativeRead && result != Result::SUCCESS) ++expected_negatives;
      else if (round && type == RequestType::Insert && result != Result::SUCCESS) ++expected_duplicates;
      else if (result == Result::SUCCESS) ++successful;
      else ++failed;
    }
  }
  const auto elapsed = NowNs() - begin;
  LOG("Run time: {:.6f} s\nRun throughput: {:.3f} Mops/s\n", elapsed / 1e9, double(FLAGS_num_run) * 1000.0 / elapsed);
  LOG("Variable round summary: round={} operations={} successful_operations={} failed_operations={} expected_duplicate_insert_rejections={} expected_negative_reads={} key_bytes={} key_arena_bytes={} elapsed_ns={}\n",
      round, FLAGS_num_run, successful, failed, expected_duplicates, expected_negatives,
      FLAGS_keySize, index.KeyBytes(), elapsed);
  if (FLAGS_workload == "a" || FLAGS_workload == "b" || FLAGS_workload == "c")
    Check(failed == 0 && successful == FLAGS_num_run, "An operation failed in a preloaded YCSB workload");
  std::vector<u64> combined;
  for (const auto& local : latencies) combined.insert(combined.end(), local.begin(), local.end());
  if (!combined.empty()) {
    std::sort(combined.begin(), combined.end());
    for (const double percentile : {.5, .9, .99, .999, .9999, .99999}) {
      const size_t position = std::min(size_t(percentile * combined.size()), combined.size() - 1);
      LOG("Tail latency {:.5}%: {} ns\n", percentile * 100, combined[position]);
    }
  }
  LOG("Run statistics: {}\n", index.Stat());
}
}  // namespace

int main(int argc, char** argv) {
  try {
    gflags::SetCommandLineOptionWithMode("keySize", "16", gflags::SET_FLAGS_DEFAULT);
    Parameter::Init(argc, argv);
    Check(argc == 1, "Unexpected positional argument in variable-key mode");
    Check(!synthetic::IsProfileCode(FLAGS_workload), "Variable mode currently supports YCSB workloads only");
    if (FLAGS_bind_core) Check(numa_run_on_node(0) == 0, "Could not bind the variable benchmark to NUMA node 0");
    omp_set_num_threads(std::max(FLAGS_num_load_threads, FLAGS_num_run_threads));
    LOG("Variable-key mode key_bytes={} value_bytes={} pointer_bytes=8 fingerprint_bits=16 cpu_node={} cpu_binding_enabled={} paper_fast_initial_mib=512 paper_slow_initial_mib=2048 counters_and_metadata_accounted_separately=true\n",
        FLAGS_keySize, FLAGS_valueSize, FLAGS_bind_core ? 0 : -1, FLAGS_bind_core);
    ByteTrace trace;
    if (FLAGS_variable_trace_probe) { trace.ValidateReferences(); return 0; }
    auto param = Parameter::GetIndexParam();
    param.num_threads = FLAGS_num_threads;
    auto index = VariableKeys::MakeIndex(FLAGS_index, param);
    LOG("Variable-key storage key_payload_node={} key_arena_bytes={} trace_storage_excluded_from_index_budget=true\n",
        index->KeyNode(), index->KeyBytes());
    LOG("Variable initial statistics: {}\n", index->Stat());
    Load(*index, trace);
    for (u32 round = 0; round < FLAGS_num_rounds; ++round) {
      LOG("Round {}\n", round);
      Run(*index, trace, round);
    }
    index->Stop();
    LOG("Variable-key benchmark completed successfully.\n");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Variable-key benchmark error: " << error.what() << '\n';
    return 1;
  }
}
