#ifndef TIMNAH_COMPACT_KEY_DICTIONARY_HPP
#define TIMNAH_COMPACT_KEY_DICTIONARY_HPP
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

template<class Hash = std::hash<std::string_view>>
class ExactKeyDictionary {
  using u64 = std::uint64_t;
  static constexpr std::size_t arena_bytes = 1U << 24;
  static constexpr std::size_t offset_count = 1U << 20;
  static constexpr u64 id_mask = (u64(1) << 56) - 1;
  std::vector<std::unique_ptr<char[]>> arenas_;
  std::vector<std::unique_ptr<u64[]>> offsets_;
  std::size_t capacity_ = 1024;
  std::unique_ptr<u64[]> table_;
  std::size_t tail_ = arena_bytes;
  u64 size_ = 0;
  Hash hash_;

  std::string_view Key(u64 id) const {
    const auto position = offsets_[(id - 1) / offset_count][(id - 1) % offset_count];
    const char* p = arenas_[position / arena_bytes].get() + position % arena_bytes;
    std::uint32_t length;
    std::memcpy(&length, p, sizeof(length));
    return {p + sizeof(length), length};
  }
  static u64 Cell(u64 id, u64 hash) { return id | (hash & ~id_mask); }
  void Grow() {
    if (capacity_ > std::numeric_limits<std::size_t>::max() / 2 / sizeof(u64))
      throw std::runtime_error("Key dictionary capacity overflow");
    const auto next = capacity_ * 2;
    auto table = std::make_unique<u64[]>(next);
    for (u64 id = 1; id <= size_; ++id) {
      const u64 hash = hash_(Key(id));
      auto slot = hash & (next - 1);
      while (table[slot]) slot = (slot + 1) & (next - 1);
      table[slot] = Cell(id, hash);
    }
    table_.swap(table);
    capacity_ = next;
  }
public:
  ExactKeyDictionary() : table_(std::make_unique<u64[]>(capacity_)) {}
  u64 size() const { return size_; }
  u64 Find(std::string_view key) const {
    const u64 hash = hash_(key);
    auto slot = hash & (capacity_ - 1);
    while (const u64 cell = table_[slot]) {
      if ((cell & ~id_mask) == (hash & ~id_mask) && Key(cell & id_mask) == key)
        return cell & id_mask;
      slot = (slot + 1) & (capacity_ - 1);
    }
    return 0;
  }
  u64 Intern(std::string_view key) {
    if (const auto id = Find(key)) return id;
    if (size_ == id_mask) throw std::runtime_error("Key dictionary ID overflow");
    if (key.size() > arena_bytes - sizeof(std::uint32_t))
      throw std::runtime_error("Trace key exceeds dictionary block size");
    if (size_ + 1 >= capacity_ * 7 / 10) Grow();
    const auto bytes = key.size() + sizeof(std::uint32_t);
    if (bytes > arena_bytes - tail_) {
      arenas_.emplace_back(new char[arena_bytes]);
      tail_ = 0;
    }
    if (size_ % offset_count == 0) offsets_.emplace_back(new u64[offset_count]);
    const auto position = u64(arenas_.size() - 1) * arena_bytes + tail_;
    char* p = arenas_.back().get() + tail_;
    const auto length = static_cast<std::uint32_t>(key.size());
    std::memcpy(p, &length, sizeof(length));
    std::memcpy(p + sizeof(length), key.data(), key.size());
    tail_ += bytes;
    offsets_[size_ / offset_count][size_ % offset_count] = position;
    const u64 id = ++size_;
    const u64 hash = hash_(key);
    auto slot = hash & (capacity_ - 1);
    while (table_[slot]) slot = (slot + 1) & (capacity_ - 1);
    table_[slot] = Cell(id, hash);
    return id;
  }
};
#endif
