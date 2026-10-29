#pragma once
#include "common.hpp"
#include <string_view>

namespace TimnahNameSpace {
// Independent completion of sub's tiered-EH organization for the Timnah paper.
// The old SubNameSpace implementation is not modified or reused as shared state.
class Timnah final : public IndexInterface {
 public:
  struct Options {
    u8 fast_depth{12};
    u8 slow_depth{14};
    int fast_node{0};
    int slow_node{2};
    double sample_rate{0.1};
    double intra_multiplier{2.0};
    double cross_multiplier{2.0};
    u64 decay_interval_ms{1000};
    double decay_factor{0.5};
    size_t queue_capacity{1024};
    bool prefault{true};
    bool variable_keys{false};
    bool huge_pages{true};
    // L1 distance between adjacent nonempty round hit-rate vectors; 0 disables.
    double convergence_threshold{0.002};
  };
  struct Diagnostics {
    u64 size{}, fast_hits{}, slow_hits{}, misses{}, samples{}, reorders{};
    u64 promotions{}, demotions{}, slow_splits{}, decay_events{};
    u64 queue_enqueued{}, queue_dropped{}, queue_pending{}, in_flight{};
    u64 queue_busy_dropped{}, queue_full_dropped{}, queue_deduplicated{};
    u64 fast_segments{}, slow_segments{}, fast_bucket_bytes{}, fast_counter_bytes{};
    u64 slow_bucket_bytes{}, slow_counter_bytes{}, variable_key_bytes{}, epoch{};
    bool tracking_active{};
    bool converged{};
    u64 observed_rounds{};
    double last_round_fast_rate{}, last_round_slow_rate{}, convergence_diff{-1};
  };
  struct Inspection {
    bool found{};
    int tier{-1};
    u32 slot{};
    u64 value{}, counter{}, copies{};
  };
  Timnah();
  explicit Timnah(Options options);
  ~Timnah() override;
  // Create, Stat, Stop and destruction require quiescent foreground callers.
  // Stat marks a completed round; Snapshot is a read-only concurrent diagnostic.
  auto Create(const ParamType& param) -> Result override;
  auto Stop() -> void override;
  auto Insert(const u64& key, const u64& value) -> Result override;
  auto Search(const u64& key, u64& value) -> Result override;
  auto Update(const u64& key, const u64& value) -> Result override;
  auto Remove(const u64& key) -> Result override;
  auto Stat() -> string override;

  // Binary-safe variable-length-key extension. Set Options::variable_keys=true;
  // use these methods instead of the u64 interface. Values remain 8 bytes.
  auto InsertBytes(std::string_view key, u64 value) -> Result;
  auto SearchBytes(std::string_view key, u64& value) -> Result;
  auto UpdateBytes(std::string_view key, u64 value) -> Result;
  auto RemoveBytes(std::string_view key) -> Result;

  auto Snapshot() const -> Diagnostics;
  // Diagnostics requiring quiescent foreground callers; worker synchronization
  // is handled internally. Validate scans the table and is intended for tests.
  auto Inspect(u64 key) -> Inspection;
  auto Validate() -> bool;
  auto DrainMaintenance(u64 timeout_ms = 30000) -> bool;
  // Deterministic policy checks; these are never used by the benchmark path.
  auto DebugSetCounter(u64 key, u32 count) -> bool;
  auto DebugMaintain(u64 key) -> bool;
  auto DebugAdvanceDecay(u64 epochs = 1) -> void;
 private:
  class Impl;
  Options options_;
  std::unique_ptr<Impl> impl_;
};
}  // namespace TimnahNameSpace
