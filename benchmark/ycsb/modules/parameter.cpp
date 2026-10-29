#include "parameter.hpp"
#include "benchmark/synthetic/profile.hpp"
#include "benchmark/synthetic/trace_replay.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
[[noreturn]] void ParameterError(const std::string& message) {
  std::cerr << "YCSB parameter error: " << message << std::endl;
  std::exit(EXIT_FAILURE);
}
}


/// Command-line flag definitions
// ----------------------------------------------------------------------------
DEFINE_uint64(dram_size, 1024, "DRAM size");
DEFINE_uint64(num_bg, 1, "Number of background threads");

/// Benchmark environment flags

/// Threads
DEFINE_bool(bind_core, true, "Bind worker threads to CPU cores");
DEFINE_uint32(num_threads, 20, "Number of threads");

DEFINE_uint32(num_load_threads, 0, "Number of load threads");
DEFINE_uint32(num_run_threads, 0, "Number of run threads");

/// Trace configuration
DEFINE_uint32(num_rounds, 30, "Number of benchmark rounds");

DEFINE_uint64(num_load, int(1e8), "Number of preloaded records");
DEFINE_uint64(num_run, int(1e8), "Number of operations");

DEFINE_double(skewness, 0.99, "Zipf distribution parameter");

DEFINE_string(workload, "", "Preset a/b/c/d, ts (Twitter Storage), tt (Twitter Transient), tc (Twitter Compute), or w (Webmail), case-insensitive; empty keeps custom ratio flags. Real-world presets replay installed observed traces. D remains ratio-only.");
DEFINE_uint64(real_epoch_ops, 10000000, "Accepted option; not used by observed trace replay");
DEFINE_uint64(real_drift_stride, 0, "Accepted option; not used by observed trace replay");

DEFINE_double(read_ratio, 1.0, "Read operation ratio");
DEFINE_double(update_ratio, 0.0, "Update operation ratio");
DEFINE_double(insert_ratio, 0.0, "Insert operation ratio");
DEFINE_double(remove_ratio, 0.0, "Remove operation ratio");
DEFINE_double(negative_ratio, 0.0, "Unsuccessful lookup ratio");

DEFINE_bool(verify, false, "Verify loaded records");
DEFINE_bool(shift, false, "Enable hotspot shifting");
DEFINE_bool(shuffle, true, "Shuffle keys before insertion");

/// Statistics flags
DEFINE_bool(latency, true, "Collect latency statistics");
DEFINE_double(latency_sample_rate, 0.01, "Latency sampling rate");

DEFINE_uint32(rndSeed, 1729, "Random seed");

/// Auxiliary flags
DEFINE_uint32(keySize, 8, "Key size");
DEFINE_uint32(valueSize, 8, "Value size");

/// Implementation
// ----------------------------------------------------------------------------
/// Initialization
auto Parameter::Init(int& argc, char**& argv) -> void {
  SetHelpMessage();
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  if (!FLAGS_workload.empty()) {
    std::transform(FLAGS_workload.begin(), FLAGS_workload.end(), FLAGS_workload.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool real = synthetic::IsProfileCode(FLAGS_workload);
    if (FLAGS_workload != "a" && FLAGS_workload != "b" && FLAGS_workload != "c" && FLAGS_workload != "d" && !real)
      ParameterError("-workload must be a, b, c, d, ts, tt, tc or w");
    for (const char* flag : {"read_ratio", "update_ratio", "insert_ratio", "remove_ratio", "negative_ratio"}) {
      gflags::CommandLineFlagInfo info;
      if (!gflags::GetCommandLineFlagInfo(flag, &info) || !info.is_default)
        ParameterError(std::string("-workload cannot be combined with -") + flag + "; use either a preset or custom ratios");
    }
    if (real) {
      try {
        real_trace::Initialize(FLAGS_workload, FLAGS_num_load);
      } catch (const std::exception& error) {
        ParameterError(error.what());
      }
      const auto profile = synthetic::GetProfile(FLAGS_workload);
      gflags::CommandLineFlagInfo info;
      if (!gflags::GetCommandLineFlagInfo("skewness", &info) || !info.is_default)
        ParameterError("real trace replay preserves the observed distribution; omit -skewness");
      FLAGS_read_ratio = profile.read_ratio;
      FLAGS_update_ratio = profile.update_ratio;
      FLAGS_insert_ratio = FLAGS_remove_ratio = FLAGS_negative_ratio = 0.0;
      FLAGS_skewness = profile.alpha;
      if (FLAGS_keySize != 8 || FLAGS_valueSize != 8)
        ParameterError("observed Twitter/Webmail presets require 8-byte keys and 8-byte values");
      if (gflags::GetCommandLineFlagInfo("num_rounds", &info) && info.is_default)
        FLAGS_num_rounds = 10;
      if (!FLAGS_real_drift_stride)
        FLAGS_real_drift_stride = std::max<uint64_t>(FLAGS_num_load / 100, 1);
      // Observed traces use continuous adaptation unless explicitly configured.
      std::string selected_index;
      if (gflags::GetCommandLineOption("index", &selected_index) && selected_index == "Timnah" &&
          gflags::GetCommandLineFlagInfo("timnah_convergence_threshold", &info) && info.is_default) {
        if (gflags::SetCommandLineOption("timnah_convergence_threshold", "0").empty())
          ParameterError("could not enable continuous Timnah adaptation");
        LOG("Real trace preset: timnah_convergence_threshold=0 (continuous adaptation; explicit flag overrides this preset)\n");
      }
    } else {
      FLAGS_read_ratio = FLAGS_workload == "a" ? 0.5 : FLAGS_workload == "c" ? 1.0 : 0.95;
      FLAGS_update_ratio = FLAGS_workload == "a" ? 0.5 : FLAGS_workload == "b" ? 0.05 : 0.0;
      FLAGS_insert_ratio = FLAGS_workload == "d" ? 0.05 : 0.0;
      FLAGS_remove_ratio = FLAGS_negative_ratio = 0.0;
    }
  }
  CheckSanity();
  LOG("{} workload={} read={} update={} insert={} remove={} negative={} distribution={}\n",
      synthetic::IsProfileCode(FLAGS_workload) ? "Real" : "YCSB",
      FLAGS_workload.empty() ? "custom" : FLAGS_workload,
      FLAGS_read_ratio, FLAGS_update_ratio, FLAGS_insert_ratio, FLAGS_remove_ratio, FLAGS_negative_ratio,
      synthetic::IsProfileCode(FLAGS_workload) ? "trace" : "zipfian");
  if (synthetic::IsProfileCode(FLAGS_workload)) {
    const auto profile = synthetic::GetProfile(FLAGS_workload);
    LOG("Real profile={} source={} key_bytes=8 value_bytes=8 preload={} operations_per_round={} rounds={} source_records={} source_sha256={} model=index_read_overwrite finite_trace_wrap=true\n",
        profile.name, profile.source, FLAGS_num_load, FLAGS_num_run, FLAGS_num_rounds,
        real_trace::Current().records, real_trace::Current().source_sha256);

  }
  if (FLAGS_workload == "d")
    LOG("D mode=ratio_only shared_trace=true reads=preloaded_zipf replay_same_insert_keys_each_round=true\n");
}

/// Set the usage format and command examples
auto Parameter::SetHelpMessage() -> void {
  gflags::SetUsageMessage("Usage: ./ycsb -index=Timnah -workload=ts -num_rounds=10 -num_threads=30 -num_bg=1; presets a/b/c/d/ts/tt/tc/w; omit -workload to use custom -read_ratio/-update_ratio/...");
}

auto Parameter::CheckSanity() -> void {
  double total = 0;
  for (double value : {FLAGS_read_ratio, FLAGS_update_ratio, FLAGS_insert_ratio, FLAGS_remove_ratio, FLAGS_negative_ratio}) {
    if (!std::isfinite(value) || value < 0) ParameterError("operation ratios must be finite and nonnegative");
    total += value;
  }
  if (!std::isfinite(total) || total <= 0) ParameterError("operation ratios must have a positive finite total");
  // Custom operation ratios are weights and need not sum to one.
  if (!FLAGS_num_load || !FLAGS_num_run || !FLAGS_num_rounds || !FLAGS_num_threads)
    ParameterError("num_load, num_run, num_rounds and num_threads must be positive");
  if (synthetic::IsProfileCode(FLAGS_workload)) {
    if (FLAGS_num_run > std::numeric_limits<uint64_t>::max() / FLAGS_num_rounds)
      ParameterError("total real trace operation count overflows uint64");
  } else if (!std::isfinite(FLAGS_skewness) || FLAGS_skewness <= 0 || FLAGS_skewness >= 1) {
    ParameterError("skewness must be in (0,1)");
  }
  if (!std::isfinite(FLAGS_latency_sample_rate) || FLAGS_latency_sample_rate < 0 || FLAGS_latency_sample_rate > 1 ||
      (FLAGS_latency && FLAGS_latency_sample_rate == 0))
    ParameterError("latency_sample_rate must be in [0,1], and positive when latency is enabled");
  if (FLAGS_num_load_threads == 0) FLAGS_num_load_threads = FLAGS_num_threads;
  if (FLAGS_num_run_threads == 0) FLAGS_num_run_threads = FLAGS_num_threads;
}
auto Parameter::GetIndexParam() -> IndexInterface::ParamType {
  IndexInterface::ParamType param;
  param.dram_size = FLAGS_dram_size;
  param.num_bg = FLAGS_num_bg;
  return param;
}
