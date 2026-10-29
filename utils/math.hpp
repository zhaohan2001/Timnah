/// Math utilities
#pragma once
#include "headers.hpp"
/// Constants
// ----------------------------------------------------------------
constexpr u64 LSB = u64(1);
constexpr u64 LSB_MASK = ~u64(LSB);
constexpr u64 MSB = u64(1) << 63;
constexpr u64 MSB_MASK = ~(MSB);
constexpr u64 MSB2 = u64(1) << 62;
constexpr u64 MSB2_MASK = ~(MSB2);
constexpr u64 MILLION = 1'000'000;
constexpr u64 MB = 1 << 20;
constexpr u64 GB = 1 << 30;

/// Bit manipulation functions
// ----------------------------------------------------------------

/// Check whether the value is a power of two.
constexpr auto IsPowerOfTwo(u64 number) -> bool {
  return (number & (number - 1)) == 0;
}

/// Get the power-of-two exponent. (Requires number > 0.)
/// Implemented using count-leading-zeros.
constexpr auto LogBase2(u64 number) -> u64 {
  return sizeof(u64) * 8 - __builtin_clzll(number) - 1;
}

// Round down to a power of two.
inline auto RoundDownPowerOfTwo(u64 number) -> u64 {
  if ((number & (number - 1)) == 0) {
    return number;
  }
  int bitPosition = sizeof(u64) * 8 - __builtin_clzll(number) - 1;
  return 1ull << bitPosition;
}

// Round up to a power of two.
inline auto RoundUpPowerOfTwo(u64 number) -> u64 {
  if ((number & (number - 1)) == 0) {
    return number;
  }
  int bitPosition = sizeof(u64) * 8 - __builtin_clzll(number);
  return 1ull << bitPosition;
}

// Extract bits [x, y] and shift them to the low bits. (0 <= x <= y <= 63)
inline auto GetBitsFromMSB(u64 number, u64 x, u64 y) -> u64 {
  return (number << x) >> (x + 63 - y);
}

// Convert a 64-bit integer to a fixed-length readable string (truncate or pad with 'A').
inline auto EncodeIntToString(u64 value, size_t length) -> string {
  const static string base64_chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  string encodedString;

  while (length > 0) {
    encodedString += base64_chars[value & 0x3F];
    value = value >> 6;
    length--;
  }

  return encodedString;
}

/// Hash functions
// ----------------------------------------------------------------------------
inline auto murmur64(u64 h) -> u64 {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53L;
  h ^= h >> 33;
  return h;
}

inline auto splitmix64(u64 x) -> u64 {
  u64 z = (x + 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// Hash functions
inline auto HashFunction(const u64 key) -> u64 { return splitmix64(key); }
inline auto HashFunction(const string& key) -> u64 {
  return std::hash<string>{}(key);
}

/// Random utilities
// ----------------------------------------------------------------------------
// Random trigger (4 ns latency)
// Parameter: trigger probability in [0, 1]
class RandomFlag {
 public:
  /* Construction */
  RandomFlag(double ratio) : dist_(0, static_cast<int>(1.0 / ratio * SCALE)) {}

  /* Operations */
  auto Rand() -> bool {
    result_ = (dist_(randomEngine_) < SCALE);
    return Test();
  }
  auto Test() -> bool { return result_; }

 private:
  /* Data */
  constexpr static u64 SCALE{10};
  mt19937_64 randomEngine_{static_cast<u64>(rand())};
  uniform_int_distribution<int> dist_{};
  bool result_{};
};

/// Dice with a selected probability.
/// Usually N=256. Performance is insensitive to N; latency is about 25 ns.
template <int N>
class Dice {
 public:
  Dice(double p) {
    default_random_engine randomEngine;
    bernoulli_distribution bernoulliDistribution(p);
    for (int i = 0; i < N; ++i) {
      dices_[i] = bernoulliDistribution(randomEngine);
    }
  }
  auto Roll() -> bool {
    index_ = ++index_ & (N - 1);
    return dices_[index_];
  }

 private:
  bitset<N> dices_;
  int index_{0};
};