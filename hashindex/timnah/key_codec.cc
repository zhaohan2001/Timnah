#include "key_codec.h"

#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace TimnahNameSpace::detail {

KeyCodec::KeyCodec(int node, bool prefault, bool huge_pages)
    : arena_(node, size_t{64} << 20, prefault, huge_pages) {}

auto KeyCodec::HashBytes(std::string_view key) -> u64 {
  // Explicit little-endian assembly makes the result independent of host
  // endianness, alignment and std::hash's implementation-defined behavior.
  // Reuse the project's splitmix64-based HashFunction for every chunk.
  auto hash = HashFunction(static_cast<u64>(key.size()) ^
                           u64{0x54494d4e41484b45});
  size_t offset = 0;
  while (key.size() - offset >= sizeof(u64)) {
    u64 word = 0;
    for (u32 byte = 0; byte < sizeof(u64); ++byte)
      word |= u64{static_cast<unsigned char>(key[offset + byte])} << (8 * byte);
    hash = HashFunction(hash ^ word);
    offset += sizeof(u64);
  }
  u64 tail = 0;
  for (size_t byte = 0; offset + byte < key.size(); ++byte)
    tail |= u64{static_cast<unsigned char>(key[offset + byte])} << (8 * byte);
  return HashFunction(hash ^ tail ^
                      (static_cast<u64>(key.size() - offset) << 56));
}

auto KeyCodec::MakeQuery(std::string_view key) -> Query {
  return Query{Header{HashBytes(key), key.size(), key.data()}};
}

auto KeyCodec::Encode(const Header* header) -> u64 {
  const auto address = reinterpret_cast<std::uintptr_t>(header);
  if (!address || static_cast<u64>(address) > kPointerMask)
    throw std::runtime_error(
        "Timnah variable-key Header address does not fit its 48-bit pointer encoding");
  const auto fingerprint = (header->hash >> 8) & 0xffff;
  return static_cast<u64>(address) | (fingerprint << 48);
}

auto KeyCodec::Decode(u64 token) -> const Header* {
  const auto address = token & kPointerMask;
  if (!address)
    throw std::invalid_argument("Timnah variable-key token has a null Header pointer");
  return reinterpret_cast<const Header*>(static_cast<std::uintptr_t>(address));
}

auto KeyCodec::Query::Token() const -> u64 { return KeyCodec::Encode(&header); }

auto KeyCodec::MakeStored(std::string_view key) -> u64 {
  if (key.size() > std::numeric_limits<size_t>::max() - sizeof(Header))
    throw std::length_error("Timnah variable-key allocation size overflows size_t");
  const auto hash = HashBytes(key);
  std::lock_guard lock(mutex_);
  auto* allocation = static_cast<std::byte*>(
      arena_.Allocate(sizeof(Header) + key.size(), 64));
  auto* bytes = reinterpret_cast<char*>(allocation + sizeof(Header));
  if (!key.empty()) std::memcpy(bytes, key.data(), key.size());
  auto* header = std::construct_at(reinterpret_cast<Header*>(allocation),
                                 Header{hash, key.size(), bytes});
  // Deletions intentionally retain immutable headers and bytes until the index
  // is destroyed, so an optimistic reader never dereferences freed key storage.
  return Encode(header);
}

auto KeyCodec::Hash(u64 token) -> u64 {
  // An optimistic reader can sample an invalidated slot before its enclosing
  // Key-version check rejects the snapshot. Structural movement may put zero
  // there, so never dereference a null speculative token.
  return (token & kPointerMask) ? Decode(token)->hash : 0;
}

auto KeyCodec::Equal(u64 left, u64 right) -> bool {
  if (!(left & kPointerMask) || !(right & kPointerMask)) return false;
  if (left == right) return true;
  if ((left >> 48) != (right >> 48)) return false;
  const auto* a = Decode(left);
  const auto* b = Decode(right);
  if (a->hash != b->hash || a->size != b->size) return false;
  return !a->size || std::memcmp(a->bytes, b->bytes, a->size) == 0;
}

auto KeyCodec::UsedBytes() const -> size_t {
  std::lock_guard lock(mutex_);
  return arena_.UsedBytes();
}

}  // namespace TimnahNameSpace::detail
