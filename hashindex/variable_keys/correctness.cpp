#include "byte_index.hpp"
#include "timnah/key_codec.h"

#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <regex>
#include <thread>
#include <unordered_map>

namespace {
using VariableKeys::Index;
using Codec = TimnahNameSpace::detail::KeyCodec;

void Require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}

auto Key(u64 id) -> std::string {
  // Equal first halves deliberately expose accidental 8-byte truncation.
  std::string key("prefix\0\0", 8);
  for (unsigned i = 0; i < 8; ++i) key.push_back(static_cast<char>(id >> (8 * i)));
  return key;
}

void CheckValue(Index& index, std::string_view key, u64 expected) {
  u64 value = ~expected;
  Require(index.Search(key, value) == Result::SUCCESS && value == expected,
          "A full-key lookup returned the wrong result or value");
}

auto FingerprintCollision() -> std::pair<std::string, std::string> {
  std::unordered_map<u16, std::string> observed;
  for (u64 id = 1; id < 1000000; ++id) {
    const auto key = Key((u64{1} << 40) + id);
    const auto query = Codec::MakeQuery(key);
    const auto fingerprint = static_cast<u16>((query.header.hash >> 8) & 0xffff);
    if (auto at = observed.find(fingerprint); at != observed.end())
      return {at->second, key};
    observed.emplace(fingerprint, key);
  }
  throw std::runtime_error("Could not construct a deterministic fingerprint collision");
}

void CodecChecks() {
  Codec codec(-1, false, false);
  std::string owner("a\0binary\0key", 12);
  const auto token = codec.MakeStored(owner);
  const auto copy = owner;
  std::fill(owner.begin(), owner.end(), '!');
  const auto same = Codec::MakeQuery(copy);
  Require(Codec::Equal(token, same.Token()), "Stored bytes alias the caller buffer");
  Require((token >> 48) == ((same.header.hash >> 8) & 0xffff),
          "The token does not embed its 16-bit fingerprint");
  Require(!Codec::Equal(0, token) && !Codec::Equal(token, 0),
          "Null speculative tokens must not be dereferenced");
  // Force equal 64-bit hashes as well as equal fingerprints. Exact equality
  // must still check the byte length and contents rather than trust a hash.
  std::string wrong(copy.size(), '?');
  auto forged = Codec::MakeQuery(wrong);
  forged.header.hash = same.header.hash;
  Require(!Codec::Equal(token, forged.Token()), "Equality trusted a hash collision");
  auto short_query = Codec::MakeQuery(std::string_view(copy).substr(0, copy.size() - 1));
  short_query.header.hash = same.header.hash;
  Require(!Codec::Equal(token, short_query.Token()), "Equality ignored the key length");
  Require(codec.UsedBytes() >= sizeof(Codec::Header) + copy.size(),
          "Key arena accounting omitted stored headers or bytes");
}

void BasicChecks(Index& index, const std::pair<std::string, std::string>& collision) {
  const std::vector<std::string> keys{
      {}, std::string("\0", 1), "same", std::string("same\0", 5),
      std::string("same\0suffix", 11), Key(1), Key(2),
      std::string(31, 'x'), std::string(32, 'x'), std::string(257, '\0'),
      std::string(4096, 'z')};
  for (std::size_t i = 0; i < keys.size(); ++i) {
    std::string owner = keys[i];
    Require(index.Insert(owner, i + 100) == Result::SUCCESS, "Initial insertion failed");
    std::fill(owner.begin(), owner.end(), static_cast<char>(0xff));
    CheckValue(index, keys[i], i + 100);
    Require(index.Insert(keys[i], 999) != Result::SUCCESS, "Duplicate insertion succeeded");
    CheckValue(index, keys[i], i + 100);
    Require(index.Update(keys[i], i + 200) == Result::SUCCESS, "Update failed");
    CheckValue(index, keys[i], i + 200);
  }
  Require(index.Insert(collision.first, 7001) == Result::SUCCESS,
          "First fingerprint-collision key was rejected");
  Require(index.Insert(collision.second, 7002) == Result::SUCCESS,
          "Distinct fingerprint-collision key was treated as a duplicate");
  CheckValue(index, collision.first, 7001);
  CheckValue(index, collision.second, 7002);
  for (const auto& key : keys) {
    Require(index.Remove(key) == Result::SUCCESS, "Removal failed");
    u64 value{};
    Require(index.Search(key, value) != Result::SUCCESS, "Removed key remained visible");
    Require(index.Update(key, 42) != Result::SUCCESS, "Update of an absent key succeeded");
    Require(index.Remove(key) != Result::SUCCESS, "Removal of an absent key succeeded");
  }
  Require(index.Remove(collision.first) == Result::SUCCESS &&
              index.Remove(collision.second) == Result::SUCCESS,
          "Collision-key removal failed");
}

void ConcurrentDuplicate(Index& index) {
  constexpr unsigned count = 16;
  std::atomic<unsigned> ready{0}, winners{0}, errors{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  const auto key = Key(9000000);
  for (unsigned worker = 0; worker < count; ++worker) workers.emplace_back([&, worker] {
    ready.fetch_add(1);
    while (!start.load()) std::this_thread::yield();
    try {
      std::string local = key;
      if (index.Insert(local, worker + 1) == Result::SUCCESS) winners.fetch_add(1);
      std::fill(local.begin(), local.end(), 'q');
    } catch (...) { errors.fetch_add(1); }
  });
  while (ready.load() != count) std::this_thread::yield();
  start.store(true);
  for (auto& worker : workers) worker.join();
  Require(errors.load() == 0 && winners.load() == 1,
          "Concurrent duplicate insertion did not have exactly one winner");
  u64 value{};
  Require(index.Search(key, value) == Result::SUCCESS && value >= 1 && value <= count,
          "Concurrent duplicate insertion lost its owned key or value");
  Require(index.Remove(key) == Result::SUCCESS, "Concurrent duplicate key removal failed");
}

void SplitAndOwnership(Index& index) {
  constexpr u64 records = 50000;
  for (u64 id = 100000; id < 100000 + records; ++id) {
    std::string owner = Key(id);
    Require(index.Insert(owner, id * 3) == Result::SUCCESS, "Split-heavy insertion failed");
    std::fill(owner.begin(), owner.end(), static_cast<char>(0xa5));
  }
  // With a 1 MiB EH+cache budget this exceeds the resident cache capacity.
  // Read misses therefore admit canonical backing tokens, never stack queries.
  for (u64 id = 100000; id < 100000 + records; ++id) {
    auto query = Key(id);
    CheckValue(index, query, id * 3);
    std::fill(query.begin(), query.end(), 'Q');
    std::array<char, 1024> overwrite{};
    std::fill(overwrite.begin(), overwrite.end(), 'S');
  }
  for (u64 id = 100000 + records; id-- > 100000;)
    CheckValue(index, Key(id), id * 3);
  Require(index.KeyBytes() >= records * (sizeof(Codec::Header) + 16),
          "Variable-key storage accounting is incomplete");
  const auto stats = index.Stat();
  std::smatch split;
  Require(std::regex_search(stats, split,
                           std::regex(R"((?:^|\s)(?:slow_splits|eh_splits|splits)=(\d+))")) &&
              std::stoull(split[1]) > 0,
          "The split-heavy fixture did not exercise a growing table");
}

void ConcurrentCrud(Index& index) {
  constexpr unsigned threads = 8;
  constexpr u64 each = 800;
  std::atomic<unsigned> errors{0};
  std::vector<std::thread> workers;
  for (unsigned worker = 0; worker < threads; ++worker) workers.emplace_back([&, worker] {
    try {
      for (u64 i = 0; i < each; ++i) {
        const u64 id = 1000000 + worker * each + i;
        auto key = Key(id);
        if (index.Insert(key, id) != Result::SUCCESS) ++errors;
        u64 value{};
        if (index.Search(Key(id), value) != Result::SUCCESS || value != id) ++errors;
        if (index.Update(Key(id), id + 17) != Result::SUCCESS) ++errors;
        if (index.Search(Key(id), value) != Result::SUCCESS || value != id + 17) ++errors;
        if (i % 2 == 0) {
          if (index.Remove(Key(id)) != Result::SUCCESS) ++errors;
          if (index.Search(Key(id), value) == Result::SUCCESS) ++errors;
        }
      }
    } catch (...) { ++errors; }
  });
  for (auto& worker : workers) worker.join();
  Require(errors.load() == 0, "Concurrent full-key CRUD failed");
  for (unsigned worker = 0; worker < threads; ++worker)
    for (u64 i = 1; i < each; i += 2) {
      const u64 id = 1000000 + worker * each + i;
      CheckValue(index, Key(id), id + 17);
    }
}
}  // namespace

int main() {
  try {
    CodecChecks();
    const auto collision = FingerprintCollision();
    for (const char* name : {"Timnah", "Dash", "CHash", "EHCache"}) {
      VariableKeys::TestOptions options;
      options.fast_bytes = 1ull << 20;
      IndexInterface::ParamType param{};
      param.num_threads = 16;
      param.num_bg = std::string_view(name) == "Timnah" ? 1 : 0;
      auto index = VariableKeys::MakeTestIndex(name, param, options);
      BasicChecks(*index, collision);
      ConcurrentDuplicate(*index);
      SplitAndOwnership(*index);
      ConcurrentCrud(*index);
      index->Stop();
      std::cout << name << " variable-key correctness passed" << std::endl;
    }
    std::cout << "All four variable-key indexes passed exact-byte correctness checks." << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Variable-key correctness failure: " << error.what() << std::endl;
    return 1;
  }
}
