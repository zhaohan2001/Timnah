#pragma once
#include "headers.hpp"

// Benchmark timer
// ----------------------------------------------------------------------------

/// Stopwatch (starts automatically)
class StopWatch {
 public:
  // ~~~~Construction~~~~
  StopWatch() { Start(); }

  /// Start timing.
  auto Start() -> void { startPoint_ = high_resolution_clock::now(); }

  /// Return elapsed time.
  template <typename RetType = u64, typename TimeType = chrono::nanoseconds>
  auto Elapsed() -> RetType {
    std::chrono::duration<RetType, typename TimeType::period> e =
        std::chrono::high_resolution_clock::now() - startPoint_;
    return e.count();
  }

 private:
  // ~~~~Data~~~~
  high_resolution_clock::time_point startPoint_{};
};

/// Measure a function runtime. Return floating-point nanoseconds.
template <typename F, typename... Args>
double MeasureTime(const string& func_name, F&& func, Args&&... args) {
  StopWatch watch{};
  std::forward<F>(func)(std::forward<Args>(args)...);  // Invoke the function
  u64 elapsed_time{watch.Elapsed()};
  if (!func_name.empty()) {
    LOG("{} runtime: {}\n", func_name, ReadableTime(elapsed_time));
  }
  return elapsed_time;
}
