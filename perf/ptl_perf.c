#include <assert.h>
#include <getopt.h>
#include <limits.h>
#include <portals4.h>
#include <portals4_bxiext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DESIRED_PT_IDX 0
#define CHECK(stmt)                                       \
	do {                                                  \
		int ret = (stmt);                                 \
		if (PTL_OK != ret) {                              \
			fprintf(stderr,                               \
			        "[%s:%d] Ptl call failed with %d \n", \
			        __FILE__,                             \
			        __LINE__,                             \
			        ret);                                 \
			exit(EXIT_FAILURE);                           \
		}                                                 \
		assert(PTL_OK == ret);                            \
	} while (0)

typedef enum { MATCHING = 0, NON_MATCHING } ni_mode_t;
typedef enum { CLIENT = 0, SERVER } benchmark_role_t;
typedef enum { LATENCY = 0, BANDWIDTH } benchmark_type_t;
typedef enum { PUT = 0, GET } operation_t;
typedef enum { PINNED = 0, FAULT, IOVEC, IOVEC_BIDIR } memory_mode_t;

typedef struct {
	ni_mode_t ni_mode;
	benchmark_role_t role;
	benchmark_type_t type;
	operation_t op;
	memory_mode_t memory_mode;
	int iterations;
	int warmup;
	int window_size;
	int n_iovec;
	ptl_size_t iovec_size;
	ptl_size_t msg_size;
	ptl_size_t min_msg_size;
	ptl_size_t max_msg_size;
	ptl_process_t peer;
} parameter_t;

typedef struct {
	ptl_handle_ni_t ni_h;
	ptl_handle_eq_t eq_h;
	ptl_handle_ct_t ct_h;
	ptl_handle_md_t md_h;
	ptl_handle_le_t le_h;
	ptl_handle_me_t me_h;
	ptl_pt_index_t pt_idx;
	ptl_iovec_t iovecs[256];
	ptl_size_t n_iovecs;
	void* msg_buffer;
	ptl_size_t buffer_len;
	ptl_process_t p_id;
	ptl_process_t peer;
} ptl_ctx_t;

double get_wtime() {
#define to_nsecs(secs) (secs * (long long) 1e9)
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (double) (to_nsecs(time.tv_sec) + time.tv_nsec);
}

void print_help() {
	printf("Usage: program_name [options]\n");
	printf("Options:\n");
	printf(
	    "  -m, --matching              No argument. Description for matching "
	    "option.\n");
	printf(
	    "  -c, --client                No argument. Description for client "
	    "option.\n");
	printf(
	    "  -t, --type <arg>            Required argument. Description for type "
	    "option.\n");
	printf(
	    "  -o, --operation <arg>       Required argument. Description for "
	    "operation option.\n");
	printf(
	    "  -z, --memory-mode <arg>     Required argument. Description for "
	    "memory mode option.\n");
	printf(
	    "  -i, --iterations <arg>      Required argument. Description for "
	    "iterations option.\n");
	printf(
	    "  -w, --warmup <arg>          Required argument. Description for "
	    "warmup option.\n");
	printf(
	    "      --n_iovec <arg>         Required argument. Description for "
	    "n_iovec option.\n");
	printf(
	    "      --iovec_size <arg>      Required argument. Description for "
	    "iovec_size option.\n");
	printf(
	    "      --msg_size <arg>        Required argument. Description for "
	    "msg_size option.\n");
	printf(
	    "      --min_msg_size <arg>    Required argument. Description for "
	    "min_msg_size option.\n");
	printf(
	    "      --max_msg_size <arg>    Required argument. Description for "
	    "max_msg_size option.\n");
	printf(
	    "  -h, --help                  No argument. Display this help "
	    "message.\n");
}

int get_cmdline_opts(int* argc, char*** argv, parameter_t* p) {
	static const struct option long_opts[] = {
	    {"matching", no_argument, NULL, 'm'},
	    {"client", no_argument, NULL, 'c'},
	    {"type", required_argument, NULL, 't'},
	    {"operation", required_argument, NULL, 'o'},
	    {"memory-mode", required_argument, NULL, 'z'},
	    {"iterations", required_argument, NULL, 'i'},
	    {"warmup", required_argument, NULL, 'w'},
	    {"n_iovec", required_argument, NULL, 0},
	    {"iovec_size", required_argument, NULL, 1},
	    {"msg_size", required_argument, NULL, 2},
	    {"min_msg_size", required_argument, NULL, 3},
	    {"max_msg_size", required_argument, NULL, 4},
	    {"pid", required_argument, NULL, 5},
	    {"nid", required_argument, NULL, 6},
	    {"window-size", required_argument, NULL, 7},
	    {"help", no_argument, NULL, 'h'}};

	const char* const short_opts = "mct:o:z:i:w:h";

	p->ni_mode = NON_MATCHING;
	p->role = SERVER;
	p->op = PUT;
	p->type = LATENCY;
	p->memory_mode = PINNED;
	p->iterations = 10;
	p->warmup = 10;
	p->window_size = 64;
	p->n_iovec = 0;
	p->iovec_size = 0;
	p->msg_size = 1024;
	p->min_msg_size = 0;
	p->max_msg_size = 0;
	p->peer.phys.nid = PTL_NID_ANY;
	p->peer.phys.pid = PTL_PID_ANY;

	while (1) {
		const int opt = getopt_long(*argc, *argv, short_opts, long_opts, NULL);

		if (-1 == opt)
			break;

		switch (opt) {
			case 'm':
				p->ni_mode = MATCHING;
				break;
			case 'c':
				p->role = CLIENT;
				break;
			case 'i':
				p->iterations = atoi(optarg);
				break;
			case 'w':
				p->warmup = atoi(optarg);
				break;
			case 't':
				if ((strcmp(optarg, "BANDWIDTH") == 0) ||
				    (strcmp(optarg, "bandwidth")) == 0) {
					p->type = BANDWIDTH;
				}
				else {
					print_help();
					return EXIT_FAILURE;
				}
				break;
			case 'o':
				if ((strcmp(optarg, "GET") == 0) ||
				    (strcmp(optarg, "get")) == 0) {
					p->op = GET;
				}
				else {
					print_help();
					return EXIT_FAILURE;
				}
				break;
			case 'z':
				if (strcmp(optarg, "FAULT") == 0 ||
				    strcmp(optarg, "fault") == 0) {
					p->memory_mode = PINNED;
				}
				else if ((strcmp(optarg, "IOVEC") == 0) ||
				         (strcmp(optarg, "iovec")) == 0) {
					p->memory_mode = IOVEC;
				}
				else if ((strcmp(optarg, "IOVEC_BIDIR") == 0) ||
				         (strcmp(optarg, "iovec_bidir")) == 0) {
					p->memory_mode = IOVEC;
				}
				else {
					print_help();
					return EXIT_FAILURE;
				}
				break;
			case 0:
				p->n_iovec = atoi(optarg);
				break;
			case 1:
				p->iovec_size = atoll(optarg);
				break;
			case 2:
				p->msg_size = atoll(optarg);
				break;
			case 3:
				p->min_msg_size = atoll(optarg);
				p->msg_size = 0;
				break;
			case 4:
				p->max_msg_size = atoll(optarg);
				break;
			case 5:
				p->peer.phys.pid = atoi(optarg);
				break;
			case 6:
				p->peer.phys.nid = atoi(optarg);
				break;
			case 7:
				p->window_size = atoi(optarg);
				break;
			case 'h':
				print_help();
				return EXIT_SUCCESS;
			case '?':
				print_help();
				return EXIT_FAILURE;
			default:
				print_help();
				return EXIT_FAILURE;
		}
	}
	return EXIT_SUCCESS;
}

void init_ptl_ctx(ptl_ctx_t* ctx, parameter_t* p) {
	unsigned int ni_matching =
	    p->ni_mode == MATCHING ? PTL_NI_MATCHING : PTL_NI_NO_MATCHING;
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

	ctx->md_h = PTL_INVALID_HANDLE;
	ctx->le_h = PTL_INVALID_HANDLE;
	ctx->me_h = PTL_INVALID_HANDLE;
	ctx->eq_h = PTL_INVALID_HANDLE;
	ctx->ct_h = PTL_INVALID_HANDLE;
	ctx->ni_h = PTL_INVALID_HANDLE;
	ctx->pt_idx = PTL_PT_ANY;
	ctx->msg_buffer = NULL;
	ctx->buffer_len = 0;

	CHECK(PtlNIInit(PTL_IFACE_DEFAULT,
	                ni_matching | PTL_NI_PHYSICAL,
	                PTL_PID_ANY,
	                &ni_requested_limits,
	                &ni_limits,
	                &ctx->ni_h));

	CHECK(PtlGetPhysId(ctx->ni_h, &ctx->p_id));
	CHECK(PtlEQAlloc(ctx->ni_h, 1024, &ctx->eq_h));
	CHECK(PtlCTAlloc(ctx->ni_h, &ctx->ct_h));
}

void destroy_ptl_ctx(ptl_ctx_t* ctx) {
	if (!PtlHandleIsEqual(ctx->md_h, PTL_INVALID_HANDLE)) {
		CHECK(PtlMDRelease(ctx->md_h));
	}
	if (!PtlHandleIsEqual(ctx->le_h, PTL_INVALID_HANDLE)) {
		CHECK(PtlLEUnlink(ctx->le_h));
	}
	if (!PtlHandleIsEqual(ctx->le_h, PTL_INVALID_HANDLE)) {
		CHECK(PtlMEUnlink(ctx->me_h));
	}
	if (PTL_PT_ANY != ctx->pt_idx) {
		CHECK(PtlPTFree(ctx->ni_h, ctx->pt_idx));
	}
	if (!PtlHandleIsEqual(ctx->eq_h, PTL_INVALID_HANDLE)) {
		CHECK(PtlEQFree(ctx->eq_h));
	}
	if (!PtlHandleIsEqual(ctx->ct_h, PTL_INVALID_HANDLE)) {
		CHECK(PtlCTFree(ctx->ct_h));
	}
	if (!PtlHandleIsEqual(ctx->ni_h, PTL_INVALID_HANDLE)) {
		CHECK(PtlNIFini(ctx->ni_h));
	}
}

void* alloc_pinned_memory(size_t bytes) {
	void* ptr = malloc(bytes);
	mlock(ptr, bytes);
	return ptr;
}

void free_pinned_memory(void* ptr, size_t size) {
	munlock(ptr, size);
	free(ptr);
}

void get_msg(ptl_ctx_t* ctx,
             parameter_t* p,
             const ptl_size_t msg_size,
             double* timings) {
	ptl_ct_event_t event;
	const ptl_size_t offset =
	    p->memory_mode == FAULT ? (ptl_size_t) ctx->msg_buffer : 0;

	ptl_size_t nr = p->warmup;
	for (int w = 0; w < p->warmup; ++w) {
		CHECK(PtlGet(
		    ctx->md_h, 0, msg_size, ctx->peer, DESIRED_PT_IDX, 0, 0, NULL));
	}
	CHECK(PtlCTWait(ctx->ct_h, nr, &event));
	if (event.failure > 0) {
		fprintf(stderr, "put_msg failed \n");
		return;
	}

	if (p->type == LATENCY) {

		for (int i = 0; i < p->iterations; ++i, ++nr) {
			double t0 = get_wtime();
			CHECK(PtlGet(
			    ctx->md_h, 0, msg_size, ctx->peer, DESIRED_PT_IDX, 0, 0, NULL));
			CHECK(PtlCTWait(ctx->ct_h, nr, &event));
			timings[i] = get_wtime() - t0;
			if (event.failure > 0) {
				fprintf(stderr, "put_msg failed\n");
				return;
			}
		}
	}
	else {
		nr += p->window_size;
		for (int i = 0; i < p->iterations; ++i, nr += p->window_size) {
			double t0 = get_wtime();
			for (int w = 0; w < p->warmup; ++w) {
				for (int w = 0; w < p->window_size; ++w) {
					CHECK(PtlGet(ctx->md_h,
					             0,
					             msg_size,
					             ctx->peer,
					             DESIRED_PT_IDX,
					             0,
					             0,
					             NULL));
				}
			}
			CHECK(PtlCTWait(ctx->ct_h, nr, &event));
			timings[i] = get_wtime() - t0;
			if (event.failure > 0) {
				fprintf(stderr, "put_msg failed\n");
				return;
			}
		}
	}
}

void put_msg(ptl_ctx_t* ctx,
             parameter_t* p,
             const ptl_size_t msg_size,
             double* timings) {
	ptl_ct_event_t event;
	const ptl_size_t offset =
	    p->memory_mode == FAULT ? (ptl_size_t) ctx->msg_buffer : 0;

	ptl_size_t nr = p->warmup;
	for (int w = 0; w < p->warmup; ++w) {
		CHECK(PtlPut(ctx->md_h,
		             offset,
		             msg_size,
		             PTL_CT_ACK_REQ,
		             ctx->peer,
		             DESIRED_PT_IDX,
		             offset,
		             0,
		             NULL,
		             0));
	}

	CHECK(PtlCTWait(ctx->ct_h, nr, &event));
	if (event.failure > 0) {
		fprintf(stderr, "put_msg failed \n");
		return;
	}

	if (p->type == LATENCY) {
		for (int i = 0; i < p->iterations; ++i, ++nr) {
			double t0 = get_wtime();
			CHECK(PtlPut(ctx->md_h,
			             offset,
			             msg_size,
			             PTL_CT_ACK_REQ,
			             ctx->peer,
			             DESIRED_PT_IDX,
			             offset,
			             0,
			             NULL,
			             0));
			CHECK(PtlCTWait(ctx->ct_h, nr, &event));
			timings[i] = get_wtime() - t0;
			if (event.failure > 0) {
				fprintf(stderr, "put_msg failed\n");
				return;
			}
		}
	}
	else {
		nr += p->window_size;
		for (int i = 0; i < p->iterations; ++i, nr += p->window_size) {
			double t0 = get_wtime();
			for (int w = 0; w < p->window_size; ++w) {
				CHECK(PtlPut(ctx->md_h,
				             offset,
				             msg_size,
				             PTL_CT_ACK_REQ,
				             ctx->peer,
				             DESIRED_PT_IDX,
				             offset,
				             0,
				             NULL,
				             0));
			}
			CHECK(PtlCTWait(ctx->ct_h, nr, &event));
			timings[i] = get_wtime() - t0;

			if (event.failure > 0) {
				fprintf(stderr, "put_msg failed\n");
				return;
			}
		}
	}
}

void print_header(parameter_t* p) {
	if (LATENCY == p->type) {
		printf("ID\tmsg_size\tlatency\n");
	}
	else {
		printf("ID\tmsg_size\tbandwidth\n");
	}
}

void print_result(parameter_t* p, const ptl_size_t msg_size, double* timings) {
	if (LATENCY == p->type) {
		for (int i = 0; i < p->iterations; ++i) {
			printf("%i\t%lu\t%.4f\n", i, msg_size, timings[i]);
		}
	}
	else {
		for (int i = 0; i < p->iterations; ++i) {
			double bw =
			    (msg_size * p->window_size * 1e-6) / (timings[i] * 1e-9);
			printf("%i\t%lu\t%.4f\n", i, msg_size, bw);
		}
	}
}

int run_client(parameter_t* p) {
	ptl_ctx_t ctx;

	CHECK(PtlInit());
	init_ptl_ctx(&ctx, p);

	ctx.peer = p->peer;
	ctx.buffer_len = p->msg_size ? p->msg_size : p->max_msg_size;
	unsigned int options = 0;
	ptl_md_t md;

	switch (p->memory_mode) {
		case PINNED:
			ctx.msg_buffer = alloc_pinned_memory(ctx.buffer_len);
			md.start = ctx.msg_buffer;
			md.length = ctx.buffer_len;
			break;
		case FAULT:
			ctx.msg_buffer = alloc_pinned_memory(ctx.buffer_len);
			md.start = NULL;
			md.length = PTL_SIZE_MAX;
			break;
		case IOVEC:
			for (int i = 0; i < p->n_iovec; ++i) {
				ctx.iovecs[i].iov_base = alloc_pinned_memory(ctx.buffer_len);
				ctx.iovecs[i].iov_len = ctx.buffer_len;
			}
			ctx.n_iovecs = p->n_iovec;
			md.start = &ctx.iovecs;
			md.length = ctx.n_iovecs;
			options |= PTL_IOVEC;
			break;
	}

	options = PTL_MD_VOLATILE | PTL_MD_EVENT_SUCCESS_DISABLE |
	          PTL_MD_EVENT_CT_ACK | PTL_MD_EVENT_CT_REPLY;

	md.options = options;
	md.ct_handle = ctx.ct_h;
	md.eq_handle = PTL_EQ_NONE;

	CHECK(PtlMDBind(ctx.ni_h, &md, &ctx.md_h));

	double* timings = malloc(p->iterations * sizeof(double));

	print_header(p);

	if (p->op == PUT) {
		if (p->msg_size) {
			put_msg(&ctx, p, p->msg_size, timings);
			print_result(p, p->msg_size, timings);
		}
		else if (p->min_msg_size && p->max_msg_size) {
			for (ptl_size_t m = p->min_msg_size; m <= p->max_msg_size; m *= 2) {
				if (m > ctx.buffer_len) {
					fprintf(stderr, "Invalid msg size\n");
					goto END;
				}
				put_msg(&ctx, p, m, timings);
				print_result(p, m, timings);
			}
		}
	}
	if (p->op == GET) {
		if (p->msg_size) {
			get_msg(&ctx, p, p->msg_size, timings);
			print_result(p, p->msg_size, timings);
		}
		else if (p->min_msg_size && p->max_msg_size) {
			for (ptl_size_t m = p->min_msg_size; m <= p->max_msg_size; m *= 2) {
				get_msg(&ctx, p, m, timings);
				print_result(p, m, timings);
			}
		}
	}
END:
	if (NULL != ctx.msg_buffer) {
		free_pinned_memory(ctx.msg_buffer, ctx.buffer_len);
	}
	if (IOVEC == p->memory_mode) {
		for (int i = 0; i < p->n_iovec; ++i) {
			free_pinned_memory(ctx.iovecs[i].iov_base, ctx.iovecs[i].iov_len);
		}
	}
	if (NULL != timings) {
		free(timings);
	}

	destroy_ptl_ctx(&ctx);
	PtlFini();
}

int run_server(parameter_t* p) {
	ptl_ctx_t ctx;

	CHECK(PtlInit());
	init_ptl_ctx(&ctx, p);

	printf("Client phys_pid: %lu\nClient phys.nid: %lu\n\n",
	       ctx.p_id.phys.pid,
	       ctx.p_id.phys.nid);

	CHECK(PtlPTAlloc(ctx.ni_h, 0, ctx.eq_h, DESIRED_PT_IDX, &ctx.pt_idx));

	ctx.buffer_len = p->msg_size ? p->msg_size : p->max_msg_size;
	unsigned int options = 0;
	ptl_event_t event;

	if (p->ni_mode == MATCHING) {
		ptl_me_t me;
		if (PINNED == p->memory_mode) {
			ctx.msg_buffer = alloc_pinned_memory(ctx.buffer_len);
			me.start = ctx.msg_buffer;
			me.length = ctx.buffer_len;
			options |= PTL_ME_IS_ACCESSIBLE;
		}
		else if (FAULT == p->memory_mode) {
			ctx.msg_buffer = alloc_pinned_memory(ctx.buffer_len);
			me.start = NULL;
			me.length = PTL_SIZE_MAX;
		}
		else if (IOVEC == p->memory_mode) {
			for (int i = 0; i < p->n_iovec; ++i) {
				ctx.iovecs[i].iov_base = alloc_pinned_memory(ctx.buffer_len);
				ctx.iovecs[i].iov_len = ctx.buffer_len;
			}
			ctx.n_iovecs = p->n_iovec;
			me.start = &ctx.iovecs;
			me.length = ctx.n_iovecs;
			options |= PTL_IOVEC | PTL_ME_IS_ACCESSIBLE;
		}
		options |= PTL_ME_OP_PUT | PTL_ME_OP_GET | PTL_ME_EVENT_COMM_DISABLE |
		           PTL_ME_EVENT_UNLINK_DISABLE;
		me.options = options;
		me.uid = PTL_UID_ANY;
		me.ct_handle = PTL_CT_NONE;

		CHECK(PtlMEAppend(
		    ctx.ni_h, DESIRED_PT_IDX, &me, PTL_PRIORITY_LIST, NULL, &ctx.me_h));
		CHECK(PtlEQWait(ctx.eq_h, &event));

		if (PTL_EVENT_LINK != event.type) {
			fprintf(stderr, "Failed to link ME\n");
			return EXIT_FAILURE;
		}
		if (PTL_EVENT_LINK != event.ni_fail_type) {
			fprintf(stderr, "ni_fail_type != PTL_NI_OK");
			return EXIT_FAILURE;
		}
	}
	else {
		ptl_le_t le;
		if (PINNED == p->memory_mode) {
			ctx.msg_buffer = alloc_pinned_memory(ctx.buffer_len);
			le.start = ctx.msg_buffer;
			le.length = ctx.buffer_len;
			options |= PTL_LE_IS_ACCESSIBLE;
		}
		else if (FAULT == p->memory_mode) {
			ctx.msg_buffer = alloc_pinned_memory(ctx.buffer_len);
			le.start = NULL;
			le.length = PTL_SIZE_MAX;
		}
		else if (IOVEC == p->memory_mode) {
			for (int i = 0; i < p->n_iovec; ++i) {
				ctx.iovecs[i].iov_base = alloc_pinned_memory(ctx.buffer_len);
				ctx.iovecs[i].iov_len = ctx.buffer_len;
			}
			le.start = &ctx.iovecs;
			le.length = ctx.n_iovecs;
			options |= PTL_IOVEC | PTL_LE_IS_ACCESSIBLE;
		}
		options |= PTL_LE_OP_PUT | PTL_LE_OP_GET | PTL_LE_EVENT_COMM_DISABLE |
		           PTL_LE_EVENT_UNLINK_DISABLE;
		le.options = options;
		le.uid = PTL_UID_ANY;
		le.ct_handle = PTL_CT_NONE;

		CHECK(PtlLEAppend(
		    ctx.ni_h, DESIRED_PT_IDX, &le, PTL_PRIORITY_LIST, NULL, &ctx.le_h));
		CHECK(PtlEQWait(ctx.eq_h, &event));

		if (PTL_EVENT_LINK != event.type) {
			fprintf(stderr, "Failed to link LE\n");
			return EXIT_FAILURE;
		}
		if (PTL_NI_OK != event.ni_fail_type) {
			fprintf(stderr, "ni_fail_type != PTL_NI_OK");
			return EXIT_FAILURE;
		}
	}

	char input[256];

	printf("Server is ready and running PT idx is %i. Type 'exit' to quit.\n",
	       ctx.pt_idx);

	while (1) {
		printf("Enter command: ");
		if (fgets(input, sizeof(input), stdin) != NULL) {
			input[strcspn(input, "\n")] = '\0';
			if (strcmp(input, "exit") == 0) {
				break;
			}
		}
		else {
			printf("Error reading input. Exiting...\n");
			break;
		}
	}
END:
	if (NULL != ctx.msg_buffer) {
		free_pinned_memory(ctx.msg_buffer, ctx.buffer_len);
	}
	if (IOVEC == p->memory_mode) {
		for (int i = 0; i < p->n_iovec; ++i) {
			free_pinned_memory(ctx.iovecs[i].iov_base, ctx.iovecs[i].iov_len);
		}
	}
	destroy_ptl_ctx(&ctx);
	PtlFini();
	return EXIT_SUCCESS;
}

int main(int argc, char* argv[]) {
	int ret = 0;
	parameter_t param;

	ret = get_cmdline_opts(&argc, &argv, &param);
	if (ret != EXIT_SUCCESS) {
		return EXIT_FAILURE;
	}

	if (param.role == SERVER) {
		run_server(&param);
	}
	else {
		run_client(&param);
	}

	return EXIT_SUCCESS;
}
