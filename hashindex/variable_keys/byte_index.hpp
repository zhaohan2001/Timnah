#pragma once

#include "common.hpp"

#include <memory>
#include <string_view>

namespace VariableKeys {

// Byte strings are copied on insertion and compared by length and contents.
// Query buffers remain owned by the caller and need only outlive that call.
class Index {
 public:
  virtual ~Index() = default;
  virtual auto Insert(std::string_view key, u64 value) -> Result = 0;
  virtual auto Search(std::string_view key, u64& value) -> Result = 0;
  virtual auto Update(std::string_view key, u64 value) -> Result = 0;
  virtual auto Remove(std::string_view key) -> Result = 0;
  virtual auto Stat() -> std::string = 0;
  virtual void Stop() = 0;
  virtual auto KeyBytes() const -> std::size_t = 0;
  virtual auto KeyNode() const -> int = 0;
};

// The production factory uses the separate executable's index-specific flags.
// Its defaults are 512 MiB fast memory and 2 GiB initial slow memory.
auto MakeIndex(std::string_view name, const IndexInterface::ParamType& param)
    -> std::unique_ptr<Index>;

// Small independent correctness fixtures do not reserve the paper-sized pools.
// This factory is not used by the benchmark or the 8-byte executable.
struct TestOptions {
  int fast_node{-1};
  int slow_node{-1};
  u8 initial_depth{1};
  u8 timnah_fast_depth{1};
  u8 timnah_slow_depth{1};
  std::size_t fast_bytes{4ull << 20};
  std::size_t slow_initial_bytes{4ull << 20};
  std::size_t slow_growth_bytes{4ull << 20};
  bool prefault{false};
  double sample_rate{0.1};
  double convergence_threshold{0};
};

auto MakeTestIndex(std::string_view name, const IndexInterface::ParamType& param,
                   const TestOptions& options = {}) -> std::unique_ptr<Index>;

}  // namespace VariableKeys
