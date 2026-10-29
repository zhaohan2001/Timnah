// Select an executable before the unchanged scalar benchmark starts.
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "sha256.hpp"

#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef HASHING_VARIABLE_EXECUTABLE
#define HASHING_VARIABLE_EXECUTABLE ""
#endif
#ifndef HASHING_SCALAR_SHA256
#define HASHING_SCALAR_SHA256 ""
#endif

namespace {
auto ExecutablePath() -> std::filesystem::path {
#ifdef __APPLE__
  std::vector<char> buffer(4096);
  uint32_t bytes = static_cast<uint32_t>(buffer.size());
  if (_NSGetExecutablePath(buffer.data(), &bytes) != 0) {
    buffer.resize(bytes);
    if (_NSGetExecutablePath(buffer.data(), &bytes) != 0) return {};
  }
  return std::filesystem::canonical(buffer.data());
#else
  std::vector<char> buffer(4096);
  for (;;) {
    const auto bytes = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (bytes < 0) return {};
    if (static_cast<std::size_t>(bytes) < buffer.size())
      return std::filesystem::path(std::string(buffer.data(), bytes));
    if (buffer.size() >= (1u << 20)) return {};
    buffer.resize(buffer.size() * 2);
  }
#endif
}

auto JsonString(const std::string& input) -> std::string {
  static constexpr char hex[] = "0123456789abcdef";
  std::string output = "\"";
  for (const unsigned char character : input) {
    if (character == '"' || character == '\\') {
      output += '\\';
      output += static_cast<char>(character);
    } else if (character < 32) {
      output += "\\u00";
      output += hex[character >> 4];
      output += hex[character & 15];
    } else {
      output += static_cast<char>(character);
    }
  }
  return output + '"';
}
}  // namespace

int main(int argc, char** argv) {
  const auto own = ExecutablePath();
  if (own.empty()) {
    std::cerr << "YCSB dispatcher error: cannot locate the current executable\n";
    return 2;
  }
  const auto scalar = own.parent_path().parent_path() / "ycsb8/ycsb";
  const auto scalar_alias = scalar.parent_path() / "libycsb8.so";
  const std::filesystem::path variable = HASHING_VARIABLE_EXECUTABLE;
  if (argc == 2 && std::string_view(argv[1]) == "--dispatch-info") {
    std::cout << "{\"schema_version\":1,\"dispatcher\":" << JsonString(own.string())
              << ",\"scalar\":" << JsonString(scalar.string())
              << ",\"scalar_alias\":" << JsonString(scalar_alias.string())
              << ",\"expected_scalar_sha256\":" << JsonString(HASHING_SCALAR_SHA256)
              << ",\"variable\":" << JsonString(variable.string()) << "}\n";
    return 0;
  }

  std::vector<char*> arguments;
  arguments.reserve(static_cast<std::size_t>(argc) + 1);
  arguments.push_back(argv[0]);
  int mode = -1;
  bool flags = true;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (flags && argument == "--") {
      flags = false;
      arguments.push_back(argv[i]);
      continue;
    }
    int selected = -1;
    if (flags && (argument == "-v" || argument == "--v" ||
                  argument == "-v=true" || argument == "--v=true" ||
                  argument == "-v=1" || argument == "--v=1")) selected = 1;
    if (flags && (argument == "-v=false" || argument == "--v=false" ||
                  argument == "-v=0" || argument == "--v=0")) selected = 0;
    if (selected != -1) {
      if (mode != -1 && mode != selected) {
        std::cerr << "YCSB dispatcher error: conflicting variable-key switches\n";
        return 2;
      }
      mode = selected;
    } else {
      arguments.push_back(argv[i]);
    }
  }
  arguments.push_back(nullptr);
  std::error_code alias_error;
  if (std::filesystem::is_symlink(scalar_alias, alias_error) || alias_error ||
      !std::filesystem::equivalent(scalar, scalar_alias, alias_error) || alias_error) {
    std::cerr << "YCSB dispatcher error: the preserved scalar fingerprint alias is missing or inconsistent\n";
    return 2;
  }
  try {
    if (VariableDispatch::Sha256File(scalar) != HASHING_SCALAR_SHA256) {
      std::cerr << "YCSB dispatcher error: the preserved scalar executable checksum changed\n";
      return 2;
    }
  } catch (const std::exception&) {
    std::cerr << "YCSB dispatcher error: cannot verify the preserved scalar executable\n";
    return 2;
  }
  const auto& target = mode == 1 ? variable : scalar;
  if (target.empty() || access(target.c_str(), X_OK) != 0) {
    std::cerr << "YCSB dispatcher error: selected executable is unavailable: "
              << target.string() << "\n";
    return 2;
  }
  execv(target.c_str(), arguments.data());
  std::cerr << "YCSB dispatcher error: exec failed (errno=" << errno << ")\n";
  return 2;
}
