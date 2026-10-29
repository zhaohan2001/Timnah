// Common allocator: use a NUMA allocator on Linux and a regular allocator otherwise.
// ----------------------------------------------------------------
#pragma once
#include "headers.hpp"

DECLARE_double(fast_mem_ratio);
// TODO: Design a memory alignment scheme.
inline auto AllocOnNode(size_t size, int node) -> void* {
  void* addr = nullptr;
#ifdef __linux__
  // addr = numa_alloc_onnode(size, node);
  // Allocate according to the ratio.
  static double fast_memory_rate{FLAGS_fast_mem_ratio};
  static double fast_size{64.0}, slow_size{64.0};
  if (fast_size / (fast_size + slow_size) < fast_memory_rate) {
    addr = numa_alloc_onnode(size, 0);
    fast_size += size;
  } else {
    addr = numa_alloc_onnode(size, node);
    slow_size += size;
  }
#else
  addr = malloc(size);
#endif
  return addr;
}

inline auto FreeOnNode(void* addr, size_t size) -> void {
#ifdef __linux__
  numa_free(addr, size);
#else
  free(addr);
#endif
}