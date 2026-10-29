#pragma once

#include <string>

namespace synthetic {

// Observed requests adapted to index reads and overwrites.
// Cache conditional-write semantics and block I/O sizes are outside this model.
struct Profile {
  std::string code;
  std::string name;
  std::string source;
  double read_ratio;
  double update_ratio;
  double alpha;
};

// Preset codes are lowercase. Unknown codes throw std::invalid_argument.
auto GetProfile(const std::string& code) -> Profile;
auto IsProfileCode(const std::string& code) -> bool;

}  // namespace synthetic
