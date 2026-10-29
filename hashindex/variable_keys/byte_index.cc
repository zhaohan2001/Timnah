#include "byte_index.hpp"

#include "chash/chash.h"
#include "dash/dash.h"
#include "ehcache/ehcache.h"
#include "timnah/key_codec.h"
#include "timnah/timnah.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

DECLARE_uint32(timnah_fast_depth);
DECLARE_uint32(timnah_slow_depth);
DECLARE_int32(timnah_fast_node);
DECLARE_int32(timnah_slow_node);
DECLARE_double(timnah_sample_rate);
DECLARE_double(timnah_intra_multiplier);
DECLARE_double(timnah_cross_multiplier);
DECLARE_uint64(timnah_decay_ms);
DECLARE_double(timnah_decay_factor);
DECLARE_uint64(timnah_queue_capacity);
DECLARE_bool(timnah_huge_pages);
DECLARE_double(timnah_convergence_threshold);
DECLARE_int32(chash_slow_node);
DECLARE_int32(dash_slow_node);
DECLARE_int32(ehcache_slow_node);

namespace VariableKeys {
namespace {
using Codec = TimnahNameSpace::detail::KeyCodec;

auto CanonicalName(std::string_view input) -> std::string {
  std::string name(input);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char value) { return std::tolower(value); });
  if (name == "timnah") return "Timnah";
  if (name == "chash") return "CHash";
  if (name == "dash" || name == "dash-eh") return "Dash";
  if (name == "ehcache" || name == "eh+cache") return "EHCache";
  throw std::invalid_argument("Variable-key mode supports Timnah, Dash, CHash and EHCache");
}

class TimnahIndex final : public Index {
 public:
  TimnahIndex(TimnahNameSpace::Timnah::Options options,
              const IndexInterface::ParamType& param)
      : table_(options), key_node_(options.slow_node) {
    if (table_.Create(param) != Result::SUCCESS)
      throw std::runtime_error("Could not create variable-key Timnah");
  }
  ~TimnahIndex() override { table_.Stop(); }
  auto Insert(std::string_view key, u64 value) -> Result override {
    return table_.InsertBytes(key, value);
  }
  auto Search(std::string_view key, u64& value) -> Result override {
    return table_.SearchBytes(key, value);
  }
  auto Update(std::string_view key, u64 value) -> Result override {
    return table_.UpdateBytes(key, value);
  }
  auto Remove(std::string_view key) -> Result override {
    return table_.RemoveBytes(key);
  }
  auto Stat() -> std::string override {
    auto text = table_.Stat();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text + fmt::format(" key_payload_node={}\n", key_node_);
  }
  void Stop() override { table_.Stop(); }
  auto KeyBytes() const -> std::size_t override {
    return table_.Snapshot().variable_key_bytes;
  }
  auto KeyNode() const -> int override { return key_node_; }

 private:
  TimnahNameSpace::Timnah table_;
  int key_node_;
};

template <class Table>
class TokenIndex final : public Index {
 public:
  TokenIndex(std::unique_ptr<Table> table, int key_node, bool prefault,
             const IndexInterface::ParamType& param)
      : codec_(key_node, prefault), table_(std::move(table)), key_node_(key_node) {
    if (table_->Create(param) != Result::SUCCESS)
      throw std::runtime_error("Could not create variable-key index");
  }
  ~TokenIndex() override { table_->Stop(); }
  auto Insert(std::string_view key, u64 value) -> Result override {
    // Retain immutable allocations until destruction, including a failed
    // concurrent duplicate insertion, so optimistic readers never see freed keys.
    const auto token = codec_.MakeStored(key);
    return table_->Insert(token, value);
  }
  auto Search(std::string_view key, u64& value) -> Result override {
    const auto query = Codec::MakeQuery(key);
    return table_->Search(query.Token(), value);
  }
  auto Update(std::string_view key, u64 value) -> Result override {
    const auto query = Codec::MakeQuery(key);
    return table_->Update(query.Token(), value);
  }
  auto Remove(std::string_view key) -> Result override {
    const auto query = Codec::MakeQuery(key);
    return table_->Remove(query.Token());
  }
  auto Stat() -> std::string override {
    auto text = table_->Stat();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text + fmt::format(" variable_key_bytes={} key_arena_used_bytes={} "
                              "key_arena_reserved_bytes={} key_payload_node={}\n",
                               codec_.UsedBytes(), codec_.UsedBytes(),
                               codec_.ReservedBytes(), key_node_);
  }
  void Stop() override { table_->Stop(); }
  auto KeyBytes() const -> std::size_t override { return codec_.UsedBytes(); }
  auto KeyNode() const -> int override { return key_node_; }

 private:
  // Destruction order matters: table-owned tokens must vanish before their
  // immutable key arena. The codec therefore precedes the table member.
  Codec codec_;
  std::unique_ptr<Table> table_;
  int key_node_;
};

auto TimnahFlags() -> TimnahNameSpace::Timnah::Options {
  if (FLAGS_timnah_fast_depth > 24 || FLAGS_timnah_slow_depth > 24)
    throw std::invalid_argument("Timnah directory depths must be in [1,24]");
  return {static_cast<u8>(FLAGS_timnah_fast_depth),
          static_cast<u8>(FLAGS_timnah_slow_depth), FLAGS_timnah_fast_node,
          FLAGS_timnah_slow_node, FLAGS_timnah_sample_rate,
          FLAGS_timnah_intra_multiplier, FLAGS_timnah_cross_multiplier,
          FLAGS_timnah_decay_ms, FLAGS_timnah_decay_factor,
          FLAGS_timnah_queue_capacity, true, true, FLAGS_timnah_huge_pages,
          FLAGS_timnah_convergence_threshold};
}

template <class Table>
auto TestTable(const IndexInterface::ParamType& param, const TestOptions& test)
    -> std::unique_ptr<Index> {
  typename Table::Options options;
  options.initial_depth = test.initial_depth;
  options.dram_node = test.fast_node;
  options.slow_node = test.slow_node;
  options.fast_bytes = test.fast_bytes;
  options.slow_initial_bytes = test.slow_initial_bytes;
  options.slow_growth_bytes = test.slow_growth_bytes;
  if constexpr (std::is_same_v<Table, EHCacheNameSpace::EHCache>)
    options.sample_rate = test.sample_rate;
  return std::make_unique<TokenIndex<Table>>(std::make_unique<Table>(options),
                                             test.slow_node, test.prefault, param);
}
}  // namespace

auto MakeIndex(std::string_view input, const IndexInterface::ParamType& param)
    -> std::unique_ptr<Index> {
  const auto name = CanonicalName(input);
  if (name == "Timnah") return std::make_unique<TimnahIndex>(TimnahFlags(), param);
  if (name == "CHash")
    return std::make_unique<TokenIndex<CHashNameSpace::CHash>>(
        std::make_unique<CHashNameSpace::CHash>(), FLAGS_chash_slow_node, true, param);
  if (name == "Dash")
    return std::make_unique<TokenIndex<DashNameSpace::Dash>>(
        std::make_unique<DashNameSpace::Dash>(), FLAGS_dash_slow_node, true, param);
  return std::make_unique<TokenIndex<EHCacheNameSpace::EHCache>>(
      std::make_unique<EHCacheNameSpace::EHCache>(), FLAGS_ehcache_slow_node, true, param);
}

auto MakeTestIndex(std::string_view input, const IndexInterface::ParamType& param,
                   const TestOptions& test) -> std::unique_ptr<Index> {
  const auto name = CanonicalName(input);
  if (name == "Timnah") {
    TimnahNameSpace::Timnah::Options options;
    options.fast_depth = test.timnah_fast_depth;
    options.slow_depth = test.timnah_slow_depth;
    options.fast_node = test.fast_node;
    options.slow_node = test.slow_node;
    options.prefault = test.prefault;
    options.variable_keys = true;
    options.huge_pages = false;
    options.sample_rate = test.sample_rate;
    options.convergence_threshold = test.convergence_threshold;
    return std::make_unique<TimnahIndex>(options, param);
  }
  if (name == "CHash") return TestTable<CHashNameSpace::CHash>(param, test);
  if (name == "Dash") return TestTable<DashNameSpace::Dash>(param, test);
  return TestTable<EHCacheNameSpace::EHCache>(param, test);
}

}  // namespace VariableKeys
