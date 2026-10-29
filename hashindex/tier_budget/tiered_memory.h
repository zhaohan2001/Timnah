#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef __linux__
#include <linux/mempolicy.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace TierBudget {

struct Config {
  int fast_node{0};
  int slow_node{2};
  size_t fast_bytes{size_t{256} << 20};
  size_t slow_initial_bytes{size_t{1} << 30};
  size_t slow_growth_bytes{size_t{64} << 20};
  bool prefault{true};
};

struct Stats {
  size_t fast_reserved{};
  size_t slow_reserved{};
  size_t fast_used{};
  size_t slow_used{};
  size_t fast_metadata{};
  size_t slow_metadata{};
  size_t fast_kv{};
  size_t slow_kv{};
  size_t split_fast_kv{};
  size_t split_slow_kv{};
  size_t slow_growth_count{};
};

// Caller serializes allocation and diagnostic calls. Objects must be destroyed
// before Memory. Reservation counts mapped bytes, usage includes 64-byte padding.
// No individual deallocation or fast-tier growth is performed.
class Memory {
 public:
  explicit Memory(Config config) : config_(config), page_size_(PageSize()) {
    if (config.fast_node < -1 || config.slow_node < -1)
      throw std::invalid_argument("TierBudget NUMA node must be -1 or non-negative");
    if (config.fast_bytes % page_size_ != 0)
      throw std::invalid_argument("TierBudget fixed fast capacity must be page-aligned");
    if (!config.slow_initial_bytes || !config.slow_growth_bytes)
      throw std::invalid_argument("TierBudget slow capacities must be positive");
    fast_ = MakeBlock(config.fast_bytes, config.fast_node);
    stats_.fast_reserved = fast_.bytes;
    AddSlowBlock(Round(config.slow_initial_bytes, page_size_), false);
  }
  Memory(const Memory&) = delete;
  Memory& operator=(const Memory&) = delete;
  Memory(Memory&&) = delete;
  Memory& operator=(Memory&&) = delete;
  ~Memory() {
    while (slow_head_) {
      auto* next = slow_head_->next;
      // Move the ownership handle out before destroying its in-mapping header.
      // Its local destructor then unmaps without writing into freed storage.
      Block mapping(std::move(slow_head_->mapping));
      std::destroy_at(slow_head_);
      slow_head_ = next;
    }
  }

  void* Metadata(size_t bytes) {
    bytes = Round(bytes, 64);
    if (!bytes) return nullptr;
    if (Fits(fast_, bytes)) {
      void* p = Take(fast_, bytes);
      stats_.fast_used += bytes;
      stats_.fast_metadata += bytes;
      return p;
    }
    void* p = Slow(bytes);
    stats_.slow_metadata += bytes;
    return p;
  }

  // Static initial placement, independent of key frequency. Remaining fast
  // capacity is eligible only at every fifth ordinal; metadata has first claim.
  void* InitialKV(size_t bytes, size_t ordinal) {
    bytes = Round(bytes, 64);
    if (!bytes) return nullptr;
    if (ordinal % 5 == 0 && Fits(fast_, bytes)) {
      void* p = Take(fast_, bytes);
      stats_.fast_used += bytes;
      stats_.fast_kv += bytes;
      return p;
    }
    void* p = Slow(bytes);
    stats_.slow_kv += bytes;
    return p;
  }

  void* SplitKV(size_t bytes) {
    bytes = Round(bytes, 64);
    if (!bytes) return nullptr;
    void* p = Slow(bytes);
    stats_.slow_kv += bytes;
    stats_.split_slow_kv += bytes;
    return p;
  }

  Stats GetStats() const { return stats_; }

  std::string Describe() const {
    std::ostringstream out;
    out << "fast_reserved=" << stats_.fast_reserved
        << " slow_reserved=" << stats_.slow_reserved
        << " fast_used=" << stats_.fast_used
        << " slow_used=" << stats_.slow_used
        << " fast_metadata=" << stats_.fast_metadata
        << " slow_metadata=" << stats_.slow_metadata
        << " fast_kv=" << stats_.fast_kv
        << " slow_kv=" << stats_.slow_kv
        << " split_fast_kv=" << stats_.split_fast_kv
        << " split_slow_kv=" << stats_.split_slow_kv
        << " slow_growth_count=" << stats_.slow_growth_count;
    return out.str();
  }

  // mbind success verifies the requested policy, not every page's residency.
  // move_pages samples up to five pages per bound block; unavailable/partial
  // results are explicit. This is a diagnostic, never a placement guarantee.
  std::string Placement() const {
    size_t checked = fast_.bound ? 1 : 0;
    for (auto* block = slow_head_; block; block = block->next)
      checked += block->mapping.bound ? 1 : 0;
    std::ostringstream out;
    out << "fast_policy_node=" << config_.fast_node
        << " slow_policy_node=" << config_.slow_node
        << " policy_checked_mappings=" << checked
        << " prefault=" << (config_.prefault ? 1 : 0);
#ifdef __linux__
    std::vector<void*> pages;
    std::vector<int> expected;
    auto sample = [&](const Block& block) {
      if (!block.bound || !block.address) return;
      const size_t populated = config_.prefault ? block.bytes : block.used;
      if (!populated) return;
      const size_t count = (populated + page_size_ - 1) / page_size_;
      size_t previous = std::numeric_limits<size_t>::max();
      for (size_t part = 0; part < 5; ++part) {
        const size_t index = ((count - 1) * part) / 4;
        if (index == previous) continue;
        previous = index;
        pages.push_back(static_cast<std::byte*>(block.address) + index * page_size_);
        expected.push_back(block.node);
      }
    };
    sample(fast_);
    for (auto* block = slow_head_; block; block = block->next) sample(block->mapping);
    if (pages.empty()) return out.str() + " page_query=not_requested page_samples=0";
    std::vector<int> status(pages.size(), -1);
    const long rc = syscall(SYS_move_pages, 0, static_cast<unsigned long>(pages.size()),
                            pages.data(), nullptr, status.data(), 0);
    if (rc < 0) {
      const int error = errno;
      out << " page_query=unavailable errno=" << error
          << " page_samples=" << pages.size();
      return out.str();
    }
    size_t matches = 0, mismatches = 0, unavailable = 0;
    for (size_t i = 0; i < status.size(); ++i) {
      if (status[i] < 0) ++unavailable;
      else if (status[i] == expected[i]) ++matches;
      else ++mismatches;
    }
    out << " page_query=sampled page_samples=" << pages.size()
        << " page_matches=" << matches << " page_mismatches=" << mismatches
        << " page_unavailable=" << unavailable
        << " all_sampled_pages_match="
        << (matches == pages.size() && !unavailable && !mismatches ? 1 : 0);
#else
    out << " page_query=unavailable reason=non_linux page_samples=0";
#endif
    return out.str();
  }

 private:
  struct Block {
    void* address{};
    size_t bytes{};
    size_t used{};
    int node{-1};
    bool bound{};
    Block() = default;
    Block(void* p, size_t length, int n, bool b)
        : address(p), bytes(length), node(n), bound(b) {}
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    Block(Block&& other) noexcept { Swap(other); }
    Block& operator=(Block&& other) noexcept {
      if (this != &other) { Reset(); Swap(other); }
      return *this;
    }
    ~Block() { Reset(); }
    void Swap(Block& other) noexcept {
      std::swap(address, other.address);
      std::swap(bytes, other.bytes);
      std::swap(used, other.used);
      std::swap(node, other.node);
      std::swap(bound, other.bound);
    }
    void Reset() noexcept {
      if (address) {
#ifdef __linux__
        munmap(address, bytes);
#else
        std::free(address);
#endif
      }
      address = nullptr;
      bytes = used = 0;
      node = -1;
      bound = false;
    }
  };

  struct alignas(64) SlowBlock {
    SlowBlock* next{};
    Block mapping;
  };
  static_assert(sizeof(SlowBlock) == 64);

  static size_t PageSize() {
#ifdef __linux__
    const long value = sysconf(_SC_PAGESIZE);
    if (value <= 0) throw std::runtime_error("TierBudget cannot determine page size");
    return static_cast<size_t>(value);
#else
    return 4096;
#endif
  }
  static size_t Round(size_t bytes, size_t alignment) {
    if (!bytes) return 0;
    const size_t remainder = bytes % alignment;
    if (!remainder) return bytes;
    const size_t padding = alignment - remainder;
    if (bytes > std::numeric_limits<size_t>::max() - padding) throw std::bad_alloc();
    return bytes + padding;
  }
  static bool Fits(const Block& block, size_t bytes) {
    return block.address && bytes <= block.bytes - block.used;
  }
  static void* Take(Block& block, size_t bytes) {
    auto* p = static_cast<std::byte*>(block.address) + block.used;
    block.used += bytes;
    return p;
  }
  Block MakeBlock(size_t bytes, int node) const {
    if (!bytes) return {};
#ifdef __linux__
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "TierBudget mmap");
    Block block(p, bytes, node, false);
    if (node >= 0) {
      constexpr size_t bits = sizeof(unsigned long) * 8;
      std::vector<unsigned long> mask(static_cast<size_t>(node) / bits + 1, 0);
      mask[static_cast<size_t>(node) / bits] |= 1ul << (static_cast<size_t>(node) % bits);
      // Linux mbind consumes maxnode-1 significant bits; pass the full bitmap
      // width plus one so its highest set bit is included (also for node0).
      const long rc = syscall(SYS_mbind, p, bytes, MPOL_BIND, mask.data(),
                              static_cast<unsigned long>(mask.size() * bits) + 1, 0);
      if (rc != 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(),
            "TierBudget mbind node " + std::to_string(node));
      }
      block.bound = true;
    }
#else
    if (node >= 0) throw std::invalid_argument("TierBudget NUMA binding requires Linux");
    void* p = std::aligned_alloc(64, Round(bytes, 64));
    if (!p) throw std::bad_alloc();
    Block block(p, bytes, node, false);
#endif
    if (config_.prefault) {
      volatile unsigned char* touch = static_cast<volatile unsigned char*>(block.address);
      for (size_t offset = 0; offset < bytes; offset += page_size_) touch[offset] = 0;
    }
    return block;
  }
  void AddSlowBlock(size_t bytes, bool growth) {
    if (bytes < sizeof(SlowBlock) ||
        stats_.slow_reserved > std::numeric_limits<size_t>::max() - bytes)
      throw std::bad_alloc();
    Block mapping = MakeBlock(bytes, config_.slow_node);
    auto* header = std::construct_at(static_cast<SlowBlock*>(mapping.address));
    header->mapping = std::move(mapping);
    header->mapping.used = sizeof(SlowBlock);
    if (slow_tail_) slow_tail_->next = header;
    else slow_head_ = header;
    slow_tail_ = header;
    stats_.slow_reserved += bytes;
    stats_.slow_used += sizeof(SlowBlock);
    stats_.slow_metadata += sizeof(SlowBlock);
    if (growth) ++stats_.slow_growth_count;
  }
  void* Slow(size_t bytes) {
    if (!Fits(slow_tail_->mapping, bytes)) {
      if (bytes > std::numeric_limits<size_t>::max() - sizeof(SlowBlock))
        throw std::bad_alloc();
      const auto amount = Round(
          std::max(config_.slow_growth_bytes, bytes + sizeof(SlowBlock)), page_size_);
      AddSlowBlock(amount, true);
    }
    void* p = Take(slow_tail_->mapping, bytes);
    stats_.slow_used += bytes;
    return p;
  }

  Config config_;
  size_t page_size_;
  Block fast_;
  SlowBlock* slow_head_{};
  SlowBlock* slow_tail_{};
  Stats stats_;
};

// Persistent index bookkeeping vectors use the same budget. Deallocation is a
// no-op because old storage remains valid until Memory is destroyed.
template <typename T>
class Allocator {
 public:
  using value_type = T;
  using size_type = size_t;
  using difference_type = std::ptrdiff_t;
  using propagate_on_container_move_assignment = std::true_type;
  using is_always_equal = std::false_type;
  template <typename U> struct rebind { using other = Allocator<U>; };

  Allocator() noexcept = default;
  explicit Allocator(Memory* memory) noexcept : memory_(memory) {}
  explicit Allocator(Memory& memory) noexcept : memory_(&memory) {}
  template <typename U>
  Allocator(const Allocator<U>& other) noexcept : memory_(other.owner()) {}

  T* allocate(size_t n) {
    static_assert(alignof(T) <= 64, "TierBudget supports alignments up to 64 bytes");
    if (!memory_ || n > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_alloc();
    return static_cast<T*>(memory_->Metadata(n * sizeof(T)));
  }
  void deallocate(T*, size_t) noexcept {}
  Memory* owner() const noexcept { return memory_; }
  template <typename U>
  bool operator==(const Allocator<U>& other) const noexcept { return memory_ == other.owner(); }
  template <typename U>
  bool operator!=(const Allocator<U>& other) const noexcept { return !(*this == other); }
 private:
  Memory* memory_{};
};

}  // namespace TierBudget
