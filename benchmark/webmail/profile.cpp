#include "benchmark/webmail/profile.hpp"
#include "benchmark/synthetic/trace_replay.hpp"
namespace webmail_workload {
auto GetProfile() -> synthetic::Profile {return real_trace::ProfileFor("w");}
}
