#pragma once

#include "benchmark/synthetic/profile.hpp"
#include "benchmark/ycsb/modules/request.hpp"

#include <random>
#include <vector>

namespace synthetic {

// An exact (up to floating-point and 53-bit uniform precision) finite discrete
// Zipf sampler: P(rank=k) is proportional to k^(-alpha), 1 <= k <= size.
// The bounded sampler accepts positive alpha, including values above one.
// Construction and sampling use O(1) space with a small acceptance-rate cache.
class BoundedZipf {
 public:
  BoundedZipf(u64 size, double alpha);
  auto Sample(std::mt19937_64& random) const -> u64;

 private:
  auto Acceptance(u64 rank) const -> double;

  u64 size_;
  double alpha_;
  double exponent_;
  double inverse_span_;
  double log_span_;
  std::vector<double> acceptance_;
};

struct AuditStats {
  u64 reads{};
  u64 updates{};
  u64 first_epoch{};
  u64 last_epoch{};
  u64 min_key_id{};
  u64 max_key_id{};
};

// Map an immutable popularity rank to an immutable preloaded key ID. This is a
// bijection of [1, size] at every epoch; it never creates an absent read target.
auto RotateRank(u64 rank, u64 size, u64 epoch, u64 drift_stride) -> u64;

// A round is prepared completely before its execution timer starts. Requests
// follow observed input order, independent of preparation thread count.
// Concurrent index workers retain their existing scheduling policy.
//
// The logical offset is round * num_run + request position. A finite trace
// wraps at its end and continues across rounds without resampling or drift.
// Seed and drift arguments remain accepted for interface compatibility.
auto GenerateRound(const Profile& profile, u64 num_load, u64 num_run, u64 round,
                   u64 seed, u64 epoch_ops, u64 drift_stride,
                   AuditStats* audit = nullptr) -> std::vector<Request>;

}  // namespace synthetic
