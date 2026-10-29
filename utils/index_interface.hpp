// Index interface definition
#pragma once

#include "headers.hpp"

/**
 * Abstraction of an index data structure
 * Lifecycle interface: create, stop.
 * Access interface: insert, search, update, remove.
 */
class IndexInterface {
 public:
  /// Dynamic configuration parameters
  struct ParamType {
    u64 dram_size{};
    u64 num_threads{};
    u64 num_bg{};
  };

  /// Lifecycle interface
  virtual auto Create(const IndexInterface::ParamType& param) -> Result = 0;
  virtual void Stop() = 0;
  virtual ~IndexInterface() = default;

  /// Access interface
  virtual auto Insert(const u64& key, const u64& value) -> Result = 0;
  virtual auto Search(const u64& key, u64& value) -> Result = 0;
  virtual auto Update(const u64& key, const u64& value) -> Result = 0;
  virtual auto Remove(const u64& key) -> Result = 0;

  /// Experiment statistics
  virtual auto Stat() -> string { return ""; }
  // virtual u64 GetLoadFactor() = 0;
  // virtual u64 GetUsedMemoryInfo() = 0;
};