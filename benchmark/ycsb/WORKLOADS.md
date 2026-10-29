# Runtime operation-mix presets

Compile once in Release, then use `-workload=a|b|c|d` (case-insensitive).

| Preset | Search | Update | Insert |
|---|---:|---:|---:|
| a | 50% | 50% | 0 |
| b | 95% | 5% | 0 |
| c | 100% | 0 | 0 |
| d | 95% | 0 | 5% |

```bash
cd build/benchmark/ycsb
./ycsb -index=Timnah -workload=d -num_rounds=10 -num_threads=30 -num_bg=1
```

D now specifies only an operation mix. All four presets use the same RequestGen,
pre-generated Trace, Zipf key distribution, timing loop and Serve dispatch.
Searches target the preloaded key domain; they do not favor recent inserts.
The separate latest generator, acknowledgment registry and D executor are removed.
This is a custom D-ratio workload, not official YCSB read-latest D.

The existing framework replays exactly the same trace in every round. Insert
keys are generated once, outside timing. Thus D round0 adds the trace's new keys;
subsequent rounds attempt the same keys again, and the index rejects duplicates.
Throughput is attempted operations per second (the same counting convention as
other presets), not successful fresh insertions per second. Approximate operation
ratios are sampled when the trace is created. Dataset size stops growing after
round0. No special fresh-key path is introduced for D or custom mixes.

Omit `-workload` to preserve the current default C or custom ratio weights:

```bash
./ycsb -index=Timnah -read_ratio=0.95 -update_ratio=0 -insert_ratio=0.05 -num_rounds=10 -num_threads=30 -num_bg=1
```

Preset and explicit ratio flags cannot be combined. Weights must be finite,
nonnegative and have a positive total. `-shift` applies equally to all presets.
The original 8B key/value, NUMA settings, convergence and index implementations
are unchanged. No per-request workload selection branch is required.

## Observed Twitter and Webmail presets

The compiled executable also accepts `-workload=ts|tt|tc|w` and replays prepared observed requests.

| Code | Workload | Source | Search | Update |
|---|---|---|---:|---:|
| ts | Twitter-Storage | cluster 3 | 99.31% | 0.69% |
| tt | Twitter-Transient | cluster 22, timestamps [0,3600) | 77.54% | 22.46% |
| tc | Twitter-Compute | complete cluster 8 | 50.36% | 49.64% |
| w | Webmail | Ditto webmail-all archive | 18.14% | 81.86% |

Keys are mapped to collision-free 8-byte identifiers; values are deterministic 8-byte surrogates. Reads become Search operations and writes become Update operations. TTL, interarrival delays, and cache-specific conditional writes are not replayed. The input order is retained before concurrent execution. Finite inputs wrap without resampling.

The default preload contains 100 million distinct records. Observed keys form the active domain, while additional cold records fill the remaining capacity. Source scope, key admission, checksums, and per-round request counts are recorded in the input manifests and runtime logs. Preparation occurs outside throughput timing. See the [project README](../../README.md#step-2-install-dependencies-and-check-the-machine-configuration) for workload installation.

Use the same workload commands without recompiling. The observed presets select continuous Timnah maintenance when the convergence threshold is unspecified; an explicit `-timnah_convergence_threshold` is honored. YCSB A-D generation and timed execution are independent of these input files. Figure collectors fingerprint the selected data and validate matching cached measurements.
