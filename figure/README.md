# Reproducing Figures 9-17

This directory contains the measurement and plotting scripts for Figures 9-17
of *Timnah: A Tier-Memory-Native Hashing for Modern Tiered Memory Systems*.

## Prepare the environment

Complete the workload installation and Release build described in the
[project README](../README.md).
Python 3.10 or newer, NumPy, and Matplotlib are required.

## Run the scripts

From the repository root, enter the figure directory:

```bash
cd figure
```

Run the following commands sequentially. For each figure, the first command
collects the data and the second generates the plot.

```bash
# This takes about 5 hours.
python3 getFigure9.py
python3 generateFigure9.py

# This takes about 9 hours.
python3 getFigure10.py
python3 generateFigure10.py

# This takes about 20 minutes.
python3 getFigure11.py
python3 generateFigure11.py

# This takes about 5 minutes.
python3 getFigure12.py
python3 generateFigure12.py

# This takes about 15 minutes.
python3 getFigure13.py
python3 generateFigure13.py

# This takes about 15 minutes.
python3 getFigure14.py
python3 generateFigure14.py

# This takes about 35 minutes.
python3 getFigure15.py
python3 generateFigure15.py

# This takes about 20 minutes.
python3 getFigure16.py
python3 generateFigure16.py

# This takes about 8 hours.
python3 getFigure17.py
python3 generateFigure17.py
```

## Results

The scripts save measurement data as JSON files and plots as `Figure 9.pdf`
through `Figure 17.pdf` in this directory. Required intermediate files are
created automatically.

## Figures 18 and 19

Figure 18 compares EH, EH+TPP, and Timnah. To reproduce EH+TPP, configure a TPP-enabled kernel following *TPP: Transparent Page Placement for CXL-Enabled Tiered-Memory*, using the [authors' kernel implementation](https://lwn.net/Articles/876993/) and [Linux NUMA-balancing configuration](https://docs.kernel.org/admin-guide/sysctl/kernel.html#numa-balancing) as references, then run with an allocation policy that permits OS-managed page placement and migration. Keep the workload, CPU affinity, and available DRAM capacity consistent with the reference runs in which TPP is disabled. Figure 19 evaluates Timnah, Dash, and CHash under CXL-L, CXL-I, and PM. Use `numactl --hardware` to inspect the NUMA topology, and select a CXL memory node attached to the benchmark's CPU socket for CXL-L or to a different socket for CXL-I, while keeping CPU and DRAM placement fixed. Set the corresponding slow-tier node parameters as described in the [project README](../README.md). The PM comparison requires a PM-equipped experimental platform and its DRAM/PM node configuration; repeat the same workloads and benchmark settings on that platform.
