## PtlBench
PtlBench is a Portals4 microbenchmark suite designed to test and analyze the performance of different parts of the Portals4 API. Unlike traditional low-level network benchmarks that use a client–server setup, PtlBench uses MPI for process orchestration,
offering better portability, ease of use, and reliable synchronization through MPI’s collective operations. It works seamlessly with Portals4-enabled network interfaces and includes five focused microbenchmarks, each targeting a specific performance aspect of the Portals4 communication layer.

### Benchmark Overview
- **ptl_bench:** The ptl bench benchmark measures band-
width and latency for PtlPut and PtlGet operations,
modeled after OMB’s one-sided communication benchmarks.
It supports both non-matching or matching NIs, and allows
evaluation of event handling via either FEs or CTs. To assess
cache effects, a custom cache saturation function is used to
simulate cold-cache conditions by populating memory with
random values. This enables controlled comparisons between
cold and hot cache scenarios. On the target side, communi-
cation uses either a persistent LE or ME, where “persistent”
denotes reuse of the same list entry throughout the benchmark.

- **ptl_me_none_persistent:** The ptl me none persistent
benchmark measures the latency and bandwidth of Portals4’s
matching network interface, focusing on communication pat-
terns representative of MPI’s send-receive semantics. In con-
trast to ptl_bench, which relies on persistent list entries
regardless of matching behavior, this benchmark emulates
more dynamic, per-message matching scenarios.
It uses two portal indices: one for the command channel
and another for data transfer. On the target side, multiple list
entries are created, each associated with a distinct memory
buffer as specified by the window_size parameter. The
initiator begins transmission after receiving a notification via
the command channel. Messages are matched to list entries
using configured match bits, and upon successful transmis-
sion, indicated by full events such as PTL_EVENT_PUT
or PTL_EVENT_GET, the corresponding list entries are re-
moved. This process repeats for each iteration.

- **ptl_memory_bench:** This benchmark evaluates the BXI
NIC’s virtual-to-physical address translation performance by
measuring page fault latency in both local and remote access
scenarios. It supports both one-sided and ping-pong commu-
nication patterns, using PtlPut and PtlGet operations.
Memory is allocated using mmap to create page-sized virtual
segments, relying on deferred physical allocation. This enables
configuration of whether page faults are handled by the NIC or
the host. Placement follows a first-touch policy, where initial
access triggers physical allocation. Pages are classified as Hot
(pre-allocated) or Cold (unallocated). Messages are constraint
to 4 kB or less, with transfers targeting random offsets within
each page. The ping-pong pattern mirrors the one-sided setup,
enabling comparative analysis of translation overhead.

- **ptl_ping_pong:** This benchmark measures Round-Trip
Time (RTT) using a ping-pong communication scheme. It
highlights Portals4’s Triggered Operations, in which standard
calls like PtlPut and PtlGet are deferred until a CT
reaches a defined threshold, allowing the NIC to execute
the operation autonomously, bypassing CPU involvement. The
benchmark compares RTT between standard PtlPut and its
triggered variant PtlTriggeredPut, and also quantifies the
additional setup latency introduced by the trigger mechanism.

- **ptl_get_ni_props:** As a hardware implementation of
Portals4, BXI imposes inherent limitations on available
resources. Portals4 allows customization of these limits by
providing a pointer to a plt_ni_limits_t structure
during NI creation. This structure can be initialized with
INT_MAX, LONG_MAX, or zero before proceeding with NI
initialization. The actual resource limits imposed by the
Portals4 implementation are then retrieved from a separate
structure after initialization.

### How to build
To build PtlBench, ensure that both an MPI implementation (such as OpenMPI) and the Portals4 library are installed and accessible on your system.

```
$ mkdir build && cd build
$ cmake -DCMAKE_INSTALL_PREFIX=<INSTALL DEST> ..
$ make -j
$ make install
```

### How to run
```
$ mpirun -np 2 ./ptl_bench
```

### Available Parameters
Each benchmark provides a specialized set of available parameters. use ```-h, --help``` to list them:
```
$./ptl_bench --help
Usage: [options]
Options:
  -b, --bandwidth                Enable bandwidth mode (no argument required)
  -g, --get                      Enable get operation (no argument required)
  -i, --iterations <value>       Specify the number of iterations (required argument)
  -x, --warmup <value>           Specify the number of warmup iterations (required argument)
  --msg_size <value>             Specify the message size (required argument)
  --min_msg_size <value>         Specify the minimum message size (required argument)
  --max_msg_size <value>         Specify the maximum message size (required argument)
  -w, --window_size <value>      Specify the window size (required argument)
  -c, --cache_size <value>       Specify the cache size (required argument)
  --cold_cache                   Enable cold cache mode with specified cache size (required argument)
  -f, --full                     Enable full event mode (no argument required)
  -h, --help                     Display this help message (no argument required)
```

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

