#include "util.h"
#include "common.h"
#include <limits.h>

#define REQUESTED_INDEX 99

_Noreturn void
bench_fail(const char* message)
{
  int initialized = 0, rank = -1;
  MPI_Initialized(&initialized);
  if(initialized)
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  fprintf(stderr, "rank %d: %s\n", rank, message);
  if(initialized)
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
  exit(EXIT_FAILURE);
}

void
p4_check(int rc, const char* call)
{
  if(rc != PTL_OK)
  {
    char message[512];
    snprintf(message, sizeof(message), "%s failed (Portals status %d)", call,
             rc);
    bench_fail(message);
  }
}

void
mpi_check(int rc, const char* call)
{
  if(rc != MPI_SUCCESS)
    bench_fail(call);
}

void
wait_event(ptl_handle_eq_t eq, ptl_event_kind_t kind, ptl_event_t* event)
{
  P4_CHECK(PtlEQWait(eq, event));
  if(event->ni_fail_type != PTL_NI_OK || event->type != kind)
  {
    char message[160];
    snprintf(message, sizeof(message),
             "Portals event type=%u expected=%u failure=%u",
             (unsigned)event->type, (unsigned)kind,
             (unsigned)event->ni_fail_type);
    bench_fail(message);
  }
}

void
wait_count(ptl_handle_ct_t ct, ptl_size_t threshold)
{
  ptl_ct_event_t event;
  P4_CHECK(PtlCTWait(ct, threshold, &event));
  if(event.failure || event.success < threshold)
    bench_fail("Counting event reports failed or incomplete operations");
}

double*
alloc_samples(size_t count)
{
  double* samples = malloc(count * sizeof(*samples));
  if(samples == NULL)
    bench_fail("Sample allocation failed");
  return samples;
}

void
report_stats(size_t size, const char* metric, const char* unit, double* samples,
             size_t count)
{
  if(bench_print_stats(size, metric, unit, samples, count) != 0)
    bench_fail("Could not write statistics");
}

void
validate_buffer(const void* buffer, size_t bytes, unsigned char expected)
{
  const unsigned char* data = buffer;
  for(size_t i = 0; i < bytes; ++i)
    if(data[i] != expected)
      bench_fail("Payload validation failed");
}

void
check_message_size(const p4_ctx_t* ctx, size_t size)
{
  if(size > ctx->limits.max_msg_size)
    bench_fail("Message size exceeds the returned NI max_msg_size");
}

int
init_p4_ctx(p4_ctx_t* const ctx, const ni_mode_t mode)
{
  int eret = -1;
  unsigned int ni_matching =
      mode == MATCHING ? PTL_NI_MATCHING : PTL_NI_NO_MATCHING;

  ptl_ni_limits_t ni_requested_limits = {
      .max_entries = INT_MAX,
      .max_unexpected_headers = INT_MAX,
      .max_mds = INT_MAX,
      .max_eqs = INT_MAX,
      .max_cts = INT_MAX,
      .max_pt_index = INT_MAX,
      .max_iovecs = INT_MAX,
      .max_list_size = INT_MAX,
      .max_triggered_ops = INT_MAX,
      .max_msg_size = LONG_MAX,
      .max_atomic_size = LONG_MAX,
      .max_fetch_atomic_size = LONG_MAX,
      .max_waw_ordered_size = LONG_MAX,
      .max_war_ordered_size = LONG_MAX,
      .max_volatile_size = LONG_MAX,
      .features = PTL_TARGET_BIND_INACCESSIBLE,
  };

  ctx->eq_h = PTL_INVALID_HANDLE;
  ctx->ct_h = PTL_INVALID_HANDLE;
  ctx->ni_h = PTL_INVALID_HANDLE;

  eret = PtlNIInit(PTL_IFACE_DEFAULT, ni_matching | PTL_NI_PHYSICAL,
                   PTL_PID_ANY, &ni_requested_limits, &ctx->limits, &ctx->ni_h);
  if(PTL_OK != eret)
    goto fail;

  eret = PtlGetPhysId(ctx->ni_h, &ctx->my_addr);
  if(PTL_OK != eret)
    goto fail;
  eret = PtlEQAlloc(ctx->ni_h, 4096, &ctx->eq_h);
  if(PTL_OK != eret)
    goto fail;
  eret = PtlCTAlloc(ctx->ni_h, &ctx->ct_h);
  if(PTL_OK != eret)
    goto fail;
  return PTL_OK;
fail:
  destroy_p4_ctx(ctx);
  p4_check(eret, "Portals context initialization");
  return eret;
}

void
destroy_p4_ctx(p4_ctx_t* const ctx)
{
  if(!PtlHandleIsEqual(ctx->eq_h, PTL_INVALID_HANDLE))
    P4_CHECK(PtlEQFree(ctx->eq_h));
  if(!PtlHandleIsEqual(ctx->ct_h, PTL_INVALID_HANDLE))
    P4_CHECK(PtlCTFree(ctx->ct_h));
  if(!PtlHandleIsEqual(ctx->ni_h, PTL_INVALID_HANDLE))
    P4_CHECK(PtlNIFini(ctx->ni_h));
}

int
exchange_ni_address(p4_ctx_t* const ctx, const int my_rank)
{
  // Exchange numeric fields independently of the vendor's underlying types.
  uint64_t local[2] = {ctx->my_addr.phys.nid, ctx->my_addr.phys.pid};
  uint64_t remote[2];
  int peer = (my_rank + 1) % 2;

  MPI_CHECK(MPI_Sendrecv(local, 2, MPI_UINT64_T, peer, 1, remote, 2,
                         MPI_UINT64_T, peer, 1, MPI_COMM_WORLD,
                         MPI_STATUS_IGNORE));
  ctx->peer_addr.phys.nid = remote[0];
  ctx->peer_addr.phys.pid = remote[1];
  return 0;
}

int
p4_pt_alloc(p4_ctx_t* const ctx, ptl_pt_index_t* const index)
{
  if(ctx->limits.max_pt_index < REQUESTED_INDEX)
    bench_fail("NI does not support portal index 99");
  return PtlPTAlloc(ctx->ni_h, 0, ctx->eq_h, REQUESTED_INDEX, index);
}

void
p4_pt_free(p4_ctx_t* const ctx, ptl_pt_index_t index)
{
  P4_CHECK(PtlPTFree(ctx->ni_h, index));
}

int
p4_md_alloc_ct(p4_ctx_t* const ctx, ptl_handle_md_t* const md_h,
               void* const start, const ptl_size_t length)
{
  ptl_md_t md = {.start = start,
                 .length = length,
                 .options = PTL_MD_VOLATILE | PTL_MD_EVENT_CT_ACK |
                            PTL_MD_EVENT_CT_REPLY |
                            PTL_MD_EVENT_SUCCESS_DISABLE,
                 .ct_handle = ctx->ct_h,
                 .eq_handle = PTL_EQ_NONE};
  return PtlMDBind(ctx->ni_h, &md, md_h);
}

int
p4_md_alloc_eq(p4_ctx_t* const ctx, ptl_handle_md_t* const md_h,
               void* const start, const ptl_size_t length)
{
  ptl_md_t md = {.start = start,
                 .length = length,
                 .options = PTL_MD_VOLATILE | PTL_MD_EVENT_SEND_DISABLE,
                 .ct_handle = PTL_CT_NONE,
                 .eq_handle = ctx->eq_h};
  return PtlMDBind(ctx->ni_h, &md, md_h);
}

int
p4_md_alloc_eq_empty(p4_ctx_t* const ctx, ptl_handle_md_t* const md_h)
{
  ptl_md_t md = {.start = NULL,
                 .length = PTL_SIZE_MAX,
                 .options = PTL_MD_EVENT_SEND_DISABLE,
                 .ct_handle = PTL_CT_NONE,
                 .eq_handle = ctx->eq_h};
  return PtlMDBind(ctx->ni_h, &md, md_h);
}

void
p4_md_free(ptl_handle_md_t md_h)
{
  P4_CHECK(PtlMDRelease(md_h));
}

int
p4_le_insert(p4_ctx_t* const ctx, ptl_handle_le_t* const le_h,
             void* const start, const ptl_size_t length,
             const ptl_pt_index_t index)
{
  int eret = -1;
  ptl_event_t event;

  ptl_le_t le = {.start = start,
                 .length = length,
                 .options = PTL_LE_OP_GET | PTL_LE_OP_PUT |
                            PTL_LE_EVENT_COMM_DISABLE |
                            PTL_LE_EVENT_UNLINK_DISABLE,
                 .uid = PTL_UID_ANY,
                 .ct_handle = PTL_CT_NONE};

  eret = PtlLEAppend(ctx->ni_h, index, &le, PTL_PRIORITY_LIST, NULL, le_h);
  if(PTL_OK != eret)
    P4_CHECK(eret);

  wait_event(ctx->eq_h, PTL_EVENT_LINK, &event);

  return eret;
}

int
p4_le_insert_ct_comm(p4_ctx_t* const ctx, ptl_handle_le_t* const le_h,
                     void* const start, const ptl_size_t length,
                     const ptl_pt_index_t index)
{
  int eret = -1;
  ptl_event_t event;

  ptl_le_t le = {.start = start,
                 .length = length,
                 .options = PTL_LE_OP_GET | PTL_LE_OP_PUT |
                            PTL_LE_EVENT_UNLINK_DISABLE |
                            PTL_LE_EVENT_COMM_DISABLE | PTL_LE_EVENT_CT_COMM,
                 .uid = PTL_UID_ANY,
                 .ct_handle = ctx->ct_h};

  eret = PtlLEAppend(ctx->ni_h, index, &le, PTL_PRIORITY_LIST, NULL, le_h);
  if(PTL_OK != eret)
    P4_CHECK(eret);

  wait_event(ctx->eq_h, PTL_EVENT_LINK, &event);

  return eret;
}

int
p4_le_insert_full_comm(p4_ctx_t* const ctx, ptl_handle_le_t* const le_h,
                       void* const start, const ptl_size_t length,
                       const ptl_pt_index_t index)
{
  int eret = -1;
  ptl_event_t event;

  ptl_le_t le = {.start = start,
                 .length = length,
                 .options = PTL_LE_OP_GET | PTL_LE_OP_PUT |
                            PTL_LE_EVENT_UNLINK_DISABLE,
                 .uid = PTL_UID_ANY,
                 .ct_handle = PTL_CT_NONE};

  eret = PtlLEAppend(ctx->ni_h, index, &le, PTL_PRIORITY_LIST, NULL, le_h);
  if(PTL_OK != eret)
    P4_CHECK(eret);

  wait_event(ctx->eq_h, PTL_EVENT_LINK, &event);

  return eret;
}

int
p4_le_insert_empty(p4_ctx_t* const ctx, ptl_handle_le_t* const le_h,
                   const ptl_pt_index_t index)
{
  int eret = -1;
  ptl_event_t event;

  ptl_le_t le = {.start = NULL,
                 .length = PTL_SIZE_MAX,
                 .options = PTL_LE_OP_GET | PTL_LE_OP_PUT |
                            PTL_LE_EVENT_COMM_DISABLE |
                            PTL_LE_EVENT_UNLINK_DISABLE,
                 .uid = PTL_UID_ANY,
                 .ct_handle = PTL_CT_NONE};

  eret = PtlLEAppend(ctx->ni_h, index, &le, PTL_PRIORITY_LIST, NULL, le_h);
  if(PTL_OK != eret)
    P4_CHECK(eret);

  wait_event(ctx->eq_h, PTL_EVENT_LINK, &event);

  return eret;
}

void
p4_le_remove(ptl_handle_le_t le_h)
{
  P4_CHECK(PtlLEUnlink(le_h));
}

int
p4_me_insert_persistent(p4_ctx_t* const ctx, ptl_handle_me_t* const me_h,
                        void* const start, const ptl_size_t length,
                        const ptl_pt_index_t index)
{
  int eret = -1;
  ptl_event_t event;
  ptl_process_t src;

  src.phys.nid = PTL_NID_ANY;
  src.phys.pid = PTL_PID_ANY;

  ptl_me_t me = {.start = start,
                 .length = length,
                 .options = PTL_ME_OP_GET | PTL_ME_OP_PUT |
                            PTL_ME_EVENT_COMM_DISABLE |
                            PTL_ME_EVENT_UNLINK_DISABLE | PTL_ME_IS_ACCESSIBLE,
                 .ct_handle = PTL_CT_NONE,
                 .uid = PTL_UID_ANY,
                 .match_id = src,
                 .match_bits = 0xDEADBEEF,
                 .ignore_bits = 0,
                 .min_free = 0};

  eret = PtlMEAppend(ctx->ni_h, index, &me, PTL_PRIORITY_LIST, NULL, me_h);
  if(PTL_OK != eret)
    P4_CHECK(eret);

  wait_event(ctx->eq_h, PTL_EVENT_LINK, &event);

  return eret;
}

static int
p4_me_insert_once(p4_ctx_t* const ctx, ptl_handle_me_t* const me_h, void* start,
                  const ptl_size_t length, const ptl_pt_index_t index,
                  const ptl_handle_ct_t ct_handle,
                  const ptl_match_bits_t match_bits)
{
  int eret = -1;
  ptl_event_t event;
  ptl_process_t src;

  src.phys.nid = PTL_NID_ANY;
  src.phys.pid = PTL_PID_ANY;

  ptl_me_t me = {.start = start,
                 .length = length,
                 .options = PTL_ME_OP_GET | PTL_ME_OP_PUT |
                            PTL_ME_EVENT_UNLINK_DISABLE | PTL_ME_IS_ACCESSIBLE |
                            PTL_ME_USE_ONCE,
                 .ct_handle = ct_handle,
                 .uid = PTL_UID_ANY,
                 .match_id = src,
                 .match_bits = match_bits,
                 .ignore_bits = 0,
                 .min_free = 0};

  eret = PtlMEAppend(ctx->ni_h, index, &me, PTL_PRIORITY_LIST, NULL, me_h);
  if(PTL_OK != eret)
    P4_CHECK(eret);

  wait_event(ctx->eq_h, PTL_EVENT_LINK, &event);

  return eret;
}

int
p4_me_insert_list_use_once(p4_ctx_t* const ctx, ptl_handle_me_t* const me_h,
                           void* const* start, ptl_size_t* const length,
                           const int num_entries, const ptl_pt_index_t index,
                           const ptl_handle_ct_t ct_handle)
{
  int eret = -1;
  ptl_match_bits_t match_bits = 1;
  for(int i = 0; i < num_entries; ++i)
  {
    eret = p4_me_insert_once(ctx, &me_h[i], start[i], length[i], index,
                             ct_handle, match_bits++);
    if(eret != PTL_OK)
      return -1;
  }
  return 0;
}

void
p4_me_remove(ptl_handle_me_t me_h)
{
  P4_CHECK(PtlMEUnlink(me_h));
}

int
alloc_buffer_init(void** ptr, size_t bytes)
{
  long page_size = sysconf(_SC_PAGESIZE);
  *ptr = NULL;
  if(page_size <= 0 || posix_memalign(ptr, (size_t)page_size, bytes) != 0)
    bench_fail("Aligned buffer allocation failed");
  memset(*ptr, 'c', bytes);
  return 0;
}

void
invalidate_cache(int* const cache_buffer, const size_t elements)
{
  volatile int* buffer = cache_buffer;
  if(elements == 0)
    return;
  buffer[0] = 1;
  for(size_t i = 1; i < elements; ++i)
  {
    buffer[i] = buffer[i - 1];
  }
}

static const char cache_pids_path[] = "/sys/class/bxi/bxi0/v2p/cache_pids";

static int
read_cache_pids(void)
{
  FILE* file = fopen(cache_pids_path, "r");
  if(file == NULL)
    return -1;
  int value;
  int fields = fscanf(file, "%d", &value);
  int status = fclose(file);
  return fields == 1 && status == 0 ? value : -1;
}

void
configure_cache_pids(int requested)
{
  if(requested < 0)
  {
    const char* setting = getenv("PTLBENCH_CACHE_PIDS");
    if(setting == NULL)
      return;
    requested = parse_int(setting);
  }

  // An already configured NIC can be used without sysfs write permissions.
  if(read_cache_pids() != requested)
  {
    FILE* file = fopen(cache_pids_path, "w");
    if(file == NULL)
    {
      perror(cache_pids_path);
      char message[128];
      snprintf(message, sizeof(message), "Cannot set BXI cache_pids=%d",
               requested);
      bench_fail(message);
    }
    int written = fprintf(file, "%d", requested);
    int status = fclose(file);
    if(written < 0 || status != 0)
    {
      perror(cache_pids_path);
      bench_fail("Cannot write BXI cache_pids");
    }
  }
  // Complete host-side setup on both ranks before either initializes Portals.
  MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
  if(read_cache_pids() != requested)
    bench_fail("BXI cache_pids readback differs from the requested value");
  int rank;
  MPI_CHECK(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  fprintf(stderr, "rank %d: BXI cache_pids=%d\n", rank, requested);
}
