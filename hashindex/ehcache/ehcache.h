#pragma once
#include "common.hpp"

namespace EHCacheNameSpace {
class EHCache final : public IndexInterface {
 public:
  struct Options {
    u8 initial_depth{16};
    int dram_node{0}, slow_node{2};
    size_t fast_bytes{256ull << 20};
    size_t slow_initial_bytes{1ull << 30};
    size_t slow_growth_bytes{64ull << 20};
    double sample_rate{0.1};
  };
  EHCache();
  explicit EHCache(Options options);
  ~EHCache() override;
  Result Create(const ParamType&) override;
  void Stop() override;
  Result Insert(const u64&, const u64&) override;
  Result Search(const u64&, u64&) override;
  Result Update(const u64&, const u64&) override;
  Result Remove(const u64&) override;
  string Stat() override;
 private:
  class Impl;
  Options options_;
  std::unique_ptr<Impl> impl_;
};
}  // namespace EHCacheNameSpace
