# Timnah: A Tier-Memory-Native Hashing for Modern Tiered Memory Systems

This repository contains the implementation of **Timnah**, described in the paper
**Timnah: A Tier-Memory-Native Hashing for Modern Tiered Memory Systems**. It
includes Timnah, the Dash, CHash, and EH+cache baselines, a common workload driver,
and scripts for the experiments in Figures 9-17.

## Experimental environment

The implementation supports Linux systems with local DRAM and a NUMA-addressable
CXL or persistent memory (PM) tier. The following configurations are supported:

| Platform | Processor and memory | Software |
|---|---|---|
| CXL platform | Dual-socket Intel Sapphire Rapids at 2.5 GHz; 48 cores and 256 GB DRAM per NUMA node; two 128 GB CXL 1.1 devices, each connected through PCIe 5.0 x8 with 16 GB/s bandwidth | Ubuntu 22.04, Linux 6.2.0 |
| PM platform | Four Intel Xeon Platinum 8260 processors at 2.40 GHz; 24 physical cores per socket, two hardware threads per core; four NUMA nodes, with 96 physical cores and 192 logical CPUs in total | Ubuntu 22.04.5 LTS, Linux 6.1.85 |

The supplied configuration places the fast tier on NUMA node 0, the slow tier on
node 2, and binds benchmark threads to node 0.

Timnah initially allocates 256 MiB of fast-tier buckets and 1 GiB of slow-tier
buckets for 8-byte keys. The 16-byte-key configuration uses 512 MiB and 2 GiB,
respectively.

The build requires an x86-64 Linux system, a C++20 compiler, CMake 3.22 or newer,
and Python 3.10 or newer. The commands below use GCC 11. The reference software
configuration uses GCC 11.4, CMake 3.22.1, Python 3.10.12, Matplotlib 3.5.1, and
NumPy 1.23.5.

## Step 1: Download the source code

Install Git if it is not already available, then clone the repository:

```bash
sudo apt-get update
sudo apt-get install -y git ca-certificates

cd "$HOME"
git clone https://github.com/zhaohan2001/Timnah.git
cd Timnah
```

Run Git, CMake, and the benchmarks as a regular user. Only package installation
requires `sudo`. All subsequent commands start in the repository root unless
stated otherwise.

## Step 2: Install dependencies and check the machine configuration

Install the following packages on Ubuntu 22.04:

```bash
sudo apt-get install -y \
  build-essential gcc-11 g++-11 cmake ninja-build \
  libfmt-dev libgtest-dev libgflags-dev libbenchmark-dev libjsoncpp-dev \
  libnuma-dev numactl \
  python3 python3-numpy python3-matplotlib zstd curl
```

These packages provide fmt, GoogleTest, gflags, Google Benchmark, JsonCpp,
libnuma, and the Python plotting libraries. GCC supplies the OpenMP and atomic
runtime libraries used by the project. YCSB generators are included in the repository. Install the four prepared
Twitter and Webmail workloads from the accompanying GitHub release:

```bash
python3 benchmark/twitter/download_traces.py
```

The download contains about 5.74 GiB of losslessly compressed data and requires
about 32 GiB after decompression. The installer verifies SHA-256 checksums and
places the inputs in `benchmark/twitter/data/`. It can resume interrupted
downloads. YCSB A-D do not require these inputs.

Check the compiler, Python libraries, and NUMA topology before building:

```bash
g++-11 --version
cmake --version
python3 --version
python3 -c "import matplotlib, numpy; print('Matplotlib', matplotlib.__version__); print('NumPy', numpy.__version__)"
numactl --hardware
lscpu
```

The choice of node 0 for CPUs and DRAM, and node 2 for the slow tier, reflects
the experimental configuration used in this work. NUMA node numbers depend on
the machine and firmware configuration. Select the CPU/DRAM node and the CXL or
PM memory node according to the topology reported on the target system.

On typical CXL platforms, CXL capacity exposed as system memory appears as a
memory-only NUMA node: `numactl --hardware` reports a nonzero memory size but an
empty CPU list for that node. This is a useful way to identify a candidate CXL
node; confirm its backing device using the platform's CXL device and region
information, since a CPU-less node alone does not uniquely identify CXL.
See the [Linux NUMA documentation](https://docs.kernel.org/admin-guide/mm/numaperf.html)
for the relationship between compute nodes and memory-only nodes.

The relevant node settings in the code are:

- **CPU binding:** `numa_run_on_node(0)` in
  [benchmark/ycsb/ycsb.cpp](benchmark/ycsb/ycsb.cpp), and in
  [benchmark/variable_keys/main.cpp](benchmark/variable_keys/main.cpp) for `-v`.
- **Memory-tier selection:** the following flags are defined near the beginning
  of each index implementation and can also be supplied on the command line.

| Index | Source file | Fast-tier flag | Slow-tier flag |
|---|---|---|---|
| Timnah | [hashindex/timnah/timnah.cc](hashindex/timnah/timnah.cc) | `-timnah_fast_node` | `-timnah_slow_node` |
| Dash | [hashindex/dash/dash.cc](hashindex/dash/dash.cc) | `-dash_dram_node` | `-dash_slow_node` |
| CHash | [hashindex/chash/chash.cc](hashindex/chash/chash.cc) | `-chash_dram_node` | `-chash_slow_node` |
| EH+cache | [hashindex/ehcache/ehcache.cc](hashindex/ehcache/ehcache.cc) | `-ehcache_dram_node` | `-ehcache_slow_node` |

For `-v`, the corresponding flag definitions are in
[hashindex/variable_keys/](hashindex/variable_keys/). Timnah's metadata allocation
also selects node 0 in [hashindex/timnah/storage.cc](hashindex/timnah/storage.cc)
and its variable-key counterpart; update that selection if the DRAM node changes.
The figure collectors and their measurement drivers also encode the experiment's
node settings and placement checks, which must be adjusted consistently for a
different topology. Rebuild in Release mode after changing source-level settings.

Both selected memory nodes must have memory available to the process, and CPU
binding must target a node with CPUs. On CXL and PM systems, make the slow-tier
capacity available as system memory through the platform's firmware and Linux
memory configuration before running the benchmark.

## Step 3: Build in Release mode

Configure and build a fresh checkout as follows:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-11 \
  -DCMAKE_CXX_COMPILER=g++-11 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHASHING_VARIABLE_KEYS=ON

cmake --build build --parallel 8
```

This builds the indexes and the ordinary 8-byte-key benchmark. The complete
build also builds, checks, and installs the independent 16-byte-key mode. Keep
the primary build directory named `build`, and let the full build finish before
running experiments. No separate library installation or manual binary copy is
needed.

Release compilation enables `-O3` and `-DNDEBUG`; the project also uses
`-march=native`. Build on the machine that will run the experiments. Check the
selected build type with:

```bash
grep '^CMAKE_BUILD_TYPE:' build/CMakeCache.txt
# Expected: CMAKE_BUILD_TYPE:STRING=Release
```

The system-package configuration has been validated with fmt 8.1.1 and
GoogleTest 1.11.0, including both key modes. If libraries are installed outside
the system directories, pass their installation prefix through
`CMAKE_PREFIX_PATH` when configuring the build.

## Step 4: Run an index on a workload

The benchmark executable is `build/benchmark/ycsb/ycsb`. The following commands
run all four indexes on YCSB C:

```bash
./build/benchmark/ycsb/ycsb -index=Timnah  -workload=c -num_rounds=10 -num_threads=30 -num_bg=1
./build/benchmark/ycsb/ycsb -index=Dash    -workload=c -num_rounds=10 -num_threads=30 -num_bg=0
./build/benchmark/ycsb/ycsb -index=CHash   -workload=c -num_rounds=10 -num_threads=30 -num_bg=0
./build/benchmark/ycsb/ycsb -index=EHCache -workload=c -num_rounds=10 -num_threads=30 -num_bg=0
```

`-num_bg` sets the number of background maintenance threads. Use `-num_bg=1`
for Timnah and `-num_bg=0` for the other indexes in these comparisons.

Each command above executes one billion workload operations after loading.

### Workload selection

Select a workload at run time with `-workload`; recompilation is not required.

| Code | Workload | Search | Update | Insert |
|---|---|---:|---:|---:|
| `a` | YCSB A | 50% | 50% | 0% |
| `b` | YCSB B | 95% | 5% | 0% |
| `c` | YCSB C | 100% | 0% | 0% |
| `d` | YCSB D | 95% | 0% | 5% |
| `ts` | Twitter-Storage, cluster 3 | 99.31% | 0.69% | 0% |
| `tt` | Twitter-Transient, cluster 22 | 77.54% | 22.46% | 0% |
| `tc` | Twitter-Compute, cluster 8 | 50.36% | 49.64% | 0% |
| `w` | Webmail | 18.14% | 81.86% | 0% |

YCSB uses a Zipfian distribution with a default parameter of 0.99. Twitter and
Webmail replay observed requests; the table reports the read/write proportions
of the prepared inputs. Keys are mapped to distinct 8-byte identifiers, and
values are deterministic 8-byte surrogates. Reads become searches and writes
become updates; cache-specific conditions, TTLs, and interarrival delays are
not replayed. Source order is retained before concurrent execution. Finite
inputs wrap without resampling. The Transient input covers cluster 22 timestamps
0-3599 seconds; Webmail contains all records in Ditto's public `webmail-all`
archive. Exact source scope, counts, and checksums are recorded in
[benchmark/twitter/datasets.json](benchmark/twitter/datasets.json).

From the repository root directory (`Timnah/`, which contains `CMakeLists.txt`,
`build/`, and `figure/`), run Timnah on the eight workloads as follows:

```bash
./build/benchmark/ycsb/ycsb -index=Timnah -workload=a  -num_rounds=10 -num_threads=30 -num_bg=1
./build/benchmark/ycsb/ycsb -index=Timnah -workload=b  -num_rounds=10 -num_threads=30 -num_bg=1
./build/benchmark/ycsb/ycsb -index=Timnah -workload=c  -num_rounds=10 -num_threads=30 -num_bg=1
./build/benchmark/ycsb/ycsb -index=Timnah -workload=d  -num_rounds=10 -num_threads=30 -num_bg=1
./build/benchmark/ycsb/ycsb -index=Timnah -workload=ts -num_rounds=10 -num_threads=30 -num_bg=1
./build/benchmark/ycsb/ycsb -index=Timnah -workload=tt -num_rounds=10 -num_threads=30 -num_bg=1
./build/benchmark/ycsb/ycsb -index=Timnah -workload=tc -num_rounds=10 -num_threads=30 -num_bg=1
./build/benchmark/ycsb/ycsb -index=Timnah -workload=w  -num_rounds=10 -num_threads=30 -num_bg=1
```

The same executable can be invoked as `./ycsb` from `build/benchmark/ycsb`.

### 16-byte keys

Append `-v` to an existing YCSB A, B, C, or D command to use 16-byte keys and
8-byte values:

```bash
./build/benchmark/ycsb/ycsb -index=Timnah -workload=c -num_rounds=10 -num_threads=30 -num_bg=1 -v
./build/benchmark/ycsb/ycsb -index=Dash   -workload=c -num_rounds=10 -num_threads=30 -num_bg=0 -v
```

The slot stores an 8-byte key pointer with a 16-bit fingerprint; full keys are
held in separate storage. A matching fingerprint is followed by a full-key
comparison. Commands without `-v` continue to use the ordinary
8-byte-key implementation. The Twitter and Webmail presets use 8-byte keys.

### Measurements and output

The ordinary 8-byte-key benchmark reports trace-initialization time, load time,
and load throughput. To track performance during execution, the commands above
group every 100 million workload operations into one round. For each round, the
benchmark reports:

- Execution time in milliseconds and throughput in millions of operations per
  second (`Mops/s`).
- Sampled mean latency and P50, P90, P99, P99.9, P99.99, and P99.999 latency in
  nanoseconds. Latency collection is enabled by default with a 1% sample rate.
- Index-specific statistics. Timnah reports tier hits, reordering and migration
  counts, sampling and queue activity, convergence state, and allocated bucket
  and counter bytes.

The following excerpt shows round 9 from a 30-thread Timnah run on YCSB C:

```text
Round 9
Run time: 723.30 ms
Run throughput: 138.255 Mops/s
Mean latency: 249 ns
Tail latency 50%: 135 ns
Tail latency 99%: 1171 ns
```

Save the complete output for later analysis with:

```bash
mkdir -p benchmark-results
set -o pipefail
./build/benchmark/ycsb/ycsb -index=Timnah -workload=c -num_rounds=10 -num_threads=30 -num_bg=1 \
  2>&1 | tee benchmark-results/timnah-ycsb-c-t30.log
```

## Step 5: Run the experiments

See [figure/README.md](https://github.com/zhaohan2001/Timnah/blob/main/figure/README.md)
for instructions on running the experiment scripts, collecting data, and
generating Figures 9-17.
