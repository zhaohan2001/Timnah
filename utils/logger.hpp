#pragma once
#include "headers.hpp"

// Logging
// ----------------------------------------------------------------------------
#define LOG fmt::print
#define LOGE(x) fmt::print("{}: {}\n", #x, x)

// Timestamped logging
// ----------------------------------------------------------------------------
// TODO: Implement.
#define TLOG

// Output formatting
// ----------------------------------------------------------------------------

/// Format a container.
template <typename T>
inline auto StringifyContainer(const T& c) -> string {
  string res;
  res.append("[");
  for (const auto& x : c) {
    res += fmt::format("{}, ", x);
  }
  res.append("]");
  return res;
}

/// Convert a result to a string.
inline auto to_string(Result _res) -> string {
  switch (_res) {
    case Result::SUCCESS:
      return "SUCCESS";
    case Result::FAILED:
      return "FAILED";
    case Result::EXISTS:
      return "EXISTS";
    case Result::NOT_FOUND:
      return "NOT_FOUND";
    case Result::LOCKED:
      return "LOCKED";
    case Result::FULL:
      return "FULL";
    case Result::LOCK_CHANGED:
      return "LOCK_CHANGED";
    case Result::NEED_RETRY:
      return "NEED_RETRY";
    case Result::DUPLICATE:
      return "DUPLICATE";
    case Result::GOING_FULL:
      return "GOING_FULL";
    default:
      return "UNKNOWN";
  }
}

/// Human-readable size.
inline auto ReadableSize(u64 size) -> string {
  if (size > (1ull << 30)) {
    return fmt::format("{:.2f} GiB", 1.0 * size / (1ull << 30));
  } else if (size > 1ull << 20) {
    return fmt::format("{:.2f} MiB", 1.0 * size / (1ull << 20));
  } else if (size > 1ull << 10) {
    return fmt::format("{:.2f} KiB", 1.0 * size / (1ull << 10));
  } else {
    return fmt::format("{} B", size);
  }
}

/// Human-readable time.
inline auto ReadableTime(u64 nano) -> string {
  string ret;
  if (nano < 1e3)
    ret = fmt::format("{} ns", nano);
  else if (nano < 1e6)
    ret = fmt::format("{:.2f} us", nano / 1e3);
  else if (nano < 1e9)
    ret = fmt::format("{:.2f} ms", nano / 1e6);
  else
    ret = fmt::format("{:.2f} s", nano / 1e9);
  return ret;
}
