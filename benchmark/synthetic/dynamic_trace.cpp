#include "benchmark/synthetic/dynamic_trace.hpp"

#include "benchmark/twitter/profiles.hpp"
#include "benchmark/webmail/profile.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "trace_replay_impl.hpp"

namespace synthetic {
namespace {

constexpr u64 kRandomBlockSize = 4096;
constexpr u64 kAcceptanceCacheSize = 4096;

auto UnitRandom(std::mt19937_64& random) -> double {
  // Explicit conversion avoids implementation-dependent distribution adapters.
  return static_cast<double>(random() >> 11) * 0x1.0p-53;
}

auto CodeSeed(const std::string& code) -> u64 {
  u64 value = 14695981039346656037ULL;
  for (const unsigned char character : code) {
    value ^= character;
    value *= 1099511628211ULL;
  }
  return value;
}

auto BlockSeed(u64 seed, u64 profile_seed, u64 round, u64 block) -> u64 {
  return splitmix64(seed ^ splitmix64(profile_seed) ^
                    splitmix64(round + 0xD1B54A32D192ED03ULL) ^
                    splitmix64(block + 0x94D049BB133111EBULL));
}

void ValidateProfile(const Profile& profile) {
  if (!std::isfinite(profile.read_ratio) || profile.read_ratio < 0 ||
      !std::isfinite(profile.update_ratio) || profile.update_ratio < 0 ||
      std::abs(profile.read_ratio + profile.update_ratio - 1.0) > 1e-12) {
    throw std::invalid_argument("synthetic read/update ratios must sum to one");
  }
  if (!std::isfinite(profile.alpha) || profile.alpha <= 0) {
    throw std::invalid_argument("synthetic Zipf alpha must be positive and finite");
  }
}

}  // namespace

auto IsProfileCode(const std::string& code) -> bool {
  return code == "ts" || code == "tt" || code == "tc" || code == "w";
}

auto GetProfile(const std::string& code) -> Profile {
  if (auto profile = twitter_workload::GetProfile(code)) return *profile;
  if (code == "w") return webmail_workload::GetProfile();
  throw std::invalid_argument("unknown synthetic workload code: " + code);
}

BoundedZipf::BoundedZipf(u64 size, double alpha)
    : size_(size), alpha_(alpha), exponent_(1.0 - alpha),
      inverse_span_(0), log_span_(0) {
  if (!size || size > (u64{1} << 53)) {
    throw std::invalid_argument("bounded Zipf size must be in [1, 2^53]");
  }
  if (!std::isfinite(alpha) || alpha <= 0) {
    throw std::invalid_argument("bounded Zipf alpha must be positive and finite");
  }
  log_span_ = std::log((static_cast<double>(size) + 0.5) / 0.5);
  inverse_span_ = std::expm1(exponent_ * log_span_);
  if (!std::isfinite(inverse_span_)) {
    throw std::invalid_argument("bounded Zipf parameters exceed numeric range");
  }
  acceptance_.resize(std::min(size, kAcceptanceCacheSize));
  for (u64 rank = 1; rank <= acceptance_.size(); ++rank) {
    acceptance_[rank - 1] = Acceptance(rank);
  }
}

auto BoundedZipf::Acceptance(u64 rank) const -> double {
  // The proposal mass for integer k is the integral of x^-alpha over
  // [k-0.5,k+0.5]. Convexity gives mass >= k^-alpha, so this ratio is <= 1.
  // The stable form below avoids subtracting two almost equal powers in tails.
  const double lower = static_cast<double>(rank) - 0.5;
  const double reciprocal = 1.0 / lower;
  double probability;
  if (exponent_ == 0.0) {
    probability = (1.0 / static_cast<double>(rank)) / std::log1p(reciprocal);
  } else {
    probability = std::exp(-alpha_ * std::log1p(0.5 * reciprocal)) *
                  (exponent_ * reciprocal) /
                  std::expm1(exponent_ * std::log1p(reciprocal));
  }
  return std::clamp(probability, 0.0, 1.0);
}

auto BoundedZipf::Sample(std::mt19937_64& random) const -> u64 {
  if (size_ == 1) return 1;
  for (;;) {
    const double uniform = UnitRandom(random);
    const double proposal = exponent_ == 0.0
        ? 0.5 * std::exp(uniform * log_span_)
        : 0.5 * std::exp(std::log1p(uniform * inverse_span_) / exponent_);
    const u64 rank = std::clamp(static_cast<u64>(std::floor(proposal + 0.5)),
                                u64{1}, size_);
    const double acceptance = rank <= acceptance_.size()
        ? acceptance_[rank - 1] : Acceptance(rank);
    if (UnitRandom(random) < acceptance) return rank;
  }
}

auto RotateRank(u64 rank, u64 size, u64 epoch, u64 drift_stride) -> u64 {
  if (!size || !rank || rank > size) {
    throw std::invalid_argument("rank rotation requires 1 <= rank <= size");
  }
  const u64 offset = static_cast<u64>(
      (static_cast<u128>(epoch) * (drift_stride % size)) % size);
  return static_cast<u64>((static_cast<u128>(rank - 1) + offset) % size) + 1;
}

auto GenerateRound(const Profile& profile, u64 num_load, u64 num_run, u64 round,
                   u64 seed, u64 epoch_ops, u64 drift_stride,
                   AuditStats* audit) -> std::vector<Request> {
  return real_trace::Generate(profile,num_load,num_run,round,audit);
}

}  // namespace synthetic
