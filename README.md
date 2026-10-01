# PtlBench

PtlBench is a C11 microbenchmark suite for Portals4. MPI orchestrates and
synchronizes the four communication benchmarks; each requires exactly two ranks.
The standalone `pf_bench` measures the first CPU store to anonymous pages.

## Benchmarks

| Executable | Measurement | Modes |
| --- | --- | --- |
| `ptl_bench` | Operation completion latency (us), or window throughput (decimal MB/s) | PUT/GET, matching/non-matching NI, counting/full events, optional initiator cache sweep |
| `ptl_me_none_persistent` | Window throughput (MB/s) and window time divided by message count (us/operation) | PUT/GET, matching NI, full events, use-once MEs and a separate command portal |
| `ptl_ping_pong` | Full round-trip time (us); triggered mode also reports reply setup time (us) | Standard PUT or pre-posted triggered PUT replies, counting events |
| `ptl_memory_bench` | One-sided completion latency or PUT ping-pong RTT (us) | PUT/GET one-sided, independently hot/cold local and remote pages |
| `pf_bench` | First CPU store time (ns) | Fresh anonymous pages, cache sweep before each sample |

`ptl_get_ni_props` is not implemented or built in this checkout. Returned NI limits
are retained and checked by the communication benchmarks. The public executable
name `ptl_me_none_persistent` is retained for compatibility.

The per-message benchmark posts one use-once ME per message, with exact match
bits, before sending a readiness command. Target PUT/GET events establish
completion and automatic unlinking. Its timed region includes the wait for
readiness; entry setup can overlap the initiator's preceding iteration/reporting.
Its reported latency is amortized window time, not individual message latency.

## Build and install

Requirements: CMake 3.20 or newer, a C11 compiler, MPI with C support, and Portals4
headers and library. Use a matching MPI compiler, launcher, and runtime library.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix /path/to/install
```

All five executables are installed. Dependency discovery currently runs even when
only `pf_bench` is requested; see [review recommendations](REVIEW.md).

For a dependency installed outside standard system paths:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/dependency/prefix
```

The finder searches for `portals4.h` and the library base name `portals`, including
`lib` and `lib64`. Explicit overrides remain available:

```sh
cmake -S . -B build \
  -DPORTALS_INCLUDE_DIR=/path/to/include \
  -DPORTALS_LIBRARY=/path/to/lib/libportals.so \
  -DMPI_C_COMPILER=/path/to/mpi/bin/mpicc
```

Choose headers and a library from the same implementation. Use a fresh build
directory when changing MPI or dependency installations: cached paths can retain
an earlier installation. Configuration/linking success does not ensure runtime
library availability on remote nodes. Configure the loader on every node (for
example through the site's module or `LD_LIBRARY_PATH`).

## Run and output

```sh
mpirun -np 2 ./build/ptl_bench --msg_size 8 -i 3 -x 1
mpirun -np 2 ./build/ptl_bench --msg_size 8 -i 3 -x 1 --table
mpirun -np 2 ./build/ptl_me_none_persistent --msg_size 8 -i 3 -u 1 -w 2
mpirun -np 2 ./build/ptl_ping_pong -m 8 -i 3 -w 1 --triggered --table
mpirun -np 2 ./build/ptl_memory_bench -m 8 -i 3 --local-hot --table
./build/pf_bench -i 3 -w 1 --table
```

On the reviewed cluster, initialize Lmod if the shell has not done so and load
`module load openmpi/5.0.9`. The previously suggested `openmpi/portals4` module is
not present there. Avoid mixing that module with `openmpi/ib`. An explicit
one-rank-per-node Open MPI launch on the project hosts is:

```sh
mpirun -np 2 --host hpca-013,hpca-014 --map-by ppr:1:node \
  -x LD_LIBRARY_PATH ./build/ptl_bench --msg_size 8 -i 3 -x 1 --table
```

Every executable supports `--help` and opt-in `--table`. CSV remains the default;
its existing headers, column order, and numeric units are preserved. Configuration
and diagnostics go to stderr, results to stdout. Only rank 0 reports network
results. The legacy CSV `latency` field means RTT in memory ping-pong mode; it is
not divided by two. `ptl_ping_pong` reports full RTT and, with `--triggered`, the
local cost of registering each reply trigger on rank 1, not trigger execution time.

Option spellings remain program-specific: `ptl_bench` uses `-x/--warmup` and
`-w/--window_size`, the per-message benchmark uses `-u/--warmup` and
`-w/--window-size`, and `ptl_ping_pong` uses `-w/--warmup`. Message sizes are bytes.
Both cache-size options (`ptl_bench -c/--cache_size` and
`ptl_memory_bench -c/--cache-size`) are in **MiB**, preserving their implemented
behavior. `--cold_cache` takes no argument. Memory ping-pong only supports PUT;
combining it with `--get` is rejected. Shared-page memory ping-pong also requires
message size no greater than the returned NI `max_volatile_size`; use `-m 8` for
the validated small-message case on this cluster. Positive iterations/message sizes/windows,
nonnegative warm-up, bounded arithmetic, and valid ranges are required.

Defaults: `ptl_bench` uses non-matching PUT/counting events, 10 measured iterations,
10 warm-up, a doubling sweep from 1 to 4194304 bytes, and bandwidth window 64.
The per-message benchmark uses PUT, one measured window, zero warm-up, window 64,
and the same size sweep. `ptl_ping_pong` uses 512 bytes, 5000 measured iterations,
and 100 warm-up; the total must fit the NI trigger capacity in triggered mode.
`ptl_memory_bench` uses 512 bytes, 10 iterations, no warm-up, cold pages on both
ranks, and one-sided PUT. Cache sweeps default to 16 MiB. `pf_bench` defaults to
10000 measured pages and 100 warm-up pages; `count` retains the original page
index, starting at the warm-up count.

## Memory state and result comparability

Memory experiments map fresh pages without initializing cold buffers. Hot flags
touch the selected rank's page after binding, as in the original implementation.
Messages fit within the actual system page size, at a randomly selected aligned
message block. `--seed N` (default 1) makes offsets repeatable within a given C
library in `ptl_memory_bench` and `pf_bench`; the seed and page size are reported.
`pf_bench` selects its offset before starting the timer and excludes warm-up rows.

A sequential volatile cache-buffer sweep is an eviction workload, not proof that
all CPU caches are cold. The network cache option affects the initiator only;
it does not reset NIC translation caches. Registration, huge pages, NUMA policy,
GET from untouched zero-backed pages, and the implementation's handling of
inaccessible target memory can change the observed work. Timing alone cannot
identify whether the CPU, driver, or NIC serviced a page fault. Final payloads are checked after the initialized-buffer benchmarks. No validation
reads are added to cold pages.

CSV/table printing stays outside timed regions and at existing reporting points.
Per-sample reporting may still perturb subsequent samples. Do not compare these
short validation runs with performance baselines. Record build mode, MPI/Portals
versions, placement, and memory settings for benchmark comparisons.

Corrections affect comparison with old results: counting latency now waits for
every operation, ping-pong uses independent initialized send/receive buffers and
a separate send-completion counter, and cache sweeps cannot be optimized away.
The per-message benchmark now uses the correct NI and command ME. `pf_bench`
excludes warm-up and RNG overhead. Previous results affected by these defects
should be regenerated. The programs no longer silently write the BXI cache-region
sysfs setting at startup; `ptl_bench --pids N` remains an explicit site-specific
control and reports write failures.

## Focused validation

```sh
python3 tests/check.py --build build
python3 tests/check_finder.py
# Requires configured MPI/Portals4 and permission to launch on both hosts:
python3 tests/check.py --build build --mpi --hosts hpca-013,hpca-014
```

The first command checks parsing, help, and standalone CSV/table output. `--mpi`
also checks communication modes, warm-up/sample counts, memory-state combinations,
and network CSV/table output, with a 30-second timeout for each MPI launch.
These are smoke/regression checks, not performance measurements or proof of all
resource-exhaustion and hardware failure behavior. See [REVIEW.md](REVIEW.md) for
findings, validation details, and remaining recommendations.

### Cite as
If you use **PtlBench** in your research or publications, please cite the following paper:  
```bibtex
@INPROCEEDINGS{Bartelheimer_2025,
  author={Bartelheimer, Niklas J. and Neuwirth, Sarah M.},
  booktitle={2025 33rd International Symposium on Modeling, Analysis and Simulation of Computer and Telecommunication Systems (MASCOTS)}, 
  title={Comprehensive Performance Analysis of Portals4 Communication Primitives on BXI Hardware}, 
  year={2025},
  pages={1-8},
  keywords={Analytical models;Computational modeling;Bandwidth;Benchmark testing;Libraries;Hardware;Performance analysis;Telecommunications;Electronics packaging;MPI;GASPI;PGAS;BXI;Portals4;Performance Study;Benchmarking;Performance Analysis},
  doi={10.1109/MASCOTS67699.2025.11283317}}
```

