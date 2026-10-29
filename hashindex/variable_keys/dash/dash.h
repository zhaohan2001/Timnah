#pragma once
#include "common.hpp"

namespace DashNameSpace {
// Dash-EH: 64-bit key tokens and values, in-bucket metadata, neighbor balancing, and stash buckets.
class Dash final : public IndexInterface {
 public:
  struct Options {
    u8 initial_depth{16};
    int dram_node{0};
    int slow_node{2};
    size_t fast_bytes{512ull << 20};
    size_t slow_initial_bytes{2ull << 30};
    size_t slow_growth_bytes{64ull << 20};
  };
  Dash();
  explicit Dash(Options options);
  ~Dash() override;
  auto Create(const ParamType& param) -> Result override;
  auto Stop() -> void override;
  auto Insert(const u64& key, const u64& value) -> Result override;
  auto Search(const u64& key, u64& value) -> Result override;
  auto Update(const u64& key, const u64& value) -> Result override;
  auto Remove(const u64& key) -> Result override;
  auto Stat() -> string override;
 private:
  class Impl;
  Options options_;
  std::unique_ptr<Impl> impl_;
};
}  // namespace DashNameSpace
