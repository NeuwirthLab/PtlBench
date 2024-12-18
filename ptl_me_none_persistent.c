#include "common.h"
#include "util.h"
#include <assert.h>
#include <getopt.h>
#include <math.h>
#include <sys/mman.h>
#include <time.h>

static int rank;
static int num_ranks;
static p4_ctx_t ctx;
static benchmark_opts_t opts;
static char processor_name[MPI_MAX_PROCESSOR_NAME];
int* cache_buffer;
size_t cache_buffer_size;
int (*communicate)(const ptl_handle_md_t md_h, const ptl_size_t local_offset,
                   const ptl_size_t remote_offset, const ptl_size_t msg_size,
                   const ptl_index_t index, const ptl_ack_req_t req_type,
                   const ptl_match_bits_t);

int
put_operation(const ptl_handle_md_t md_h, const ptl_size_t local_offset,
              const ptl_size_t remote_offset, const ptl_size_t msg_size,
              const ptl_index_t index, const ptl_ack_req_t req_type,
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
              const ptl_index_t index, const ptl_ack_req_t req_type,
              const ptl_match_bits_t match_bits)
{
  int eret = -1;

  eret = PtlGet(md_h, local_offset, msg_size, ctx.peer_addr, index, match_bits,
                remote_offset, NULL);

  return eret;
}

static inline void
wait_for_completion(const ptl_size_t wait_for)
{
  ptl_event_t event;
  int eret = -1;
  for(int i = 0; i < wait_for; ++i)
  {
    eret = PtlEQWait(ctx.eq_h, &event);
    if(PTL_OK != eret || event.ni_fail_type != PTL_NI_OK)
    {
      fprintf(stderr, "PtlCTWait failed\n");
      MPI_Abort(MPI_COMM_WORLD, eret);
    }
  }
}

typedef struct
{
  ptl_handle_eq_t eq_h;
  ptl_handle_le_t le_h;
  ptl_handle_md_t md_h;
  ptl_index_t index;
} cmd_ctx_t;

void
setup_cmd_channel(p4_ctx_t* const ctx, cmd_ctx_t* const cmd)
{
  int eret = -1;

  eret = PtlEQAlloc(ctx->ni_h, 4096, &cmd->eq_h);
  if(PTL_OK != eret)
    MPI_Abort(MPI_COMM_WORLD, -97);

  eret = PtlPTAlloc(ctx->ni_h, 0, cmd->eq_h, 21, &cmd->index);
  if(PTL_OK != eret)
    MPI_Abort(MPI_COMM_WORLD, -98);

  ptl_md_t md = {.start = NULL,
                 .length = PTL_SIZE_MAX,
                 .options = PTL_MD_VOLATILE | PTL_MD_EVENT_SUCCESS_DISABLE,
                 .ct_handle = PTL_CT_NONE,
                 .eq_handle = PTL_EQ_NONE};
  eret = PtlMDBind(ctx->ni_h, &md, &cmd->md_h);
  if(PTL_OK != eret)
    MPI_Abort(MPI_COMM_WORLD, -95);

  ptl_event_t event;

  ptl_le_t le = {.start = NULL,
                 .length = PTL_SIZE_MAX,
                 .options = PTL_LE_OP_PUT | PTL_LE_EVENT_UNLINK_DISABLE,
                 .uid = PTL_UID_ANY,
                 .ct_handle = PTL_CT_NONE};

  eret = PtlLEAppend(ctx->ni_h, cmd->index, &le, PTL_PRIORITY_LIST, NULL,
                     &cmd->le_h);
  if(PTL_OK != eret)
    MPI_Abort(MPI_COMM_WORLD, -99);

  PtlEQWait(cmd->eq_h, &event);

  if(PTL_EVENT_LINK != event.type)
  {
    fprintf(stderr, "Failed to link LE\n");
    MPI_Abort(MPI_COMM_WORLD, -100);
  }
  if(PTL_NI_OK != event.ni_fail_type)
  {
    fprintf(stderr, "ni_fail_type != PTL_NI_OK");
    MPI_Abort(MPI_COMM_WORLD, -101);
  }
}

void
free_cmd_channel(cmd_ctx_t* cmd)
{
  PtlEQFree(cmd->eq_h);
  PtlMDRelease(cmd->md_h);
  PtlLEUnlink(cmd->le_h);
  PtlPTFree(ctx.ni_h, cmd->index);
}

void
wait_for_cmd(cmd_ctx_t* const cmd)
{
  ptl_event_t event;
  int eret = PtlEQWait(cmd->eq_h, &event);
  if(PTL_OK != eret || event.ni_fail_type != PTL_NI_OK)
  {
    fprintf(stderr, "PtlCTWait failed\n");
    MPI_Abort(MPI_COMM_WORLD, -122);
  }
}

void
send_cmd(cmd_ctx_t* const cmd)
{
  int eret = PtlPut(cmd->md_h, 0, 0, PTL_NO_ACK_REQ, ctx.peer_addr, cmd->index,
                    0, 0, NULL, 0);
  if(PTL_OK != eret)
  {
    MPI_Abort(MPI_COMM_WORLD, -123);
  }
}

void
run_me_non_persistent_benchmark()
{
  ptl_handle_md_t md_h;
  ptl_index_t index;
  ptl_index_t cmd_index;
  ptl_ct_event_t zero = {.success = 0, .failure = 0};
  int eret = -1;
  int distance = -1;
  double t0, t;
  cmd_ctx_t cmd;

  ptl_handle_me_t* me_hs = NULL;
  ptl_size_t* sizes = NULL;
  void** buffers = NULL;
  void* send_buffer = NULL;

  eret = p4_pt_alloc(&ctx, &index);
  if(PTL_OK != eret)
  {
    MPI_Abort(MPI_COMM_WORLD, -1);
  }

  setup_cmd_channel(&ctx, &cmd);

  communicate = opts.op == PUT ? &put_operation : &get_operation;
  int iterations = opts.iterations + opts.warmup;

  me_hs = malloc(opts.window_size * sizeof(ptl_handle_me_t));
  sizes = malloc(opts.window_size * sizeof(ptl_size_t));
  buffers = malloc(opts.window_size * sizeof(void*));

  if(0 == rank)
  {
    fprintf(stdout, "func,window_size,msg_size,bandwidth,latency\n");
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
      if(eret < 0)
      {
        fprintf(stderr, "md alloc failed %i\n", eret);
        MPI_Abort(MPI_COMM_WORLD, eret);
      }
    }
    else
    {
      for(int i = 0; i < opts.window_size; ++i)
      {
        sizes[i] = msg_size;
        alloc_buffer_init(&buffers[i], sizes[i]);
      }
    }

    MPI_Barrier(MPI_COMM_WORLD);

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
          if(eret < 0)
          {
            fprintf(stderr, "comm failed %i\n", eret);
            MPI_Abort(MPI_COMM_WORLD, eret);
          }
        }
        wait_for_completion(opts.window_size);

        if(i >= opts.warmup)
        {
          t = MPI_Wtime() - t0;
          fprintf(stdout, "%s,%i,%lu,%.4f,%.4f\n",
                  opts.op == PUT ? "put" : "get", opts.window_size, msg_size,
                  (msg_size * opts.window_size * 1e-6) / t,
                  (t * 1e6) / opts.window_size);
          fflush(stdout);
        }
      }
      p4_md_free(md_h);
    }
    else
    {
      for(int i = 0; i < iterations; ++i)
      {
        p4_me_insert_list_use_once(&ctx, me_hs, buffers, sizes,
                                   opts.window_size, index, PTL_CT_NONE);
        send_cmd(&cmd);
        wait_for_completion(opts.window_size);
      }
      for(int i = 0; i < opts.window_size; ++i){
	      free(buffers[i]);
      }
    }
  }

  MPI_Barrier(MPI_COMM_WORLD);

  if(0 != rank)
  {
    for(int i = 0; i < opts.window_size; ++i)
    {
      free(buffers[i]);
    }
  }

  free(me_hs);
  free(buffers);
  free(sizes);
  free_cmd_channel(&cmd);
  PtlPTFree(ctx.ni_h, index);
}

int
main(int argc, char* argv[])
{
  int eret = -1;
  ptl_ct_event_t zct = {.failure = 0, .success = 0};
  static const struct option long_opts[] = {
      {"iterations", required_argument, NULL, 'i'},
      {"warmup", required_argument, NULL, 'u'},
      {"window-size", required_argument, NULL, 'w'},
      {"get", required_argument, NULL, 'g'},
      {"help", no_argument, NULL, 'h'}};

  const char* const short_opts = "i:w:u:gh";

  opts.ni_mode = NON_MATCHING;
  opts.op = PUT;
  opts.type = LATENCY;
  opts.iterations = 1;
  opts.warmup = 0;
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
    {
      break;
    }
    switch(opt)
    {
    case 'i':
      opts.iterations = atoi(optarg);
      break;
    case 'c':
      opts.cache_size = atoi(optarg);
      opts.cache_size *= MiB;
      break;
    case 'u':
      opts.warmup = atoi(optarg);
      break;
    case 'w':
      opts.window_size = atoi(optarg);
      break;
    case 'g':
      opts.op = GET;
      break;
    case 'h':
      // print_help_message();
      exit(EXIT_SUCCESS);
    case '?':
      // print_help_message();
      exit(EXIT_FAILURE);
    default:
      // print_help_message();
      exit(EXIT_FAILURE);
    }
  }

  int name_len;
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);
  MPI_Get_processor_name(processor_name, &name_len);

  if(2 != num_ranks)
  {
    fprintf(stderr, "Benchmark requires exactly two processes\n");
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
  }

  PtlInit();
  eret = init_p4_ctx(&ctx, PTL_NI_MATCHING);
  if(PTL_OK != eret)
  {
    fprintf(stderr, "init failed with %i\n", eret);
    fflush(stderr);
    goto END;
  }

  eret = exchange_ni_address(&ctx, rank);

  if(0 > eret)
  {
    fprintf(stderr, "exchange failed with %i\n", eret);
    fflush(stderr);
    goto END;
  }

  // cache_buffer = malloc(opts.cache_size);
  // if(NULL == cache_buffer)
  //  goto END;
  // cache_buffer_size = opts.cache_size / sizeof(int);

  set_cache_regions(1);
  run_me_non_persistent_benchmark();
END:
  // free(cache_buffer);
  destroy_p4_ctx(&ctx);
  PtlFini();
  MPI_Finalize();
  return eret;
}
