#pragma once

#include <cmath>
#include <cstdint>

namespace TimnahNameSpace::detail {

// Create validates that rate is finite and in [0, 1]. The previous predicate
// was double(random >> 11) * 2^-53 < rate. Both power-of-two scalings are
// exact for this domain; for integer n, n < rate*2^53 iff n < ceil(rate*2^53).
// Even the smallest positive double scales to 2^-1021, so no underflow occurs.
inline std::uint64_t SamplingThreshold(double rate) {
  return static_cast<std::uint64_t>(std::ceil(std::ldexp(rate, 53)));
}

inline bool Sample53(std::uint64_t& random, std::uint64_t threshold) {
  // Rate zero did not advance the RNG. Rate one did, and must still do so.
  if (!threshold) return false;
  std::uint64_t x = random;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  random = x;
  return (x >> 11) < threshold;
}

}  // namespace TimnahNameSpace::detail
