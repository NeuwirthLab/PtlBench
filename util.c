#include "util.h"
#include <limits.h>
#include "common.h"

int init_p4_ctx(p4_ctx_t* const ctx, const ni_mode_t mode) {
	int eret = -1;
	unsigned int ni_matching =
	    mode == MATCHING ? PTL_NI_MATCHING : PTL_NI_NO_MATCHING;
	ptl_ni_limits_t ni_limits;

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

	eret = PtlNIInit(PTL_IFACE_DEFAULT,
	                 ni_matching | PTL_NI_PHYSICAL,
	                 PTL_PID_ANY,
	                 &ni_requested_limits,
	                 &ni_limits,
	                 &ctx->ni_h);
	if (PTL_OK != eret)
		return eret;

	eret = PtlGetPhysId(ctx->ni_h, &ctx->my_addr);
	if (PTL_OK != eret)
		return eret;
	eret = PtlEQAlloc(ctx->ni_h, 4096, &ctx->eq_h);
	if (PTL_OK != eret)
		return eret;
	eret = PtlCTAlloc(ctx->ni_h, &ctx->ct_h);
	if (PTL_OK != eret)
		return eret;
	return PTL_OK;
}

void destroy_p4_ctx(p4_ctx_t* const ctx) {
	if (!PtlHandleIsEqual(ctx->eq_h, PTL_INVALID_HANDLE))
		PtlEQFree(ctx->eq_h);
	if (!PtlHandleIsEqual(ctx->ct_h, PTL_INVALID_HANDLE))
		PtlCTFree(ctx->ct_h);
	if (!PtlHandleIsEqual(ctx->ni_h, PTL_INVALID_HANDLE))
		PtlNIFini(ctx->ni_h);
}

int exchange_ni_address(p4_ctx_t* const ctx, const int my_rank) {
	MPI_Request req[4];
	int peer = (my_rank + 1) % 2;

	MPI_Irecv(&ctx->peer_addr.phys.nid,
	          1,
	          MPI_UNSIGNED,
	          peer,
	          1,
	          MPI_COMM_WORLD,
	          req);
	MPI_Irecv(&ctx->peer_addr.phys.pid,
	          1,
	          MPI_UNSIGNED,
	          peer,
	          2,
	          MPI_COMM_WORLD,
	          req + 1);
	MPI_Isend(&ctx->my_addr.phys.nid,
	          1,
	          MPI_UNSIGNED,
	          peer,
	          1,
	          MPI_COMM_WORLD,
	          req + 2);
	MPI_Isend(&ctx->my_addr.phys.pid,
	          1,
	          MPI_UNSIGNED,
	          peer,
	          2,
	          MPI_COMM_WORLD,
	          req + 3);
	MPI_Waitall(4, req, MPI_STATUS_IGNORE);
	return 0;
}

int p4_pt_alloc(p4_ctx_t* const ctx, ptl_index_t* const index) {
	return PtlPTAlloc(ctx->ni_h, 0, ctx->eq_h, 0, index);
}

void p4_pt_free(p4_ctx_t* const ctx, ptl_index_t index) {
	PtlPTFree(ctx->ni_h, index);
}

int p4_md_alloc_ct(p4_ctx_t* const ctx,
                   ptl_handle_md_t* const md_h,
                   void* const start,
                   const ptl_size_t length) {
	ptl_md_t md = {.start = start,
	               .length = length,
	               .options = PTL_MD_VOLATILE | PTL_MD_EVENT_CT_ACK |
	                          PTL_MD_EVENT_CT_REPLY |
	                          PTL_MD_EVENT_SUCCESS_DISABLE,
	               .ct_handle = ctx->ct_h,
	               .eq_handle = PTL_EQ_NONE};
	return PtlMDBind(ctx->ni_h, &md, md_h);
}

int p4_md_alloc_eq(p4_ctx_t* const ctx,
                   ptl_handle_md_t* const md_h,
                   void* const start,
                   const ptl_size_t length) {
	ptl_md_t md = {.start = start,
	               .length = length,
	               .options = PTL_MD_VOLATILE | PTL_MD_EVENT_SEND_DISABLE,
	               .ct_handle = PTL_CT_NONE,
	               .eq_handle = ctx->eq_h};
	return PtlMDBind(ctx->ni_h, &md, md_h);
}

int p4_md_alloc_eq_empty(p4_ctx_t* const ctx, ptl_handle_md_t* const md_h) {
	ptl_md_t md = {.start = NULL,
	               .length = PTL_SIZE_MAX,
	               .options = PTL_MD_EVENT_SEND_DISABLE,
	               .ct_handle = PTL_CT_NONE,
	               .eq_handle = ctx->eq_h};
	return PtlMDBind(ctx->ni_h, &md, md_h);
}

void p4_md_free(ptl_handle_md_t md_h) {
	PtlMDRelease(md_h);
}

int p4_le_insert(p4_ctx_t* const ctx,
                 ptl_handle_le_t* const le_h,
                 void* const start,
                 const ptl_size_t length,
                 const ptl_index_t index) {
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
	if (PTL_OK != eret)
		return eret;

	PtlEQWait(ctx->eq_h, &event);

	if (PTL_EVENT_LINK != event.type) {
		fprintf(stderr, "Failed to link LE\n");
		return -1;
	}
	if (PTL_NI_OK != event.ni_fail_type) {
		fprintf(stderr, "ni_fail_type != PTL_NI_OK");
		return -1;
	}
	return eret;
}

int p4_le_insert_full_comm(p4_ctx_t* const ctx,
                 ptl_handle_le_t* const le_h,
                 void* const start,
                 const ptl_size_t length,
                 const ptl_index_t index) {
	int eret = -1;
	ptl_event_t event;

	ptl_le_t le = {.start = start,
	               .length = length,
	               .options = PTL_LE_OP_GET | PTL_LE_OP_PUT |
	                          PTL_LE_EVENT_UNLINK_DISABLE,
	               .uid = PTL_UID_ANY,
	               .ct_handle = PTL_CT_NONE};

	eret = PtlLEAppend(ctx->ni_h, index, &le, PTL_PRIORITY_LIST, NULL, le_h);
	if (PTL_OK != eret)
		return eret;

	PtlEQWait(ctx->eq_h, &event);

	if (PTL_EVENT_LINK != event.type) {
		fprintf(stderr, "Failed to link LE\n");
		return -1;
	}
	if (PTL_NI_OK != event.ni_fail_type) {
		fprintf(stderr, "ni_fail_type != PTL_NI_OK");
		return -1;
	}
	return eret;
}

int p4_le_insert_empty(p4_ctx_t* const ctx,
                       ptl_handle_le_t* const le_h,
                       const ptl_index_t index) {
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
	if (PTL_OK != eret)
		return eret;

	PtlEQWait(ctx->eq_h, &event);

	if (PTL_EVENT_LINK != event.type) {
		fprintf(stderr, "Failed to link LE\n");
		return -1;
	}
	if (PTL_NI_OK != event.ni_fail_type) {
		fprintf(stderr, "ni_fail_type != PTL_NI_OK");
		return -1;
	}
	return eret;
}

void p4_le_remove(ptl_handle_le_t le_h) {
	PtlLEUnlink(le_h);
}

int p4_me_insert_persistent(p4_ctx_t* const ctx,
                            ptl_handle_me_t* const me_h,
                            void* const start,
                            const ptl_size_t length,
                            const ptl_index_t index) {
	int eret = -1;
	ptl_event_t event;
	ptl_process_t src;

	src.phys.nid = PTL_NID_ANY;
	src.phys.pid = PTL_PID_ANY;

	ptl_me_t me = {
	    .start = start,
	    .length = length,
	    .options = PTL_ME_OP_GET | PTL_ME_OP_PUT | PTL_ME_EVENT_COMM_DISABLE |
	               PTL_ME_EVENT_UNLINK_DISABLE | PTL_ME_IS_ACCESSIBLE,
	    .ct_handle = PTL_CT_NONE,
	    .uid = PTL_UID_ANY,
	    .match_id = src,
	    .match_bits = 0xDEADBEEF,
	    .ignore_bits = 0,
	    .min_free = 0};

	eret = PtlMEAppend(ctx->ni_h, index, &me, PTL_PRIORITY_LIST, NULL, me_h);
	if (PTL_OK != eret)
		return eret;

	PtlEQWait(ctx->eq_h, &event);

	if (PTL_EVENT_LINK != event.type) {
		fprintf(stderr, "Failed to link LE\n");
		return -1;
	}
	if (PTL_NI_OK != event.ni_fail_type) {
		fprintf(stderr, "ni_fail_type != PTL_NI_OK");
		return -1;
	}
	return eret;
}

//int p4_me_list(p4_ctx_t * const ctx, ptl_handle_me_t * const me_h, void * const start, const ptl_size_t length, const ptl_index_t index){
//}

void p4_me_remove(ptl_handle_me_t me_h) {
	PtlMEUnlink(me_h);
}

int alloc_buffer_init(void** ptr, size_t bytes) {
	size_t page_size = sysconf(_SC_PAGESIZE);
	posix_memalign(ptr, page_size, bytes);
	if (NULL == *ptr)
		return -1;
	memset(*ptr, 'c', bytes);
	return 0;
}

void invalidate_cache(int* const cache_buffer, const size_t elements) {
	cache_buffer[0] = 1;
	for (size_t i = 1; i < elements; ++i) {
		cache_buffer[i] = cache_buffer[i - 1];
	}
}