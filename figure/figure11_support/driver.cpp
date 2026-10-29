// Dedicated Figure 11 measurement driver. Never linked into production ycsb.
#include "common.hpp"
#include "parameter.hpp"
#include "trace.hpp"
#include "figure11_observer.hpp"
#include "hashindex/timnah/timnah.h"
#include "hashindex/dash/dash.h"
#include "hashindex/chash/chash.h"
#include "hashindex/ehcache/ehcache.h"

DEFINE_string(index, "Timnah", "Figure 11 index: Timnah, Dash, CHash, EH+cache");

namespace {
constexpr u64 kStagesPerRound = 10;
struct alignas(64) WorkerCounts {
  u64 searches{}, dram{}, slow{}, misses{}, updates{}, update_failures{};
};

auto MakeIndex() -> std::unique_ptr<IndexInterface> {
  if (FLAGS_index == "Timnah") return std::make_unique<TimnahNameSpace::Timnah>();
  if (FLAGS_index == "Dash") return std::make_unique<DashNameSpace::Dash>();
  if (FLAGS_index == "CHash") return std::make_unique<CHashNameSpace::CHash>();
  if (FLAGS_index == "EH+cache") return std::make_unique<EHCacheNameSpace::EHCache>();
  throw std::runtime_error("Unsupported Figure 11 index");
}

void Require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

void RunStageRound(IndexInterface& index, const Trace& trace, u64 round) {
  std::vector<WorkerCounts> counts(FLAGS_num_run_threads);
  const u64 stage_operations = FLAGS_num_run / kStagesPerRound;
#pragma omp parallel num_threads(FLAGS_num_run_threads)
  {
    const unsigned worker = omp_get_thread_num();
    (void)figure11::Drain();
    for (u64 stage = 0; stage < kStagesPerRound; ++stage) {
      WorkerCounts local;
      (void)figure11::Drain();
      const u64 begin = stage * stage_operations;
      const u64 end = begin + stage_operations;
#pragma omp for schedule(dynamic, 100000)
      for (u64 position = begin; position < end; ++position) {
        const auto& request = trace.GetRunRequest(position);
        if (request.type == RequestType::Read) {
          ++local.searches;
          u64 value;
          if (index.Search(request.key, value) != Result::SUCCESS) ++local.misses;
        } else if (request.type == RequestType::Update) {
          ++local.updates;
          if (index.Update(request.key, request.value) != Result::SUCCESS)
            ++local.update_failures;
        } else {
          fmt::print(stderr, "Unexpected operation in Figure 11 trace\n");
          std::abort();
        }
      }
      const auto observed = figure11::Drain();
      local.dram = observed.dram_hits;
      local.slow = observed.slow_hits;
      counts[worker] = local;
#pragma omp barrier
#pragma omp single
      {
        WorkerCounts total;
        for (const auto& item : counts) {
          total.searches += item.searches;
          total.dram += item.dram;
          total.slow += item.slow;
          total.misses += item.misses;
          total.updates += item.updates;
          total.update_failures += item.update_failures;
        }
        if (!total.searches || total.dram + total.slow + total.misses != total.searches ||
            total.searches + total.updates != stage_operations || total.misses ||
            total.update_failures) {
          fmt::print(stderr,
              "Figure11Error round={} stage_in_round={} searches={} dram={} slow={} misses={} updates={} update_failures={}\n",
              round, stage + 1, total.searches, total.dram, total.slow,
              total.misses, total.updates, total.update_failures);
          std::abort();
        }
        fmt::print(
            "Figure11Stage stage={} round={} stage_in_round={} operations={} search_attempts={} dram_hits={} slow_hits={} read_misses={} update_attempts={} update_failures={} dram_hit_rate={:.15f}\n",
            round * kStagesPerRound + stage + 1, round, stage + 1,
            stage_operations, total.searches, total.dram, total.slow,
            total.misses, total.updates, total.update_failures,
            static_cast<double>(total.dram) / total.searches);
        std::fflush(stdout);
      }
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Parameter::Init(argc, argv);
    Require(FLAGS_workload == "c" || FLAGS_workload == "a" ||
                FLAGS_workload == "ts" || FLAGS_workload == "w",
            "Figure 11 supports only c, a, ts, and w");
    Require(FLAGS_num_rounds == 10 && FLAGS_num_run >= 10000 &&
                FLAGS_num_run % kStagesPerRound == 0,
            "Figure 11 needs ten rounds and num_run >= 10000 divisible by ten");
    Require(FLAGS_num_run_threads == 30 && FLAGS_num_load_threads == 30 &&
                FLAGS_num_threads == 30,
            "Figure 11 requires 30 foreground threads");
    Require(FLAGS_num_bg == (FLAGS_index == "Timnah" ? 1u : 0u),
            "Figure 11 needs one Timnah worker and zero workers for other indexes");
    if (FLAGS_bind_core) Require(numa_run_on_node(0) == 0, "Cannot bind to CPU node 0");
    omp_set_dynamic(0);
    // As in ordinary ycsb, synthetic generation explicitly uses run threads.
    // Static YCSB retains its existing trace-generation policy.
    if (synthetic::IsProfileCode(FLAGS_workload))
      omp_set_num_threads(FLAGS_num_run_threads);

    auto index = MakeIndex();
    Require(index->Create(Parameter::GetIndexParam()) == Result::SUCCESS,
            "Index creation failed");
    fmt::print("Figure11Config index={} workload={} num_threads=30 num_bg={} num_rounds=10 stages_per_round=10 num_load={} num_run={} metric=search_dram_hits/search_attempts diagnostic_only=true\n",
               FLAGS_index, FLAGS_workload, FLAGS_num_bg, FLAGS_num_load, FLAGS_num_run);

    std::unique_ptr<Trace> trace;
    if (synthetic::IsProfileCode(FLAGS_workload)) {
      trace = std::make_unique<Trace>(synthetic::GetProfile(FLAGS_workload),
                                      FLAGS_num_load, FLAGS_num_run, FLAGS_rndSeed);
    } else {
      const RequestTypeRatio ratios{FLAGS_read_ratio, FLAGS_update_ratio,
                                    FLAGS_insert_ratio, FLAGS_remove_ratio,
                                    FLAGS_negative_ratio};
      trace = std::make_unique<Trace>(FLAGS_num_load, FLAGS_num_run,
                                      FLAGS_skewness, ratios);
    }
    u64 failed_load = 0;
#pragma omp parallel for num_threads(FLAGS_num_load_threads) schedule(dynamic, 100000) reduction(+:failed_load)
    for (u64 i = 0; i < FLAGS_num_load; ++i) {
      const auto& request = trace->GetLoadRequest(i);
      failed_load += index->Insert(request.key, request.value) != Result::SUCCESS;
    }
    Require(failed_load == 0, "The preload contains failed inserts");
    fmt::print("Figure11Load records={} failed_inserts=0 statistics={}\n",
               FLAGS_num_load, index->Stat());
    for (u64 round = 0; round < 10; ++round) {
      if (synthetic::IsProfileCode(FLAGS_workload)) trace->PrepareRound(round);
      fmt::print("Round {}\n", round);
      RunStageRound(*index, *trace, round);
      // Only true round boundaries call Stat: Timnah convergence is unchanged.
      fmt::print("Figure11Round round={} statistics={}\n", round, index->Stat());
      std::fflush(stdout);
    }
    index->Stop();
    fmt::print("Figure11Complete stages=100 operations={} metric=search_dram_hits/search_attempts\n",
               FLAGS_num_run * 10);
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "Figure11Error {}\n", error.what());
    return 1;
  }
}
