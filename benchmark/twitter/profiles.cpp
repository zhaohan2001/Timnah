#include "benchmark/twitter/profiles.hpp"
#include "benchmark/synthetic/trace_replay.hpp"
namespace twitter_workload {
auto GetProfile(std::string_view code) -> std::optional<synthetic::Profile> {
  if(code=="ts"||code=="tt"||code=="tc")return real_trace::ProfileFor(std::string(code));
  return std::nullopt;
}
}
