#pragma once
#include "headers.hpp"
#include "logger.hpp"

// Debug logging
// ----------------------------------------------------------------------------

/// Checkpoint logging
#define AP(x) LOG("Arrive {}\n", #x)

/// Scope logging (RAII)
class RunGuard {
 private:
  string name;

 public:
  RunGuard(string name) : name(name) { LOG("Enter {}\n", name); }
  ~RunGuard() { LOG("Exit {}\n", name); }
};

// Assertions
// ----------------------------------------------------------------------------

/// Assertion with a message (also enabled in Release builds)
#define EnsureMsg(expression, info)                                   \
  if (!(expression)) [[unlikely]] {                                   \
    fmt::print("\n[Assertion Failed] {}\n[Error Message]: {}\n",      \
               fmt::format("file {}:{}, expression \"{}\"", __FILE__, \
                           __LINE__, #expression),                    \
               info);                                                 \
    abort();                                                          \
  }

/// Assertion without a message (also enabled in Release builds)
#define Ensure(expression) EnsureMsg(expression, "No Message Provided.")

/// Stop execution
#define AbortHere() EnsureMsg(false, "abort here")

/// Assertion in Debug builds
#ifdef NDEBUG
#define DebugEnsure(expression) ((void)0)
#else
#define DebugEnsure(expression) Ensure(expression)
#endif

/// Assertion with a message in Debug builds
#ifdef NDEBUG
#define DebugEnsureMsg(expression, info) ((void)0)
#else
#define DebugEnsureMsg(expression, info) EnsureMsg(expression, info)
#endif
