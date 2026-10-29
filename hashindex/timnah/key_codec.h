#pragma once

#include "common.hpp"
#include "storage.h"

#include <mutex>
#include <string_view>

namespace TimnahNameSpace::detail {

// Optional variable-length-key indirection for the paper's Section 4.10.
// Every token stores a 48-bit immutable Header pointer and a 16-bit fingerprint.
// The u64 benchmark interface does not use this codec unless explicitly enabled.
class KeyCodec {
 public:
  struct Header {
    u64 hash;
    size_t size;
    const char* bytes;
  };

  // A query references caller-owned bytes only for the duration of that call.
  // Compute Token() after MakeQuery returns: encoding a pointer in MakeQuery
  // itself would point at a temporary Header if the returned Query were moved.
  struct Query {
    Header header;
    auto Token() const -> u64;
  };

  explicit KeyCodec(int node, bool prefault, bool huge_pages = true);
  KeyCodec(const KeyCodec&) = delete;
  auto operator=(const KeyCodec&) -> KeyCodec& = delete;

  static auto MakeQuery(std::string_view key) -> Query;
  auto MakeStored(std::string_view key) -> u64;
  // Zero-pointer tokens are tolerated for speculative reads of invalidated
  // slots: Hash returns zero and Equal returns false, even for two null tokens.
  static auto Hash(u64 token) -> u64;
  static auto Equal(u64 left, u64 right) -> bool;
  auto UsedBytes() const -> size_t;

 private:
  static constexpr u64 kPointerMask = (u64{1} << 48) - 1;
  static auto HashBytes(std::string_view key) -> u64;
  static auto Encode(const Header* header) -> u64;
  static auto Decode(u64 token) -> const Header*;

  mutable std::mutex mutex_;
  Arena arena_;
};

}  // namespace TimnahNameSpace::detail
