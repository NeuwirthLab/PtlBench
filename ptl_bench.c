#include "common.h"
#include "util.h"
#include <getopt.h>

static int table_output;
static int stats_output;
static int rank;
static int num_ranks;
static benchmark_opts_t opts;
static p4_ctx_t ctx;
static double* samples;

int* cache_buffer;
size_t cache_buffer_size;

static inline void
wait_for_completion(const ptl_size_t wait_for)
{
  if(COUNTING == opts.event_type)
    wait_count(ctx.ct_h, wait_for);
  else
  {
    ptl_event_t event;
    for(ptl_size_t i = 0; i < wait_for; ++i)
      wait_event(ctx.eq_h, opts.op == PUT ? PTL_EVENT_ACK : PTL_EVENT_REPLY,
                 &event);
  }
}

static void
output_samples(size_t msg_size)
{
  for(int i = 0; i < opts.iterations; ++i)
    samples[i] = opts.type == LATENCY
                     ? samples[i] * 1e6
                     : (msg_size * opts.window_size * 1e-6) / samples[i];

  if(stats_output)
  {
    if(opts.type == LATENCY)
      report_stats(msg_size, "latency", "us (operation completion)", samples,
                   opts.iterations);
    else
      report_stats(msg_size, "bandwidth", "MB/s (decimal)", samples,
                   opts.iterations);
  }
  else
  {
    const char* op = opts.op == PUT ? "put" : "get";
    for(int i = 0; i < opts.iterations; ++i)
      printf(table_output ? "%s     %10zu    %16.4f\n" : "%s,%zu,%.4f\n", op,
             msg_size, samples[i]);
  }
  fflush(stdout);
}

int
p4_put_latency()
{
  int eret = -1;
  ptl_handle_md_t md_h;
  ptl_handle_le_t le_h;
  ptl_handle_me_t me_h;
  ptl_pt_index_t index;
  ptl_ct_event_t zero = {.success = 0, .failure = 0};
  void* buffer = NULL;
  double t0 = 0.0;

  eret = p4_pt_alloc(&ctx, &index);
  if(PTL_OK != eret)
    P4_CHECK(eret);
  ptl_match_bits_t match_bits = opts.ni_mode == MATCHING ? 0xDEADBEEF : 0;

  // print header
  if(0 == rank && !stats_output)
    fprintf(stdout, table_output ? "Operation    Bytes    Completion (us)\n"
                                 : "func,msg_size,latency\n");

  for(size_t msg_size = opts.min_msg_size; msg_size <= opts.max_msg_size;
      msg_size *= 2)
  {
    eret = alloc_buffer_init(&buffer, msg_size);
    if(0 != eret)
      P4_CHECK(eret);

    if(1 == rank)
    {
      memset(buffer, 'd', msg_size);
      if(MATCHING == opts.ni_mode)
      {
        eret = p4_me_insert_persistent(&ctx, &me_h, buffer, msg_size, index);
      }
      else
      {
        eret = p4_le_insert(&ctx, &le_h, buffer, msg_size, index);
      }
      if(PTL_OK != eret)
        P4_CHECK(eret);
    }

    P4_CHECK(eret);
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

    if(0 == rank)
    {
      if(COUNTING == opts.event_type)
        eret = p4_md_alloc_ct(&ctx, &md_h, buffer, msg_size);
      else
        eret = p4_md_alloc_eq(&ctx, &md_h, buffer, msg_size);
      if(PTL_OK != eret)
        P4_CHECK(eret);

      for(int i = 0; i < opts.iterations + opts.warmup; ++i)
      {
        if(opts.cache_state == COLD_CACHE)
        {
          invalidate_cache(cache_buffer, cache_buffer_size);
        }
        if(i >= opts.warmup)
        {
          t0 = MPI_Wtime();
        }

        eret = PtlPut(md_h, 0, msg_size, PTL_ACK_REQ, ctx.peer_addr, index,
                      match_bits, 0, NULL, 0);

        if(PTL_OK != eret)
          P4_CHECK(eret);

        wait_for_completion(opts.event_type == COUNTING ? (ptl_size_t)i + 1
                                                        : 1);

        if(i >= opts.warmup)
        {
          samples[i - opts.warmup] = MPI_Wtime() - t0;
        }
      }
      if(0 == rank && COUNTING == opts.event_type)
      {
        eret = PtlCTSet(ctx.ct_h, zero);
        if(PTL_OK != eret)
          P4_CHECK(eret);
      }
      p4_md_free(md_h);
    }
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
    if(1 == rank)
    {
      if(MATCHING == opts.ni_mode)
        p4_me_remove(me_h);
      else
        p4_le_remove(le_h);
    }
    if((opts.op == PUT && rank == 1) || (opts.op == GET && rank == 0))
      validate_buffer(buffer, msg_size, opts.op == PUT ? 'c' : 'd');
    free(buffer);
    if(rank == 0)
      output_samples(msg_size);
  }
  p4_pt_free(&ctx, index);
  return 0;
}

int
p4_get_latency()
{
  int eret = -1;
  ptl_handle_md_t md_h;
  ptl_handle_le_t le_h;
  ptl_handle_me_t me_h;
  ptl_pt_index_t index;
  ptl_ct_event_t zero = {.success = 0, .failure = 0};
  void* buffer = NULL;
  double t0 = 0.0;

  eret = p4_pt_alloc(&ctx, &index);
  if(PTL_OK != eret)
    P4_CHECK(eret);
  ptl_match_bits_t match_bits = opts.ni_mode == MATCHING ? 0xDEADBEEF : 0;
  // print header
  if(0 == rank && !stats_output)
    fprintf(stdout, table_output ? "Operation    Bytes    Completion (us)\n"
                                 : "func,msg_size,latency\n");

  for(size_t msg_size = opts.min_msg_size; msg_size <= opts.max_msg_size;
      msg_size *= 2)
  {
    eret = alloc_buffer_init(&buffer, msg_size);
    if(0 != eret)
      P4_CHECK(eret);

    if(1 == rank)
    {
      memset(buffer, 'd', msg_size);
      if(MATCHING == opts.ni_mode)
      {
        eret = p4_me_insert_persistent(&ctx, &me_h, buffer, msg_size, index);
      }
      else
      {
        eret = p4_le_insert(&ctx, &le_h, buffer, msg_size, index);
      }
    }

    P4_CHECK(eret);
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

    if(0 == rank)
    {
      if(COUNTING == opts.event_type)
        eret = p4_md_alloc_ct(&ctx, &md_h, buffer, msg_size);
      else
        eret = p4_md_alloc_eq(&ctx, &md_h, buffer, msg_size);
      if(PTL_OK != eret)
        P4_CHECK(eret);

      for(int i = 0; i < opts.iterations + opts.warmup; ++i)
      {
        if(opts.cache_state == COLD_CACHE)
        {
          invalidate_cache(cache_buffer, cache_buffer_size);
        }
        if(i >= opts.warmup)
        {
          t0 = MPI_Wtime();
        }
        eret = PtlGet(md_h, 0, msg_size, ctx.peer_addr, index, match_bits, 0,
                      NULL);
        if(PTL_OK != eret)
          P4_CHECK(eret);

        wait_for_completion(opts.event_type == COUNTING ? (ptl_size_t)i + 1
                                                        : 1);

        if(i >= opts.warmup)
        {
          samples[i - opts.warmup] = MPI_Wtime() - t0;
        }
      }
      if(0 == rank && COUNTING == opts.event_type)
      {
        eret = PtlCTSet(ctx.ct_h, zero);
        if(PTL_OK != eret)
          P4_CHECK(eret);
      }
      p4_md_free(md_h);
    }
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
    if(1 == rank)
    {
      if(MATCHING == opts.ni_mode)
        p4_me_remove(me_h);
      else
        p4_le_remove(le_h);
    }
    if((opts.op == PUT && rank == 1) || (opts.op == GET && rank == 0))
      validate_buffer(buffer, msg_size, opts.op == PUT ? 'c' : 'd');
    free(buffer);
    if(rank == 0)
      output_samples(msg_size);
  }
  p4_pt_free(&ctx, index);
  return 0;
}

int
p4_put_bandwidth()
{
  int eret = -1;
  ptl_handle_md_t md_h;
  ptl_handle_le_t le_h;
  ptl_handle_me_t me_h;
  ptl_size_t offset;
  ptl_pt_index_t index;
  ptl_ct_event_t zero = {.success = 0, .failure = 0};
  void* buffer = NULL;
  double t0 = 0.0;

  eret = p4_pt_alloc(&ctx, &index);
  if(PTL_OK != eret)
    P4_CHECK(eret);
  ptl_match_bits_t match_bits = opts.ni_mode == MATCHING ? 0xDEADBEEF : 0;
  // print header
  if(0 == rank && !stats_output)
    fprintf(stdout, table_output ? "Operation    Bytes    Bandwidth (MB/s)\n"
                                 : "func,msg_size,bandwidth\n");

  for(size_t msg_size = opts.min_msg_size; msg_size <= opts.max_msg_size;
      msg_size *= 2)
  {
    size_t bytes = opts.window_size * msg_size;
    eret = alloc_buffer_init(&buffer, bytes);
    if(0 != eret)
      P4_CHECK(eret);

    if(1 == rank)
    {
      memset(buffer, 'd', bytes);
      if(MATCHING == opts.ni_mode)
      {
        eret = p4_me_insert_persistent(&ctx, &me_h, buffer, bytes, index);
      }
      else
      {
        eret = p4_le_insert(&ctx, &le_h, buffer, bytes, index);
      }
      if(PTL_OK != eret)
        P4_CHECK(eret);
    }

    P4_CHECK(eret);
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

    if(0 == rank)
    {
      if(COUNTING == opts.event_type)
        eret = p4_md_alloc_ct(&ctx, &md_h, buffer, bytes);
      else
        eret = p4_md_alloc_eq(&ctx, &md_h, buffer, bytes);
      if(PTL_OK != eret)
        P4_CHECK(eret);

      for(int i = 0; i < opts.iterations + opts.warmup; ++i)
      {
        if(opts.cache_state == COLD_CACHE)
        {
          invalidate_cache(cache_buffer, cache_buffer_size);
        }
        if(i >= opts.warmup)
        {
          t0 = MPI_Wtime();
        }
        for(int w = 0; w < opts.window_size; ++w)
        {
          offset = w * msg_size;
          eret = PtlPut(md_h, offset, msg_size, PTL_ACK_REQ, ctx.peer_addr,
                        index, match_bits, offset, NULL, 0);
          if(PTL_OK != eret)
            P4_CHECK(eret);
        }

        wait_for_completion(opts.window_size);

        if(i >= opts.warmup)
        {
          samples[i - opts.warmup] = MPI_Wtime() - t0;
        }
        if(0 == rank && COUNTING == opts.event_type)
        {
          eret = PtlCTSet(ctx.ct_h, zero);
          if(PTL_OK != eret)
            P4_CHECK(eret);
        }
      }
      p4_md_free(md_h);
    }
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
    if(1 == rank)
    {
      if(MATCHING == opts.ni_mode)
        p4_me_remove(me_h);
      else
        p4_le_remove(le_h);
    }
    if((opts.op == PUT && rank == 1) || (opts.op == GET && rank == 0))
      validate_buffer(buffer, bytes, opts.op == PUT ? 'c' : 'd');
    free(buffer);
    if(rank == 0)
      output_samples(msg_size);
  }
  p4_pt_free(&ctx, index);
  return 0;
}

int
p4_get_bandwidth()
{
  int eret = -1;
  ptl_handle_md_t md_h;
  ptl_handle_le_t le_h;
  ptl_handle_me_t me_h;
  ptl_size_t offset;
  ptl_pt_index_t index;
  ptl_ct_event_t zero = {.success = 0, .failure = 0};
  void* buffer = NULL;
  double t0 = 0.0;

  eret = p4_pt_alloc(&ctx, &index);
  if(PTL_OK != eret)
    P4_CHECK(eret);
  ptl_match_bits_t match_bits = opts.ni_mode == MATCHING ? 0xDEADBEEF : 0;
  // print header
  if(0 == rank && !stats_output)
    fprintf(stdout, table_output ? "Operation    Bytes    Bandwidth (MB/s)\n"
                                 : "func,msg_size,bandwidth\n");

  for(size_t msg_size = opts.min_msg_size; msg_size <= opts.max_msg_size;
      msg_size *= 2)
  {
    size_t bytes = opts.window_size * msg_size;
    eret = alloc_buffer_init(&buffer, bytes);
    if(0 != eret)
      P4_CHECK(eret);

    if(1 == rank)
    {
      memset(buffer, 'd', bytes);
      if(MATCHING == opts.ni_mode)
      {
        eret = p4_me_insert_persistent(&ctx, &me_h, buffer, bytes, index);
      }
      else
      {
        eret = p4_le_insert(&ctx, &le_h, buffer, bytes, index);
      }
    }

    P4_CHECK(eret);
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

    if(0 == rank)
    {
      if(COUNTING == opts.event_type)
        eret = p4_md_alloc_ct(&ctx, &md_h, buffer, bytes);
      else
        eret = p4_md_alloc_eq(&ctx, &md_h, buffer, bytes);
      if(PTL_OK != eret)
        P4_CHECK(eret);

      for(int i = 0; i < opts.iterations + opts.warmup; ++i)
      {
        if(opts.cache_state == COLD_CACHE)
        {
          invalidate_cache(cache_buffer, cache_buffer_size);
        }
        if(i >= opts.warmup)
        {
          t0 = MPI_Wtime();
        }
        for(int w = 0; w < opts.window_size; ++w)
        {
          offset = w * msg_size;
          eret = PtlGet(md_h, offset, msg_size, ctx.peer_addr, index,
                        match_bits, offset, NULL);
          if(PTL_OK != eret)
            P4_CHECK(eret);
        }

        wait_for_completion(opts.window_size);

        if(i >= opts.warmup)
        {
          samples[i - opts.warmup] = MPI_Wtime() - t0;
        }

        if(0 == rank && COUNTING == opts.event_type)
        {
          eret = PtlCTSet(ctx.ct_h, zero);
          if(PTL_OK != eret)
            P4_CHECK(eret);
        }
      }
      p4_md_free(md_h);
    }
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
    if(1 == rank)
    {
      if(MATCHING == opts.ni_mode)
        p4_me_remove(me_h);
      else
        p4_le_remove(le_h);
    }
    if((opts.op == PUT && rank == 1) || (opts.op == GET && rank == 0))
      validate_buffer(buffer, bytes, opts.op == PUT ? 'c' : 'd');
    free(buffer);
    if(rank == 0)
      output_samples(msg_size);
  }
  p4_pt_free(&ctx, index);
  return 0;
}

void
print_help_message()
{
  fprintf(stdout, "Usage: ptl_bench [options]\n");
  fprintf(stdout, "Options:\n");
  fprintf(stdout, "  -m, --matching                 Enable matching mode (no "
                  "argument required)\n");
  fprintf(stdout, "  -b, --bandwidth                Enable bandwidth mode (no "
                  "argument required)\n");
  fprintf(stdout, "  -g, --get                      Enable get operation (no "
                  "argument required)\n");
  fprintf(stdout,
          "  -i, --iterations <value>       Specify the number of iterations "
          "(required argument)\n");
  fprintf(stdout,
          "  -x, --warmup <value>           Specify the number of warmup "
          "iterations (required argument)\n");
  fprintf(stdout, "  --msg_size <value>             Specify the message size "
                  "(required argument)\n");
  fprintf(stdout,
          "  --min_msg_size <value>         Specify the minimum message size "
          "(required argument)\n");
  fprintf(stdout,
          "  --max_msg_size <value>         Specify the maximum message size "
          "(required argument)\n");
  fprintf(stdout, "  -w, --window_size <value>      Specify the window size "
                  "(required argument)\n");
  fprintf(
      stdout,
      "  -c, --cache_size <value>       Specify cache size in MiB (required "
      "argument)\n");
  fprintf(
      stdout,
      "  --cold_cache 	              Enable cache sweep (no argument)\n");
  fprintf(stdout,
          "  -f, --full                     Enable full mode (no argument "
          "required)\n");
  fprintf(stdout,
          "  -h, --help                     Display this help message (no "
          "argument required)\n");
  printf(
      "  --table                       Human-readable output (default: CSV)\n");
  printf("  -p, --pids <value>             Set BXI cache regions "
         "(site-specific)\n");
  fflush(stdout);
  puts("  --stats                   Report mean, median, min and max as CSV");
}

void
print_benchmark_opts()
{
  fprintf(stderr, "Benchmark Configuration:\n\n");
  fprintf(stderr, "ni_mode: %s\n",
          opts.ni_mode == MATCHING ? "MATCHING" : "NON MATCHING");
  fprintf(stderr, "op: %s\n", opts.op == PUT ? "PUT" : "GET");
  fprintf(stderr, "type: %s\n", opts.type == LATENCY ? "LATENCY" : "BANDWIDTH");
  fprintf(stderr, "event_type: %s\n",
          opts.event_type == COUNTING ? "COUNTING" : "FULL");
  fprintf(stderr, "iterations: %i\n", opts.iterations);
  fprintf(stderr, "warmup: %i\n", opts.warmup);
  fprintf(stderr, "window_size: %i\n",
          opts.type == LATENCY ? 1 : opts.window_size);
  fprintf(stderr, "min_msg_size: %zu\n", opts.min_msg_size);
  fprintf(stderr, "max_msg_size: %zu\n", opts.max_msg_size);
  fprintf(stderr, "cache_size: %zu\n", opts.cache_size);
  fprintf(stderr, "cache_state: %s\n\n",
          opts.cache_state == COLD_CACHE ? "COLD_CACHE" : "HOT_CACHE");
  fprintf(stderr, "units: bytes, completion latency in us, decimal MB/s\n");
  fflush(stderr);
}

int
main(int argc, char* argv[])
{
  int eret = -1;
  int cache_regions = -1;

  static const struct option long_opts[] = {
      {"matching", no_argument, NULL, 'm'},
      {"bandwidth", no_argument, NULL, 'b'},
      {"get", no_argument, NULL, 'g'},
      {"iterations", required_argument, NULL, 'i'},
      {"warmup", required_argument, NULL, 'x'},
      {"msg_size", required_argument, NULL, 1},
      {"min_msg_size", required_argument, NULL, 2},
      {"max_msg_size", required_argument, NULL, 3},
      {"window_size", required_argument, NULL, 'w'},
      {"cache_size", required_argument, NULL, 'c'},
      {"cold_cache", no_argument, NULL, 4},
      {"full", no_argument, NULL, 'f'},
      {"pids", required_argument, NULL, 'p'},
      {"table", no_argument, NULL, 1000},
      {"stats", no_argument, NULL, BENCH_STATS_OPTION},
      {"help", no_argument, NULL, 'h'},
      {0, 0, 0, 0}};

  const char* const short_opts = "mbgi:x:w:c:fp:h";

  opts.ni_mode = NON_MATCHING;
  opts.op = PUT;
  opts.type = LATENCY;
  opts.iterations = 10;
  opts.warmup = 10;
  opts.window_size = 64;
  opts.msg_size = 1024;
  opts.min_msg_size = 1;
  opts.max_msg_size = 4194304;
  opts.event_type = COUNTING;
  opts.cache_size = _16MiB;
  opts.cache_state = HOT_CACHE;

  while(1)
  {
    const int opt = getopt_long(argc, argv, short_opts, long_opts, NULL);

    if(-1 == opt)
      break;

    switch(opt)
    {
    case 'm':
      opts.ni_mode = MATCHING;
      break;
    case 'b':
      opts.type = BANDWIDTH;
      break;
    case 'g':
      opts.op = GET;
      break;
    case 'i':
      opts.iterations = parse_int(optarg);
      break;
    case 'x':
      opts.warmup = parse_int(optarg);
      break;
    case 'c':
      opts.cache_size = parse_mib(optarg);
      break;
    case 'p':
      cache_regions = parse_int(optarg);
      break;
    case 1:
      opts.msg_size = parse_size(optarg);
      opts.min_msg_size = opts.msg_size;
      opts.max_msg_size = opts.msg_size;
      break;
    case 2:
      opts.min_msg_size = parse_size(optarg);
      break;
    case 3:
      opts.max_msg_size = parse_size(optarg);
      break;
    case 4:
      opts.cache_state = COLD_CACHE;
      break;
    case 'w':
      opts.window_size = parse_int(optarg);
      break;
    case 'f':
      opts.event_type = FULL;
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
  } // end while

  if(optind != argc)
    option_error("Unexpected positional argument");
  if(stats_output && table_output)
    option_error("Choose either --stats or --table");
  validate_iterations(opts.iterations, opts.warmup);
  validate_range(opts.min_msg_size, opts.max_msg_size, opts.window_size);
  if(opts.event_type == FULL && opts.window_size > 4096)
    option_error("Full-event window exceeds the 4096-entry EQ capacity");
  MPI_CHECK(MPI_Init(&argc, &argv));
  MPI_CHECK(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  MPI_CHECK(MPI_Comm_size(MPI_COMM_WORLD, &num_ranks));

  if(2 != num_ranks)
  {
    fprintf(stderr, "Benchmark requires exactly two processes\n");
    bench_fail("Benchmark failed; see preceding diagnostic");
  }

  configure_cache_pids(cache_regions);

  if(0 == rank)
    print_benchmark_opts();

  P4_CHECK(PtlInit());

  eret = init_p4_ctx(&ctx, opts.ni_mode);
  if(PTL_OK != eret)
  {
    fprintf(stderr, "init failed with %i\n", eret);
    bench_fail("Benchmark initialization failed");
  }

  check_message_size(&ctx, opts.max_msg_size);

  cache_buffer = malloc(opts.cache_size);
  if(NULL == cache_buffer)
    bench_fail("Benchmark initialization failed");

  cache_buffer_size = opts.cache_size / sizeof(int);

  eret = exchange_ni_address(&ctx, rank);
  if(0 != eret)
  {
    fprintf(stderr, " exchange failed\n");
    bench_fail("Benchmark initialization failed");
  }

  if(rank == 0)
  {
    samples = alloc_samples(opts.iterations);
    if(stats_output)
      puts(BENCH_STATS_HEADER);
  }

  if(LATENCY == opts.type)
  {
    if(opts.op == PUT)
    {
      eret = p4_put_latency();
    }
    else
    {
      eret = p4_get_latency();
    }
  }
  else if(BANDWIDTH == opts.type)
  {
    if(opts.op == PUT)
    {
      eret = p4_put_bandwidth();
    }
    else
    {
      eret = p4_get_bandwidth();
    }
  }

  P4_CHECK(eret);
  free(samples);
  free(cache_buffer);
  destroy_p4_ctx(&ctx);
  PtlFini();
  MPI_CHECK(MPI_Finalize());
  return eret;
}
