/// Test the benchmark framework
#include "parameter.hpp"
#include "trace.hpp"

auto ExpectedZipfDist(u64 data_set_size, u64 access_num) {
  double first_ratio{1.0};
  vector<double> expected_nums;
  for (int i = 0; i < data_set_size; ++i) {
    expected_nums.push_back(first_ratio / (i + 1));
  }
  double total = accumulate(expected_nums.begin(), expected_nums.end(), 0.0);
  for (auto& d : expected_nums) {
    d = d * access_num / total;
  }
  return expected_nums;
}

int main(int argc, char** argv) {
  Parameter::Init(argc, argv);
  LOGE(sizeof(RequestType));
  LOGE(sizeof(Request));
  LOG("{}\n", "hello");
  LOG("op size {}\n", sizeof(Request));
  Trace* pTrace{nullptr};
  RequestTypeRatio ratio{1.0, 0.0, 0.0, 0.0, 0.0};
  auto createTrace = [&]() {
    pTrace = new Trace(FLAGS_num_load, FLAGS_num_run, FLAGS_skewness, ratio);
  };
  MeasureTime("Create Trace", createTrace);

  u64 total_cnt{};
  auto v = pTrace->VerifyDistribution();
  for (int i = 0; i < 10; ++i) {
    LOG("{}: {}\n", v[i].first, v[i].second);
  }
  for (const auto& [k, c] : v) {
    total_cnt += c;
  }
  LOG("{} elements, {} % occupied, {} accesses\n", v.size(),
      100.0 * v.size() / FLAGS_num_load, total_cnt);

  auto expected = ExpectedZipfDist(FLAGS_num_load, FLAGS_num_run);
  for (int i = 0; i < 10; ++i) {
    LOG("{} : {:.2f}\n", i, expected[i]);
  }

  for (int i = v.size() - 10; i < v.size(); ++i) {
    LOG("{}: {}\n", v[i].first, v[i].second);
  }

  for (int i = expected.size() - 10; i < expected.size(); ++i) {
    LOG("{} : {:.2f}\n", i, expected[i]);
  }

  return 0;
}