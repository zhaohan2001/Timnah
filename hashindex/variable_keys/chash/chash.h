#pragma once
#include "common.hpp"

namespace CHashNameSpace {

// Basic CHash implementation: 8-byte key tokens and values, 256-byte buckets, and 8-KiB slow-tier segments.
// ----------------------------------------------------------------
class CHash final : public IndexInterface {
 public:
  struct Options {
    u8 initial_depth{16};
    int dram_node{0};
    int slow_node{2};
    size_t fast_bytes{512ull << 20};
    size_t slow_initial_bytes{2ull << 30};
    size_t slow_growth_bytes{64ull << 20};
  };

  CHash();
  explicit CHash(Options options);
  ~CHash() override;
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

}  // namespace CHashNameSpace
