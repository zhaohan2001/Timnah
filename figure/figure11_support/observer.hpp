#pragma once

#include <cstdint>

// This header belongs only to the isolated Figure 11 diagnostic executable.
// Every source is linked into that one executable, so all hooks use the same
// thread-local counter. Production indexes do not include this header.
namespace figure11 {
struct Counters {
  std::uint64_t dram_hits{0};
  std::uint64_t slow_hits{0};
};

inline thread_local Counters counters;

inline void Record(bool fast) noexcept {
  if (fast) ++counters.dram_hits;
  else ++counters.slow_hits;
}

inline Counters Drain() noexcept {
  const auto result = counters;
  counters = {};
  return result;
}
}  // namespace figure11
