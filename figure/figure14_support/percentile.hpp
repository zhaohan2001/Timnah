#pragma once
#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace figure14 {
// Match ordinary ycsb's order statistic. Clamp tiny deterministic test inputs.
inline std::size_t PercentileIndex(double probability, std::size_t population) {
  if (!population || probability < 0 || probability > 1)
    throw std::invalid_argument("Invalid percentile population or probability");
  return std::max<std::size_t>(1, static_cast<std::size_t>(probability * population)) - 1;
}
}
