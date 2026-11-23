#include "common.h"
#include "util.h"
#include <getopt.h>
#include <sys/mman.h>
#include <time.h>

#define MAX_PING_PONG_BYTES (4 * 1024)

static int table_output;
static int stats_output;
static int rank;
static int num_ranks;
static p4_ctx_t ctx;
static memory_benchmark_opts_t opts;
static size_t page_size;
static unsigned int seed = 1;
static char processor_name[MPI_MAX_PROCESSOR_NAME];

int* cache_buffer;
size_t cache_buffer_size;
void** page_buffer;
int (*communicate)(const ptl_handle_md_t md_h, const ptl_size_t offset,
                   const ptl_pt_index_t index, const ptl_ack_req_t req_type);

int
get_random_index()
{
  int blocks = page_size / opts.msg_size;
  return rand() % blocks;
}

void
touch_cold_pages(const int n_pages)
{
  size_t ints_per_page = page_size / sizeof(int);
  int** buffer = (int**)page_buffer;
  for(int p = 0; p < n_pages; ++p)
    for(size_t i = 0; i < ints_per_page; ++i)
    {
      buffer[p][i] = i;
    }
}

void
get_cold_pages(const int n_pages)
{
  for(int p = 0; p < n_pages; ++p)
  {
    page_buffer[p] = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(page_buffer[p] == MAP_FAILED)
      bench_fail("mmap failed");
  }
}

void
free_cold_pages(const int n_pages)
{
  size_t page_size = sysconf(_SC_PAGESIZE);
  for(int i = 0; i < n_pages; ++i)
  {
    if(munmap(page_buffer[i], page_size) != 0)
      bench_fail("munmap failed");
  }
}

int
put_operation(const ptl_handle_md_t md_h, const ptl_size_t offset,
              const ptl_pt_index_t index, const ptl_ack_req_t req_type)
{
  int eret = -1;
  eret = PtlPut(md_h, offset, opts.msg_size, req_type, ctx.peer_addr, index, 0,
                offset, NULL, 0);
  return eret;
}

int
get_operation(const ptl_handle_md_t md_h, const ptl_size_t offset,
              const ptl_pt_index_t index, const ptl_ack_req_t req_type)
{
  (void)req_type;
  int eret = -1;

  eret = PtlGet(md_h, offset, opts.msg_size, ctx.peer_addr, index, 0, offset,
                NULL);

  return eret;
}

static void
output_samples(double* samples)
{
  if(stats_output)
  {
    report_stats(opts.msg_size, "latency",
                 opts.pattern == PINGPONG ? "us (round trip)"
                                          : "us (operation completion)",
                 samples, opts.iterations);
    return;
  }

  for(int i = 0; i < opts.iterations; ++i)
    printf(table_output ? "%-9s %-10s %-5s %-6s %7i %14.4f\n"
                        : "%s,%s,%s,%s,%i,%.4f\n",
           opts.op == PUT ? "PtlPut" : "PtlGet",
           opts.pattern == PINGPONG ? "ping_pong" : "one_sided",
           opts.local_state == COLD ? "cold" : "hot",
           opts.remote_state == COLD ? "cold" : "hot", opts.msg_size,
           samples[i]);
}

void
run_one_sided_benchmark()
{
  ptl_handle_le_t le_h;
  ptl_handle_md_t md_h;
  ptl_pt_index_t index;
  ptl_event_t event;
  double* samples = rank == 0 ? alloc_samples(opts.iterations) : NULL;
  communicate = opts.op == PUT ? &put_operation : &get_operation;

  P4_CHECK(p4_pt_alloc(&ctx, &index));

  if(0 == rank && !stats_output)
  {
    // print header
    fprintf(stdout, table_output ? (opts.pattern == PINGPONG
                                        ? "Operation Pattern    Local "
                                          "Remote   Bytes     RTT (us)\n"
                                        : "Operation Pattern    Local "
                                          "Remote   Bytes Completion (us)\n")
                                 : "op,benchmark,local_page_state,remote_"
                                   "page_state,msg_size,latency\n");
  }

  for(int i = 0; i < opts.iterations; ++i)
  {
    get_cold_pages(1);

    if(1 == rank)
    {
      P4_CHECK(p4_le_insert(&ctx, &le_h, page_buffer[0], page_size, index));
      if(HOT == opts.remote_state)
      {
        touch_cold_pages(1);
      }
    }
    else
    {
      P4_CHECK(p4_md_alloc_eq(&ctx, &md_h, page_buffer[0], page_size));
      if(HOT == opts.local_state)
      {
        touch_cold_pages(1);
      }
    }

    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

    if(0 == rank)
    {
      ptl_size_t block_offset = get_random_index() * opts.msg_size;
      invalidate_cache(cache_buffer, cache_buffer_size);

      double t0 = MPI_Wtime();

      P4_CHECK(communicate(md_h, block_offset, index, PTL_ACK_REQ));

      wait_event(ctx.eq_h,
                 opts.pattern == PINGPONG
                     ? PTL_EVENT_PUT
                     : (opts.op == PUT ? PTL_EVENT_ACK : PTL_EVENT_REPLY),
                 &event);
      samples[i] = (MPI_Wtime() - t0) * 1e6;
      p4_md_free(md_h);
    }
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
    if(1 == rank)
    {
      p4_le_remove(le_h);
    }
    free_cold_pages(1);
  }
  p4_pt_free(&ctx, index);
  if(rank == 0)
    output_samples(samples);
  free(samples);
}

void
run_ping_pong_benchmark()
{
  ptl_handle_le_t le_h;
  ptl_handle_md_t md_h;
  ptl_pt_index_t index;
  ptl_event_t event;
  double* samples = rank == 0 ? alloc_samples(opts.iterations) : NULL;
  communicate = &put_operation;

  P4_CHECK(p4_pt_alloc(&ctx, &index));

  MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

  if(0 == rank && !stats_output)
  {
    // print header
    fprintf(stdout, table_output ? (opts.pattern == PINGPONG
                                        ? "Operation Pattern    Local "
                                          "Remote   Bytes     RTT (us)\n"
                                        : "Operation Pattern    Local "
                                          "Remote   Bytes Completion (us)\n")
                                 : "op,benchmark,local_page_state,remote_"
                                   "page_state,msg_size,latency\n");
  }

  for(int i = 0; i < opts.iterations; ++i)
  {
    get_cold_pages(1);

    P4_CHECK(
        p4_le_insert_full_comm(&ctx, &le_h, page_buffer[0], page_size, index));
    if(1 == rank && HOT == opts.remote_state)
    {
      touch_cold_pages(1);
    }
    ptl_md_t md = {.start = page_buffer[0],
                   .length = page_size,
                   .options = PTL_MD_EVENT_SEND_DISABLE | PTL_MD_EVENT_CT_SEND,
                   .ct_handle = ctx.ct_h,
                   .eq_handle = ctx.eq_h};
    if((ptl_size_t)opts.msg_size <= ctx.limits.max_volatile_size)
      md.options |= PTL_MD_VOLATILE;
    P4_CHECK(PtlMDBind(ctx.ni_h, &md, &md_h));
    if(0 == rank && HOT == opts.local_state)
    {
      touch_cold_pages(1);
    }

    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

    if(0 == rank)
    {
      ptl_size_t block_offset = get_random_index() * opts.msg_size;
      invalidate_cache(cache_buffer, cache_buffer_size);

      double t0 = MPI_Wtime();

      P4_CHECK(communicate(md_h, block_offset, index, PTL_NO_ACK_REQ));

      wait_event(ctx.eq_h,
                 opts.pattern == PINGPONG
                     ? PTL_EVENT_PUT
                     : (opts.op == PUT ? PTL_EVENT_ACK : PTL_EVENT_REPLY),
                 &event);

      samples[i] = (MPI_Wtime() - t0) * 1e6;
    }
    else
    {
      wait_event(ctx.eq_h,
                 opts.pattern == PINGPONG
                     ? PTL_EVENT_PUT
                     : (opts.op == PUT ? PTL_EVENT_ACK : PTL_EVENT_REPLY),
                 &event);

      P4_CHECK(communicate(md_h, event.remote_offset, index, PTL_NO_ACK_REQ));
    }

    // The responder sends only after the request PUT completes at the target.
    // Drain local SEND completion separately before releasing the descriptor.
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
    wait_count(ctx.ct_h, (ptl_size_t)i + 1);
    p4_md_free(md_h);
    p4_le_remove(le_h);
    free_cold_pages(1);
  }
  p4_pt_free(&ctx, index);
  if(rank == 0)
    output_samples(samples);
  free(samples);
}

void
print_help_message()
{
  printf("Usage: ptl_memory_bench [options]\n");
  printf("Options:\n");
  printf("  -g, --get                 One-sided GET instead of PUT\n");
  printf("  -p, --ping_pong           PUT ping-pong (RTT, up to 4 KiB)\n");
  printf("  --seed N                  Random offset seed (default 1)\n");
  printf("  --table                   Human-readable output (default "
         "CSV)\n");
  printf("  -i, --iterations <num>    Specify the number of iterations "
         "(required argument)\n");
  printf("  -c, --cache-size <size>   Set the cache size in MiB (required "
         "argument)\n");
  printf("  -l, --local-hot           Enable local-hot mode (no "
         "argument)\n");
  printf("  -r, --remote-hot          Enable remote-hot mode (no argument)\n");
  printf("  -m, --msg_size <size>     Set the message size in bytes (required "
         "argument)\n");
  printf("  -h, --help                Display this help message (no "
         "argument)\n");
  puts("  --stats                   Report mean, median, min and max as CSV");
}

int
main(int argc, char* argv[])
{
  int eret = -1;

  static const struct option long_opts[] = {
      {"seed", required_argument, NULL, 1001},
      {"iterations", required_argument, NULL, 'i'},
      {"cache-size", required_argument, NULL, 'c'},
      {"local-hot", no_argument, NULL, 'l'},
      {"remote-hot", no_argument, NULL, 'r'},
      {"get", no_argument, NULL, 'g'},
      {"msg_size", required_argument, NULL, 'm'},
      {"ping_pong", no_argument, NULL, 'p'},
      {"table", no_argument, NULL, 1000},
      {"stats", no_argument, NULL, BENCH_STATS_OPTION},
      {"help", no_argument, NULL, 'h'},
      {0, 0, 0, 0}};

  const char* const short_opts = "i:c:m:lrpgh";

  opts.iterations = 10;
  opts.msg_size = 512;
  opts.cache_size = _16MiB;
  opts.remote_state = COLD;
  opts.local_state = COLD;
  opts.op = PUT;
  opts.pattern = ONE_SIDED;

  while(1)
  {
    const int opt = getopt_long(argc, argv, short_opts, long_opts, NULL);
    if(-1 == opt)
    {
      break;
    }
    switch(opt)
    {
    case 1001:
      seed = (unsigned int)parse_int(optarg);
      break;
    case 'i':
      opts.iterations = parse_int(optarg);
      break;
    case 'c':
      opts.cache_size = parse_mib(optarg);
      break;
    case 'm':
      opts.msg_size = parse_int(optarg);
      break;
    case 'g':
      opts.op = GET;
      break;
    case 'p':
      opts.pattern = PINGPONG;
      break;
    case 'l':
      opts.local_state = HOT;
      break;
    case 'r':
      opts.remote_state = HOT;
      break;
    case 1000:
      table_output = 1;
      break;
    case BENCH_STATS_OPTION:
      stats_output = 1;
      break;
    case 'h':
      print_help_message();
      exit(EXIT_SUCCESS);
    case '?':
      print_help_message();
      exit(EXIT_FAILURE);
    default:
      print_help_message();
      exit(EXIT_FAILURE);
    }
  }

  int name_len;
  if(optind != argc)
    option_error("Unexpected positional argument");
  if(stats_output && table_output)
    option_error("Choose either --stats or --table");
  validate_iterations(opts.iterations, opts.warmup);
  if(opts.pattern == PINGPONG && opts.msg_size > MAX_PING_PONG_BYTES)
    option_error("Memory ping-pong message size must be at most 4096 bytes");
  long system_page_size = sysconf(_SC_PAGESIZE);
  if(system_page_size <= 0 || opts.msg_size <= 0 ||
     opts.msg_size > system_page_size)
    option_error("Message must fit within one system page");
  if(opts.pattern == PINGPONG && opts.op == GET)
    option_error("Ping-pong supports PUT only");
  MPI_CHECK(MPI_Init(&argc, &argv));
  MPI_CHECK(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  MPI_CHECK(MPI_Comm_size(MPI_COMM_WORLD, &num_ranks));
  MPI_CHECK(MPI_Get_processor_name(processor_name, &name_len));

  if(2 != num_ranks)
  {
    fprintf(stderr, "Benchmark requires exactly two processes\n");
    bench_fail("Benchmark failed; see preceding diagnostic");
  }

  fprintf(stderr, "Proc %i on %s\n", rank, processor_name);

  configure_cache_pids(-1);
  P4_CHECK(PtlInit());
  eret = init_p4_ctx(&ctx, NON_MATCHING);
  if(PTL_OK != eret)
    bench_fail("Benchmark initialization failed");

  eret = exchange_ni_address(&ctx, rank);

  if(0 != eret)
    bench_fail("Benchmark initialization failed");

  cache_buffer = malloc(opts.cache_size);
  if(NULL == cache_buffer)
    bench_fail("Benchmark initialization failed");
  cache_buffer_size = opts.cache_size / sizeof(int);

  page_size = (size_t)system_page_size;
  check_message_size(&ctx, opts.msg_size);

  page_buffer = malloc(sizeof(void*));
  if(NULL == page_buffer)
    bench_fail("Benchmark initialization failed");

  srand(seed);
  if(rank == 0)
    fprintf(stderr,
            "op=%s pattern=%s ni=non-matching events=full bytes=%d "
            "window=1 iterations=%d warmup=0 local=%s remote=%s "
            "cache_sweep_bytes=%zu page_bytes=%zu seed=%u; time in us\n",
            opts.op == PUT ? "put" : "get",
            opts.pattern == PINGPONG ? "ping-pong RTT" : "one-sided completion",
            opts.msg_size, opts.iterations,
            opts.local_state == HOT ? "hot" : "cold",
            opts.remote_state == HOT ? "hot" : "cold", opts.cache_size,
            page_size, seed);

  if(rank == 0 && stats_output)
    puts(BENCH_STATS_HEADER);

  if(ONE_SIDED == opts.pattern)
    run_one_sided_benchmark();
  else if(PINGPONG == opts.pattern)
    run_ping_pong_benchmark();

  free(cache_buffer);
  free(page_buffer);
  destroy_p4_ctx(&ctx);
  PtlFini();
  MPI_CHECK(MPI_Finalize());
  return eret;
}
