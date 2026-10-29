#include "parameter.hpp"
#include "request.hpp"
DEFINE_bool(probe_generate, false, "Generate a small deterministic operation sample");
int main(int argc, char** argv) {
  Parameter::Init(argc, argv);
  fmt::print("RESOLVED {} {} {} {} {} {}\n", FLAGS_workload.empty() ? "custom" : FLAGS_workload,
             FLAGS_read_ratio, FLAGS_update_ratio, FLAGS_insert_ratio, FLAGS_remove_ratio, FLAGS_negative_ratio);
  if (FLAGS_probe_generate) {
    RequestTypeGen generator({FLAGS_read_ratio, FLAGS_update_ratio, FLAGS_insert_ratio, FLAGS_remove_ratio, FLAGS_negative_ratio});
    RequestTypeGen::SetSeed(1729);
    std::array<u64, 5> counts{};
    for (u32 i = 0; i < 100000; ++i) ++counts[static_cast<u32>(generator.RandType())];
    fmt::print("COUNTS {} {} {} {} {}\n", counts[0], counts[1], counts[2], counts[3], counts[4]);
  }
}
