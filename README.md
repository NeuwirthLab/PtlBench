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
combining it with `--get` is rejected. Memory ping-pong messages must fit in
one system page and are capped at 4 KiB. In shared-page memory ping-pong, the
responder waits for the target PUT event before replying into the initiator's
buffer. Both ranks wait for local SEND completion before releasing their
descriptors, outside the measured round trip. Positive iteration counts,
message sizes, and windows are required, with nonnegative warm-up and valid ranges.

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

Each executable buffers the measured samples and reports them after all iterations
for that message size. The next size overwrites the same sample storage. This
also applies to raw CSV and table output. When comparing with earlier runs,
account for the removal of printing between iterations.

Corrections affect comparison with old results: counting latency now waits for
every operation, ping-pong uses independent initialized send/receive buffers and
a separate send-completion counter, and cache sweeps cannot be optimized away.
The per-message benchmark now uses the correct NI and command ME. `pf_bench`
excludes warm-up and RNG overhead. Previous results affected by these defects
should be regenerated. The programs no longer silently write the BXI cache-region
sysfs setting at startup; `ptl_bench --pids N` remains an explicit site-specific
control and reports write failures.

## Collect a complete benchmark suite

On `hpca-013` and `hpca-014`, run the site launcher from the repository:

```sh
./run.sh
```

It loads `openmpi/5.0.9`, builds in Release mode under `build/performance`, and
collects ten repetitions of each experiment in a timestamped directory under
`results`. Runner arguments
can be passed directly, including `./run.sh --dry-run` to preview the collection
after building. Use `./run.sh --cache-pids 1` for a collection limited to the
baseline value when the hosts already use 1 and your account cannot change it.

The Python 3.8+ runner uses only the standard library. Build the current
executables first, load the appropriate MPI environment, and inspect the plan:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
module load openmpi/5.0.9  # this cluster; use your site's MPI module elsewhere
python3 scripts/run_benchmarks.py --build build \
  --hosts hpca-013,hpca-014 --mpi-args="-x LD_LIBRARY_PATH" \
  --output results --label bxi --dry-run
```

Remove `--dry-run` to execute the suite. Each invocation creates a new directory
under `--output`; it never replaces a previous collection. A short collection
with one measured sample, no warm-up, and window 1 is useful for checking setup:

```sh
python3 scripts/run_benchmarks.py --build build \
  --hosts hpca-013,hpca-014 --mpi-args="-x LD_LIBRARY_PATH" \
  --iterations 1 --warmup 0 --windows 1 --cache-mib 1 --repeats 1 \
  --output results --label smoke
```

The normal defaults are **10 measured iterations, 10 warm-up, bandwidth window 64,
16 MiB cache sweeps, seed 1, and ten repetitions**. Each repetition runs the
entire matrix sequentially and saves its own results in `repeat-001` through
`repeat-010`. The iteration and warm-up counts apply to each launch. The memory
benchmark has no warm-up option, so its warm-up remains zero. Run `pf_bench`
directly for the standalone page-fault experiment.

The default matrix covers the four network benchmarks and 268 cases per
repetition, for 2,680 planned cases in total. It includes
PUT/GET, matching/non-matching, counting/full events, latency/bandwidth, hot/cold
cache workloads, standard/triggered ping-pong, and all local/remote page states.
Message sizes increase by powers of two from **1 byte to 4 MiB**, with a
**4 KiB** cap for all `ptl_memory_bench` modes, including shared-page ping-pong.
The runner uses native sweeps for
`ptl_bench` and `ptl_me_none_persistent` and launches the single-size benchmarks
once per size.
Use `--windows 1 8 64` to sweep bandwidth and per-message windows explicitly.
`ptl_bench` latency uses window 1.

The plain `ptl_bench` PUT/GET cases sweep BXI `cache_pids` at **1 and 128**.
Use `--cache-pids N [N ...]` to choose other values for those cases. All other
network benchmarks use 1.
Each MPI rank verifies the host's setting before Portals initialization. Changing
the value requires write access to `/sys/class/bxi/bxi0/v2p/cache_pids` on each
host where the value differs. A permissions error fails the affected case.
The setting is host-wide and persists at the last value applied on each host.
The runner records the requested value in directory names and metadata and checks
that both ranks confirm it before accepting a run.

Memory messages must also fit a page on each rank. The runner attempts sizes in
ascending order and learns tighter limits from explicit benchmark diagnostics.
It records the first
rejected case as `unsupported` and later covered cases as `skipped`, with a link
to the original diagnostic. It similarly skips remaining triggered cases after a
reported trigger-capacity rejection. It never lowers iterations or truncates a
sweep silently, and never treats arbitrary MPI/SSH errors as unsupported hardware.
Limits come from the launched ranks, not an assumption about the launch host's
page size. The reviewed NI reports zero trigger capacity.

Use `--benchmarks ptl_ping_pong ptl_memory_bench` to select executables, `--repeats 3`
to set the repetition count to three, and `--mpirun /path/to/mpirun` to choose a launcher.
The runner inherits the caller's module environment. MPI jobs always use two
ranks and default to
`--map-by ppr:1:node`. Without `--hosts`, supply a suitable allocation; use
`--map-by` and `--mpi-args` for other Open MPI placement/runtime options. The
absolute build path must be accessible at the same location on both nodes.
Jobs execute sequentially. Every launch has a configurable `--timeout` (300 s by
default). While waiting for a long launch, the runner reports elapsed time and
the number of statistics rows received. A row arrives after its message size
finishes, so the largest sizes can take much longer between updates. The
timeout covers the whole sweep in a single launch. For example, a 64-message
bandwidth window with 10,000 measured iterations and 100 warm-up iterations per
size may need `--timeout 900` on the BXI hosts.
Timeout or interruption terminates the launcher process group and stops
the collection; MPI is given time to clean up remote ranks. Ordinary failures
are recorded while other cases continue; `--fail-fast` stops on those too.

Example collection layout:

```text
results/20261001T090000.123456Z-bxi/
  manifest.json
  summary.csv
  ptl_bench/put-nonmatching-counting-latency-cache-hot-window-1/cache-pids-1/repeat-001/
    statistics.csv
    stderr.log
    run.json
  ptl_ping_pong/standard/bytes-04194304/cache-pids-1/repeat-001/...
  ptl_memory_bench/put-local-cold-remote-hot/bytes-00004096/cache-pids-1/repeat-001/...
```

`statistics.csv` contains one row per message size and metric in each repetition.
Each row reports the sample count, unit, arithmetic mean, median, minimum, and
maximum over measured iterations. The runner passes `--stats` to the
executables, which calculate
statistics in C and print them after the iterations at each size. The runner
validates this CSV before saving it. For a direct invocation, use `--stats`
for the same summary or omit it for raw CSV. `--table` displays the raw
measurements in a readable table.
`stderr.log` retains configuration, placement diagnostics, and errors.
`run.json` records the exact command as an argument array, configuration, units,
UTC timestamps, return code, status, and launch elapsed time. Launch elapsed time
includes MPI startup/teardown and is not a benchmark latency. `manifest.json`
contains the full plan and outcomes, binary SHA-256 hashes, Git revision/status,
selected build/environment metadata, and observed limits. `summary.csv` indexes
all cases and links to statistics and diagnostics. Metadata is updated between
launches. An active run also has its own `run.json`.

Only **`ok`** rows in `summary.csv` have a successful exit and validated CSV header,
metric names, units, size coverage, and sample counts. Failed, skipped, and
unavailable cases have their status recorded in the summary. The script
returns 0 when all runnable cases succeeded (explicit unsupported/skipped cases
are allowed), 1 for failures/missing executables/incomplete data/timeouts, and 130
for interruption. Dry runs create only a plan and summary, and print commands.

## Focused validation

```sh
python3 tests/check.py --build build
python3 tests/check_finder.py
python3 -m unittest discover -s tests -p test_runner.py
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

