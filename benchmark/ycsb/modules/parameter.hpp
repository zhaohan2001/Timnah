/// Command-line parameter handling
#pragma once

#include "common.hpp"

/// Command-line flag declarations
// ----------------------------------------------------------------------------
DECLARE_string(index);
DECLARE_uint64(dram_size);
DECLARE_uint64(num_bg);

/// Threads
DECLARE_bool(bind_core);
DECLARE_uint32(num_threads);

DECLARE_uint32(num_load_threads);
DECLARE_uint32(num_run_threads);

/// Trace configuration
DECLARE_uint32(num_rounds);

DECLARE_uint64(num_load);
DECLARE_uint64(num_run);

DECLARE_double(skewness);

DECLARE_string(workload);

// Only used by the synthetic Twitter/Webmail presets.
DECLARE_uint64(real_epoch_ops);
DECLARE_uint64(real_drift_stride);

DECLARE_double(read_ratio);
DECLARE_double(update_ratio);
DECLARE_double(insert_ratio);
DECLARE_double(remove_ratio);
DECLARE_double(negative_ratio);

DECLARE_bool(verify);
DECLARE_bool(shift);
DECLARE_bool(shuffle);

/// Statistics flags
DECLARE_bool(latency);
DECLARE_double(latency_sample_rate);

DECLARE_uint32(rndSeed);

/// Legacy flags
DECLARE_uint32(keySize);
DECLARE_uint32(valueSize);

/// Command-line parameter handling
// ----------------------------------------------------------------------------
class Parameter {
 public:
  /// Parse command-line flags
  static auto Init(int& argc, char**& argv) -> void;
  /// Build index configuration
  static auto GetIndexParam() -> IndexInterface::ParamType;

 private:
  /// Set command-line usage text
  static auto SetHelpMessage() -> void;
  /// Validate command-line flags
  static auto CheckSanity() -> void;
};

extern IndexInterface* GetIndexPtr();
