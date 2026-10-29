/// Requests and request generators
#pragma once

#include "common.hpp"
#include "zipf.hpp"

/// Request type definitions
// ----------------------------------------------------------------------------
using EnumReqType = u8;
enum class RequestType : u8 {
  Read = 0,
  Update = 1,
  Insert = 2,
  Remove = 3,
  NegativeRead = 4
};

struct RequestTypeRatio {
  double read_ratio{};
  double update_ratio{};
  double insert_ratio{};
  double remove_ratio{};
  double negative_ratio{};
};

/// Key/value request
struct Request {
  u64 key;
  u64 value;
  RequestType type;
};

/// Request type generator
// ----------------------------------------------------------------------------
class RequestTypeGen {
 public:
  // ~~~~Construction~~~~
  RequestTypeGen(RequestTypeRatio ratio)
      : distribution_({ratio.read_ratio, ratio.update_ratio, ratio.insert_ratio,
                       ratio.remove_ratio, ratio.negative_ratio}) {}
  static auto SetSeed(u64 seed) -> void { random_engine_.seed(seed); }

  // ~~~~Basic operations~~~~
  /// Generate a request type (thread safe)
  auto RandType() -> RequestType {
    return static_cast<RequestType>(distribution_(random_engine_));
  }

 private:
  // ~~~~Data~~~~
  inline static thread_local mt19937_64 random_engine_{
      static_cast<u64>(rand())};
  discrete_distribution<EnumReqType> distribution_{};
};

/// Key generator
// ----------------------------------------------------------------------------
class KeyGen {
 public:
  // ~~~~Construction~~~~
  KeyGen(u64 num_load, double skewness)
      : num_load_{num_load},
        cur_ID_{num_load + 1},
        zipf_dist_{1, num_load, skewness} {}
  auto SetSeed(u64 seed) { random_engine_.seed(seed); }

  // ~~~~Generation~~~~
  /// Generate a distributed key for search, update, or remove
  auto GenKey(bool is_negative = false) -> u64 {
    u64 keyID{GenKeyID()};
    return KeyFromID(keyID, is_negative, true); // Pass apply_shift = true
  }

  /// Generate a new key for run-phase insertion
  auto NextKey() -> u64 {
    u64 keyID{cur_ID_.fetch_add(1)};
    return KeyFromID(keyID, false, false); // Pass is_negative = false and apply_shift = false
  }

  /// Map a key ID to a key for load-phase insertion
  auto KeyFromID(u64 keyID, bool is_negative = false, bool apply_shift = true) -> u64 {
    // Successful lookup IDs are in [1, num_load_]
    // Unsuccessful lookup IDs are outside that range
    // Insertion uses apply_shift = false
    if (apply_shift && shifter_ != 0) {
      keyID = (keyID + shifter_ + num_load_ - 1) % num_load_ + 1;
    }
    if (is_negative) keyID += num_load_ << 3;
    return HashFunction(keyID);  // Hash the key ID to a large integer
  }

  /// Shift the hotspot
  auto ShiftHotness() -> void {
    shifter_ = uniform_int_distribution<u64>{}(random_engine_);
  }

 private:
  // ~~~~Helper functions~~~~
  /// Generate a key ID from the configured distribution
  auto GenKeyID() -> u64 { return zipf_dist_(random_engine_); }
  // ~~~~Data~~~~
  u64 num_load_{};  // Key IDs are in [1, num_load_];
  u64 shifter_{};   // Hotspot offset
  atomic<u64> cur_ID_{};
  // Thread-safe random engines
  inline static thread_local mt19937_64 random_engine_{
      static_cast<u64>(rand())};

  // Available key distributions
  zipfian_int_distribution<u64> zipf_dist_;
};

/// Value generator
// ----------------------------------------------------------------------------
class ValueGen {
 public:
  ValueGen() = default;
  // ~~~~Generation~~~~
  auto GenValue() -> u64 { return uni_dist_(random_engine_); }

 private:
  inline static thread_local mt19937_64 random_engine_{
      static_cast<u64>(rand())};
  uniform_int_distribution<u64> uni_dist_;
};

/// Request generator
// ----------------------------------------------------------------------------
class RequestGen {
 public:
  // ~~~~Construction~~~~
  RequestGen(u64 num_load, double skewness, RequestTypeRatio ratio)
      : key_gen_{num_load, skewness}, value_gen_{}, type_gen_{ratio} {}
  auto SetSeed(u64 seed) -> void {
    key_gen_.SetSeed(seed);
    type_gen_.SetSeed(seed);
  }

  // ~~~~Basic operations~~~~
  /// Generate load requests
  auto GenLoadRequest(u64 keyID) -> Request {
    return {key_gen_.KeyFromID(keyID, false, false), value_gen_.GenValue(),
            RequestType::Insert};
  }

  /// Generate run requests
  auto GenRunRequest() -> Request {
    RequestType type{type_gen_.RandType()};
    switch (type) {
      case RequestType::Read:
        // Search: {distributed key, empty value, type}
        return {key_gen_.GenKey(), 0, type};
        break;
      case RequestType::Update:
        // Update: {distributed key, random value, type}
        return {key_gen_.GenKey(), value_gen_.GenValue(), type};
        break;
      case RequestType::Insert:
        // Insert: {next key ID, random value, type}
        return {key_gen_.NextKey(), value_gen_.GenValue(), type};
        break;
      case RequestType::Remove:
        // Remove: {distributed key, 0, type}
        return {key_gen_.GenKey(), 0, type};
        break;
      case RequestType::NegativeRead:
        // Unsuccessful search: {distributed key outside the loaded range, 0, type}
        return {key_gen_.GenKey(true), 0, type};
        break;
      default:
        AbortHere();
        break;
    }
  }

  // ~~~~Hotspot shifting~~~~
  auto ShiftHotness() -> void { key_gen_.ShiftHotness(); }

 private:
  // ~~~~Helper operations~~~~
  // ~~~~Data~~~~
  KeyGen key_gen_;
  ValueGen value_gen_{};
  RequestTypeGen type_gen_;
};