#include "common.h"
#include "util.h"
#include <getopt.h>

static int table_output;
static int stats_output;
static int rank;
static int num_ranks;
static p4_ctx_t ctx;
static benchmark_opts_t opts;
static char processor_name[MPI_MAX_PROCESSOR_NAME];

void
run_triggered_ping_pong_benchmark()
{
  ptl_handle_le_t le_h;
  ptl_handle_md_t md_h;
  ptl_pt_index_t index;
  ptl_handle_ct_t send_ct;
  void* send_buffer = NULL;
  int eret = -1;
  void* buffer = NULL;

  double t0;
  double* rtt =
      malloc((size_t)(opts.iterations + opts.warmup) * sizeof(double));
  double* setup =
      malloc((size_t)(opts.iterations + opts.warmup) * sizeof(double));
  if(!rtt || !setup)
    bench_fail("Sample allocation failed");

  eret = p4_pt_alloc(&ctx, &index);
  if(PTL_OK != eret)
  {
    P4_CHECK(eret);
  }

  if(0 == rank && !stats_output)
  {
    fprintf(stdout, table_output ? "Sample  Operation              Bytes       "
                                   "RTT (us)     Setup (us)\n"
                                 : "n,func,msg_size,rtt,setup_time\n");
  }

  alloc_buffer_init(&buffer, opts.msg_size);
  memset(buffer, 'r', opts.msg_size);
  alloc_buffer_init(&send_buffer, opts.msg_size);
  P4_CHECK(p4_le_insert_ct_comm(&ctx, &le_h, buffer, opts.msg_size, index));
  P4_CHECK(PtlCTAlloc(ctx.ni_h, &send_ct));
  // Count local sends separately: they must never advance receive triggers.
  ptl_md_t md = {.start = send_buffer,
                 .length = opts.msg_size,
                 .options = PTL_MD_EVENT_CT_SEND | PTL_MD_EVENT_SUCCESS_DISABLE,
                 .ct_handle = send_ct,
                 .eq_handle = PTL_EQ_NONE};
  P4_CHECK(PtlMDBind(ctx.ni_h, &md, &md_h));

  if(1 == rank)
  {
    for(int i = 1; i <= opts.iterations + opts.warmup; ++i)
    {
      t0 = MPI_Wtime();
      eret = PtlTriggeredPut(md_h, 0, opts.msg_size, PTL_NO_ACK_REQ,
                             ctx.peer_addr, index, 0, 0, NULL, 0, ctx.ct_h, i);
      if(PTL_OK != eret)
      {
        fprintf(stderr, "PtlTriggeredPut failed with %i\n", eret);
        P4_CHECK(eret);
      }
      setup[i - 1] = (MPI_Wtime() - t0) * 1e6;
    }
  }

  MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

  for(int i = 1; i <= opts.iterations + opts.warmup; ++i)
  {
    if(0 == rank)
    {
      t0 = MPI_Wtime();
      eret = PtlPut(md_h, 0, opts.msg_size, PTL_NO_ACK_REQ, ctx.peer_addr,
                    index, 0, 0, NULL, 0);

      if(PTL_OK != eret)
      {
        fprintf(stderr, "PtlPut failed with %i\n", eret);
        P4_CHECK(eret);
      }
      wait_count(ctx.ct_h, i);

      rtt[i - 1] = (MPI_Wtime() - t0) * 1e6;
    }
  }

  MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

  if(0 == rank)
    MPI_CHECK(MPI_Recv(setup, opts.iterations + opts.warmup, MPI_DOUBLE, 1, 0,
                       MPI_COMM_WORLD, MPI_STATUS_IGNORE));
  else
    MPI_CHECK(MPI_Send(setup, opts.iterations + opts.warmup, MPI_DOUBLE, 0, 0,
                       MPI_COMM_WORLD));

  if(0 == rank)
  {
    if(stats_output)
    {
      report_stats(opts.msg_size, "rtt", "us (round trip)", rtt + opts.warmup,
                   opts.iterations);
      report_stats(opts.msg_size, "setup_time", "us (trigger registration)",
                   setup + opts.warmup, opts.iterations);
    }
    else
    {
      for(int i = opts.warmup; i < opts.iterations + opts.warmup; ++i)
      {
        fprintf(stdout,
                table_output ? "%6i  PtlTriggeredPut  %10zu %14.4f %14.4f\n"
                             : "%i,PtlTriggeredPut,%zu,%.4f,%.4f\n",
                i - opts.warmup, opts.msg_size, rtt[i], setup[i]);
      }
    }
  }

  MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
  wait_count(send_ct, opts.iterations + opts.warmup);
  validate_buffer(buffer, opts.msg_size, 'c');
  p4_le_remove(le_h);
  p4_md_free(md_h);
  P4_CHECK(PtlCTFree(send_ct));
  free(send_buffer);
  free(buffer);
  free(rtt);
  free(setup);
  p4_pt_free(&ctx, index);
}

void
run_ping_pong_benchmark()
{
  ptl_handle_le_t le_h;
  ptl_handle_md_t md_h;
  ptl_pt_index_t index;
  ptl_handle_ct_t send_ct;
  void* send_buffer = NULL;
  int eret = -1;
  void* buffer = NULL;

  double t0;
  double* time =
      malloc((size_t)(opts.iterations + opts.warmup) * sizeof(double));
  if(!time)
    bench_fail("Sample allocation failed");

  eret = p4_pt_alloc(&ctx, &index);
  if(PTL_OK != eret)
  {
    P4_CHECK(eret);
  }

  if(0 == rank && !stats_output)
  {
    fprintf(stdout,
            table_output
                ? "Sample  Operation              Bytes       RTT (us)\n"
                : "n,func,msg_size,rtt\n");
  }

  alloc_buffer_init(&buffer, opts.msg_size);
  memset(buffer, 'r', opts.msg_size);
  alloc_buffer_init(&send_buffer, opts.msg_size);
  P4_CHECK(p4_le_insert_ct_comm(&ctx, &le_h, buffer, opts.msg_size, index));
  P4_CHECK(PtlCTAlloc(ctx.ni_h, &send_ct));
  // Count local sends separately: they must never advance receive triggers.
  ptl_md_t md = {.start = send_buffer,
                 .length = opts.msg_size,
                 .options = PTL_MD_EVENT_CT_SEND | PTL_MD_EVENT_SUCCESS_DISABLE,
                 .ct_handle = send_ct,
                 .eq_handle = PTL_EQ_NONE};
  P4_CHECK(PtlMDBind(ctx.ni_h, &md, &md_h));

  MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

  for(int i = 1; i <= opts.iterations + opts.warmup; ++i)
  {
    if(0 == rank)
    {
      t0 = MPI_Wtime();
      eret = PtlPut(md_h, 0, opts.msg_size, PTL_NO_ACK_REQ, ctx.peer_addr,
                    index, 0, 0, NULL, 0);

      if(PTL_OK != eret)
      {
        fprintf(stderr, "PtlPut failed with %i\n", eret);
        P4_CHECK(eret);
      }
      wait_count(ctx.ct_h, i);

      time[i - 1] = (MPI_Wtime() - t0) * 1e6;
    }
    else
    {
      wait_count(ctx.ct_h, i);

      eret = PtlPut(md_h, 0, opts.msg_size, PTL_NO_ACK_REQ, ctx.peer_addr,
                    index, 0, 0, NULL, 0);

      if(PTL_OK != eret)
      {
        fprintf(stderr, "PtlPut failed with %i\n", eret);
        P4_CHECK(eret);
      }
    }
  }

  if(0 == rank)
  {
    if(stats_output)
    {
      report_stats(opts.msg_size, "rtt", "us (round trip)", time + opts.warmup,
                   opts.iterations);
    }
    else
    {
      for(int i = opts.warmup; i < opts.iterations + opts.warmup; ++i)
      {
        fprintf(stdout,
                table_output ? "%6i  PtlPut           %10zu %14.4f\n"
                             : "%i,PtlPut,%zu,%.4f\n",
                i - opts.warmup, opts.msg_size, time[i]);
      }
    }
  }
  MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
  wait_count(send_ct, opts.iterations + opts.warmup);
  validate_buffer(buffer, opts.msg_size, 'c');
  p4_le_remove(le_h);
  p4_md_free(md_h);
  P4_CHECK(PtlCTFree(send_ct));
  free(send_buffer);
  free(buffer);
  free(time);
  p4_pt_free(&ctx, index);
}

void
print_help_message()
{
  printf("Usage: ptl_ping_pong [OPTIONS]\n");
  printf("Options:\n");
  printf("  -t, --triggered           Pre-post triggered replies\n");
  printf("  --table                   Human-readable output (default CSV)\n");
  printf("  -i, --iterations <arg>    Specify the number of iterations to run "
         "(required argument).\n");
  printf("  -w, --warmup <arg>        Specify the number of warmup iterations "
         "(required argument).\n");
  printf("  -m, --msg_size <arg>      Specify the message size in bytes "
         "(required argument).\n");
  printf("  -h, --help                Display this help message and exit.\n");
  puts("  --stats                   Report mean, median, min and max as CSV");
}

int
main(int argc, char* argv[])
{
  int eret = -1;
  static const struct option long_opts[] = {
      {"iterations", required_argument, NULL, 'i'},
      {"warmup", required_argument, NULL, 'w'},
      {"msg_size", required_argument, NULL, 'm'},
      {"triggered", no_argument, NULL, 't'},
      {"table", no_argument, NULL, 1000},
      {"stats", no_argument, NULL, BENCH_STATS_OPTION},
      {"help", no_argument, NULL, 'h'},
      {0, 0, 0, 0}};

  const char* const short_opts = "i:w:m:th";

  opts.iterations = 5000;
  opts.msg_size = 512;
  opts.cache_size = _16MiB;
  opts.warmup = 100;
  int triggered = 0;

  while(1)
  {
    const int opt = getopt_long(argc, argv, short_opts, long_opts, NULL);
    if(-1 == opt)
    {
      break;
    }
    switch(opt)
    {
    case 'i':
      opts.iterations = parse_int(optarg);
      break;
    case 'm':
      opts.msg_size = parse_size(optarg);
      break;
    case 'w':
      opts.warmup = parse_int(optarg);
      break;
    case 't':
      triggered = 1;
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
  validate_range(opts.msg_size, opts.msg_size, 1);
  MPI_CHECK(MPI_Init(&argc, &argv));
  MPI_CHECK(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  MPI_CHECK(MPI_Comm_size(MPI_COMM_WORLD, &num_ranks));
  MPI_CHECK(MPI_Get_processor_name(processor_name, &name_len));

  if(2 != num_ranks)
  {
    bench_fail("Benchmark requires exactly two processes");
  }

  configure_cache_pids(-1);
  P4_CHECK(PtlInit());
  eret = init_p4_ctx(&ctx, NON_MATCHING);
  if(PTL_OK != eret)
    bench_fail("Benchmark initialization failed");

  eret = exchange_ni_address(&ctx, rank);

  if(0 != eret)
    bench_fail("Benchmark initialization failed");

  check_message_size(&ctx, opts.msg_size);
  if(triggered && opts.iterations + opts.warmup > ctx.limits.max_triggered_ops)
  {
    fprintf(stderr, "rank %d: NI max_triggered_ops=%d, requested=%d\n", rank,
            ctx.limits.max_triggered_ops, opts.iterations + opts.warmup);
    bench_fail("Iterations plus warmup exceed NI max_triggered_ops");
  }
  if(rank == 0)
    fprintf(stderr,
            "op=put ni=non-matching events=counting triggered=%d "
            "bytes=%zu window=1 iterations=%d warmup=%d; RTT and "
            "trigger setup in us; initialized separate send/receive buffers\n",
            triggered, opts.msg_size, opts.iterations, opts.warmup);

  if(rank == 0 && stats_output)
    puts(BENCH_STATS_HEADER);

  if(triggered)
    run_triggered_ping_pong_benchmark();
  else
    run_ping_pong_benchmark();

  destroy_p4_ctx(&ctx);
  PtlFini();
  MPI_CHECK(MPI_Finalize());
  return eret;
}
