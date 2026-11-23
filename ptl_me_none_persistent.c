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
int (*communicate)(const ptl_handle_md_t md_h, const ptl_size_t local_offset,
                   const ptl_size_t remote_offset, const ptl_size_t msg_size,
                   const ptl_pt_index_t index, const ptl_ack_req_t req_type,
                   const ptl_match_bits_t);

int
put_operation(const ptl_handle_md_t md_h, const ptl_size_t local_offset,
              const ptl_size_t remote_offset, const ptl_size_t msg_size,
              const ptl_pt_index_t index, const ptl_ack_req_t req_type,
              const ptl_match_bits_t match_bits)
{
  int eret = -1;
  eret = PtlPut(md_h, local_offset, msg_size, req_type, ctx.peer_addr, index,
                match_bits, remote_offset, NULL, 0);
  return eret;
}

int
get_operation(const ptl_handle_md_t md_h, const ptl_size_t local_offset,
              const ptl_size_t remote_offset, const ptl_size_t msg_size,
              const ptl_pt_index_t index, const ptl_ack_req_t req_type,
              const ptl_match_bits_t match_bits)
{
  (void)req_type;
  int eret = -1;

  eret = PtlGet(md_h, local_offset, msg_size, ctx.peer_addr, index, match_bits,
                remote_offset, NULL);

  return eret;
}

static inline void
wait_for_completion(const ptl_size_t wait_for)
{
  ptl_event_t event;
  ptl_event_kind_t kind =
      rank == 0 ? (opts.op == PUT ? PTL_EVENT_ACK : PTL_EVENT_REPLY)
                : (opts.op == PUT ? PTL_EVENT_PUT : PTL_EVENT_GET);
  for(ptl_size_t i = 0; i < wait_for; ++i)
    wait_event(ctx.eq_h, kind, &event);
}

typedef struct
{
  ptl_handle_eq_t eq_h;
  ptl_handle_me_t me_h;
  ptl_handle_md_t md_h;
  ptl_pt_index_t index;
} cmd_ctx_t;

void
setup_cmd_channel(p4_ctx_t* const ctx, cmd_ctx_t* const cmd)
{
  int eret = -1;

  eret = PtlEQAlloc(ctx->ni_h, 4096, &cmd->eq_h);
  if(PTL_OK != eret)
    P4_CHECK(eret);

  eret = PtlPTAlloc(ctx->ni_h, 0, cmd->eq_h, 21, &cmd->index);
  if(PTL_OK != eret)
    P4_CHECK(eret);

  ptl_md_t md = {.start = NULL,
                 .length = 0,
                 .options = PTL_MD_VOLATILE | PTL_MD_EVENT_SUCCESS_DISABLE,
                 .ct_handle = PTL_CT_NONE,
                 .eq_handle = PTL_EQ_NONE};
  eret = PtlMDBind(ctx->ni_h, &md, &cmd->md_h);
  if(PTL_OK != eret)
    P4_CHECK(eret);

  ptl_event_t event;

  ptl_me_t me = {.start = NULL,
                 .length = 0,
                 .options = PTL_ME_OP_PUT | PTL_ME_EVENT_UNLINK_DISABLE,
                 .uid = PTL_UID_ANY,
                 .match_id = ctx->peer_addr,
                 .match_bits = 0,
                 .ignore_bits = 0,
                 .ct_handle = PTL_CT_NONE};

  eret = PtlMEAppend(ctx->ni_h, cmd->index, &me, PTL_PRIORITY_LIST, NULL,
                     &cmd->me_h);
  if(PTL_OK != eret)
    P4_CHECK(eret);

  wait_event(cmd->eq_h, PTL_EVENT_LINK, &event);
}

void
free_cmd_channel(cmd_ctx_t* cmd)
{
  p4_md_free(cmd->md_h);
  p4_me_remove(cmd->me_h);
  P4_CHECK(PtlPTFree(ctx.ni_h, cmd->index));
  P4_CHECK(PtlEQFree(cmd->eq_h));
}

void
wait_for_cmd(cmd_ctx_t* const cmd)
{
  ptl_event_t event;
  wait_event(cmd->eq_h, PTL_EVENT_PUT, &event);
}

void
send_cmd(cmd_ctx_t* const cmd)
{
  int eret = PtlPut(cmd->md_h, 0, 0, PTL_NO_ACK_REQ, ctx.peer_addr, cmd->index,
                    0, 0, NULL, 0);
  if(PTL_OK != eret)
  {
    P4_CHECK(eret);
  }
}

void
run_me_non_persistent_benchmark()
{
  ptl_handle_md_t md_h;
  ptl_pt_index_t index;
  int eret = -1;
  double t0 = 0.0;
  cmd_ctx_t cmd;

  ptl_handle_me_t* me_hs = NULL;
  ptl_size_t* sizes = NULL;
  void** buffers = NULL;
  void* send_buffer = NULL;

  eret = p4_pt_alloc(&ctx, &index);
  if(PTL_OK != eret)
  {
    P4_CHECK(eret);
  }

  setup_cmd_channel(&ctx, &cmd);

  communicate = opts.op == PUT ? &put_operation : &get_operation;
  int iterations = opts.iterations + opts.warmup;
  double* bandwidth = rank == 0 ? alloc_samples(opts.iterations) : NULL;
  double* latency = rank == 0 ? alloc_samples(opts.iterations) : NULL;

  me_hs = malloc(opts.window_size * sizeof(ptl_handle_me_t));
  sizes = malloc(opts.window_size * sizeof(ptl_size_t));
  buffers = malloc(opts.window_size * sizeof(void*));
  if(!me_hs || !sizes || !buffers)
    bench_fail("Window allocation failed");

  if(0 == rank && !stats_output)
  {
    fprintf(stdout,
            table_output
                ? "Operation   Window      Bytes    MB/s       us/operation\n"
                : "func,window_size,msg_size,bandwidth,latency\n");
    fflush(stdout);
  }

  for(size_t msg_size = opts.min_msg_size; msg_size <= opts.max_msg_size;
      msg_size *= 2)
  {
    if(0 == rank)
    {
      alloc_buffer_init(&send_buffer, msg_size * opts.window_size);
      eret =
          p4_md_alloc_eq(&ctx, &md_h, send_buffer, opts.window_size * msg_size);
      if(eret != PTL_OK)
      {
        fprintf(stderr, "md alloc failed %i\n", eret);
        P4_CHECK(eret);
      }
    }
    else
    {
      for(int i = 0; i < opts.window_size; ++i)
      {
        sizes[i] = msg_size;
        alloc_buffer_init(&buffers[i], sizes[i]);
        memset(buffers[i], 'd', sizes[i]);
      }
    }

    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

    if(0 == rank)
    {
      for(int i = 0; i < iterations; ++i)
      {
        if(i >= opts.warmup)
        {
          t0 = MPI_Wtime();
        }

        wait_for_cmd(&cmd);

        for(int w = 0; w < opts.window_size; ++w)
        {
          eret = communicate(md_h, w * msg_size, 0, msg_size, index,
                             PTL_ACK_REQ, w + 1);
          if(eret != PTL_OK)
          {
            fprintf(stderr, "comm failed %i\n", eret);
            P4_CHECK(eret);
          }
        }
        wait_for_completion(opts.window_size);

        if(i >= opts.warmup)
        {
          latency[i - opts.warmup] = MPI_Wtime() - t0;
        }
      }
      p4_md_free(md_h);
      if(opts.op == GET)
        validate_buffer(send_buffer, msg_size * opts.window_size, 'd');
      free(send_buffer);
      for(int i = 0; i < opts.iterations; ++i)
      {
        double elapsed = latency[i];
        bandwidth[i] = (msg_size * opts.window_size * 1e-6) / elapsed;
        latency[i] = (elapsed * 1e6) / opts.window_size;
      }
      if(stats_output)
      {
        report_stats(msg_size, "bandwidth", "MB/s (decimal)", bandwidth,
                     opts.iterations);
        report_stats(msg_size, "latency", "us/operation (window time / window)",
                     latency, opts.iterations);
      }
      else
      {
        for(int i = 0; i < opts.iterations; ++i)
          printf(table_output ? "%-9s %8i %10zu %12.4f %12.4f\n"
                              : "%s,%i,%zu,%.4f,%.4f\n",
                 opts.op == PUT ? "put" : "get", opts.window_size, msg_size,
                 bandwidth[i], latency[i]);
      }
      fflush(stdout);
    }
    else
    {
      for(int i = 0; i < iterations; ++i)
      {
        P4_CHECK(p4_me_insert_list_use_once(
            &ctx, me_hs, buffers, sizes, opts.window_size, index, PTL_CT_NONE));
        send_cmd(&cmd);
        wait_for_completion(opts.window_size);
      }
      for(int i = 0; i < opts.window_size; ++i)
      {
        if(opts.op == PUT)
          validate_buffer(buffers[i], sizes[i], 'c');
        free(buffers[i]);
      }
    }
  }

  MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

  free(bandwidth);
  free(latency);
  free(me_hs);
  free(buffers);
  free(sizes);
  free_cmd_channel(&cmd);
  p4_pt_free(&ctx, index);
}

void
print_help_message(void)
{
  printf(
      "Usage: ptl_me_none_persistent [options]\n"
      "  -i, --iterations N   Measured windows (default 1)\n"
      "  -u, --warmup N       Warm-up windows (default 0)\n"
      "  -w, --window-size N  Messages per window (default 64)\n"
      "  --msg_size N         Single size in bytes (default sweep 1:4194304)\n"
      "  -g, --get            GET instead of PUT\n"
      "  --table              Human-readable output (default CSV)\n"
      "  -h, --help           Show help\n");
  puts("  --stats                   Report mean, median, min and max as CSV");
}

int
main(int argc, char* argv[])
{
  int eret = -1;
  static const struct option long_opts[] = {
      {"msg_size", required_argument, NULL, 1},
      {"iterations", required_argument, NULL, 'i'},
      {"warmup", required_argument, NULL, 'u'},
      {"window-size", required_argument, NULL, 'w'},
      {"get", no_argument, NULL, 'g'},
      {"table", no_argument, NULL, 1000},
      {"stats", no_argument, NULL, BENCH_STATS_OPTION},
      {"help", no_argument, NULL, 'h'},
      {0, 0, 0, 0}};

  const char* const short_opts = "i:w:u:gh";

  opts.ni_mode = MATCHING;
  opts.op = PUT;
  opts.type = LATENCY;
  opts.iterations = 1;
  opts.warmup = 0;
  opts.window_size = 64;
  opts.msg_size = 1024;
  opts.min_msg_size = 1;
  opts.max_msg_size = 4194304;
  opts.event_type = FULL;
  opts.cache_size = _16MiB;
  opts.cache_state = HOT_CACHE;

  while(1)
  {
    const int opt = getopt_long(argc, argv, short_opts, long_opts, NULL);
    if(-1 == opt)
    {
      break;
    }
    switch(opt)
    {
    case 1:
      opts.min_msg_size = opts.max_msg_size = parse_size(optarg);
      break;
    case 'i':
      opts.iterations = parse_int(optarg);
      break;
    case 'u':
      opts.warmup = parse_int(optarg);
      break;
    case 'w':
      opts.window_size = parse_int(optarg);
      break;
    case 'g':
      opts.op = GET;
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
  validate_range(opts.min_msg_size, opts.max_msg_size, opts.window_size);
  if(opts.window_size > 4096)
    option_error("Full-event window exceeds the 4096-entry EQ capacity");
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
  eret = init_p4_ctx(&ctx, MATCHING);
  if(PTL_OK != eret)
  {
    fprintf(stderr, "init failed with %i\n", eret);
    fflush(stderr);
    bench_fail("Benchmark initialization failed");
  }

  eret = exchange_ni_address(&ctx, rank);

  if(0 != eret)
  {
    fprintf(stderr, "exchange failed with %i\n", eret);
    fflush(stderr);
    bench_fail("Benchmark initialization failed");
  }

  check_message_size(&ctx, opts.max_msg_size);
  if(opts.window_size > ctx.limits.max_list_size ||
     opts.window_size >= ctx.limits.max_entries)
    bench_fail("Window plus command entry exceeds NI list/entry limits");
  if(rank == 0)
    fprintf(stderr,
            "op=%s ni=matching events=full window=%d iterations=%d "
            "warmup=%d bytes=%zu:%zu; MB/s and us/operation; "
            "includes command readiness and entry setup wait\n",
            opts.op == PUT ? "put" : "get", opts.window_size, opts.iterations,
            opts.warmup, opts.min_msg_size, opts.max_msg_size);

  if(rank == 0 && stats_output)
    puts(BENCH_STATS_HEADER);

  run_me_non_persistent_benchmark();
  destroy_p4_ctx(&ctx);
  PtlFini();
  MPI_CHECK(MPI_Finalize());
  return eret;
}
