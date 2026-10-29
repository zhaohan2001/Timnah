/// Headers
#pragma once

/// Standard library headers
// ----------------------------------------------------------------------------

#include <bits/stdc++.h>

/// Third-party library headers
// ----------------------------------------------------------------------------

#include <fmt/color.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <jsoncpp/json/json.h>
#include <omp.h>

#ifdef __linux__
#include <numa.h>
#endif

/// Common namespaces
// ----------------------------------------------------------------------------
using namespace std;
using namespace std::chrono;

/// Common type aliases
// ----------------------------------------------------------------
using u128 = unsigned __int128;
using u64 = uint64_t;
using u32 = uint32_t;
using u16 = uint16_t;
using u8 = uint8_t;
using i64 = int64_t;
using i32 = int32_t;
using i16 = int16_t;
using i8 = int8_t;

/// Operation result codes
enum class Result : u8 {
  SUCCESS,
  FAILED,
  EXISTS,
  NOT_FOUND,
  LOCKED,
  FULL,
  LOCK_CHANGED,
  NEED_RETRY,
  DUPLICATE,
  GOING_FULL
};
