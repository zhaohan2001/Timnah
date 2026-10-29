#include "sampling.h"

#include <gtest/gtest.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace {
using u64 = std::uint64_t;
using TimnahNameSpace::detail::Sample53;
using TimnahNameSpace::detail::SamplingThreshold;
constexpr u64 kResolution = u64{1} << 53;
constexpr u64 kLargestSample = kResolution - 1;

bool PreviousPredicate(u64 random, double rate) {
  return (random >> 11) * (1.0 / 9007199254740992.0) < rate;
}

bool PreviousStep(u64& random, double rate) {
  if (rate == 0) return false;
  u64 x = random;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  random = x;
  return rate == 1 || PreviousPredicate(x, rate);
}

std::vector<double> BoundaryRates() {
  const double quantum = std::ldexp(1.0, -53);
  return {0.0, -0.0, std::numeric_limits<double>::denorm_min(),
          std::numeric_limits<double>::min(), std::nextafter(quantum, 0.0),
          quantum, std::nextafter(quantum, 1.0),
          std::nextafter(0.1, 0.0), 0.1, std::nextafter(0.1, 1.0),
          std::nextafter(0.5, 0.0), 0.5, std::nextafter(0.5, 1.0),
          std::nextafter(1.0, 0.0), 1.0};
}

void CheckThresholdBoundary(double rate) {
  const u64 threshold = SamplingThreshold(rate);
  ASSERT_LE(threshold, kResolution);
  std::vector<u64> samples{0, 1, 2, kLargestSample - 1, kLargestSample};
  for (int delta = -2; delta <= 2; ++delta) {
    if (delta < 0 && threshold < static_cast<u64>(-delta)) continue;
    const u64 n = delta < 0 ? threshold - static_cast<u64>(-delta)
                            : threshold + static_cast<u64>(delta);
    if (n <= kLargestSample) samples.push_back(n);
  }
  for (const u64 n : samples) {
    // The low eleven RNG bits must not alter the sampling decision.
    for (const u64 tail : std::array<u64, 3>{0, 1, 2047}) {
      const u64 bits = (n << 11) | tail;
      EXPECT_EQ(n < threshold, PreviousPredicate(bits, rate))
          << "rate=" << rate << " n=" << n << " tail=" << tail;
    }
  }
}

TEST(TimnahSampling, IntegerThresholdPreservesFloatingBoundaryDecisions) {
  EXPECT_EQ(SamplingThreshold(0.0), 0);
  EXPECT_EQ(SamplingThreshold(-0.0), 0);
  EXPECT_EQ(SamplingThreshold(std::numeric_limits<double>::denorm_min()), 1);
  EXPECT_EQ(SamplingThreshold(1.0), kResolution);
  for (const double rate : BoundaryRates()) CheckThresholdBoundary(rate);
}

TEST(TimnahSampling, RandomRatesAcrossAllFiniteProbabilityExponents) {
  std::mt19937_64 generator(0x4d595df4d0f33173ULL);
  for (u64 exponent = 0; exponent < 1023; ++exponent) {
    // All non-negative double exponents below 1, including subnormals.
    const u64 fraction = generator() & ((u64{1} << 52) - 1);
    const double rate = std::bit_cast<double>((exponent << 52) | fraction);
    ASSERT_GE(rate, 0.0);
    ASSERT_LT(rate, 1.0);
    CheckThresholdBoundary(rate);
    const auto threshold = SamplingThreshold(rate);
    for (u64 i = 0; i < 64; ++i) {
      const u64 bits = generator();
      EXPECT_EQ((bits >> 11) < threshold, PreviousPredicate(bits, rate));
    }
  }
}

TEST(TimnahSampling, EveryDecisionAndRngStateMatchPreviousSampler) {
  for (const double rate : BoundaryRates()) {
    for (const u64 seed : std::array<u64, 6>{0, 1, 2, 1729,
             0x9e3779b97f4a7c15ULL, std::numeric_limits<u64>::max()}) {
      u64 previous = seed, current = seed;
      const auto threshold = SamplingThreshold(rate);
      for (u64 i = 0; i < 4096; ++i) {
        ASSERT_EQ(Sample53(current, threshold), PreviousStep(previous, rate))
            << "rate=" << rate << " seed=" << seed << " step=" << i;
        ASSERT_EQ(current, previous);
      }
      if (rate == 0) EXPECT_EQ(current, seed);
      if (rate == 1 && seed != 0) EXPECT_NE(current, seed);
    }
  }
}
}  // namespace
