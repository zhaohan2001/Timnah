#pragma once
#include "benchmark/synthetic/profile.hpp"
#include <cstdint>
namespace real_trace {
struct Info {
  uint64_t records{}, keys{}, reads{}, updates{}, preload_keys{}, source_keys{};
  std::string source_sha256, path, scope;
};
auto Initialize(const std::string& code, uint64_t maximum_keys) -> Info;
auto Current() -> const Info&;
auto ProfileFor(const std::string& code) -> synthetic::Profile;
}
