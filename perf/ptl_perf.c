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
	ptl_process_t p_id;
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
				if (strcmp(optarg, "PINNED") == 0 ||
				    strcmp(optarg, "pinned") == 0) {
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

void* ptl_malloc(size_t size) {
	return malloc(size);
}

void ptl_free(void* ptr, size_t size) {
	if (size > 0)
		free(ptr);
}

void ptl_lock(void* ptr, size_t size) {
	mlock(ptr, size);
}

void ptl_unlock(void* ptr, size_t size) {
	munlock(ptr, size);
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
	struct ptl_mem_ops ops = {
	    .alloc = ptl_malloc,
	    .free = ptl_free,
	    .lock = ptl_lock,
	    .unlock = ptl_unlock,
	};
	CHECK(PtlSetMemOps(&ops));
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
	CHECK(PtlEQFree(ctx->eq_h));
	CHECK(PtlCTFree(ctx->ct_h));
	CHECK(PtlNIFini(ctx->ni_h));
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

typedef struct {
	ptl_handle_md_t md_h;
	ptl_process_t peer;
	double* timings;
	void* msg_buffer;
	ptl_size_t msg_size;
} msg_ctx_t;

void get_msg(ptl_ctx_t* ctx, parameter_t* p, msg_ctx_t* m_ctx) {
	ptl_size_t test = 0;
	ptl_ct_event_t event;
	unsigned int which = 0;
	ptl_size_t msg_size =
	    p->memory_mode == IOVEC ? p->n_iovec * msg_size : msg_size;

	for (int w = 0; w < p->warmup; ++w) {
		CHECK(PtlGet(
		    m_ctx->md_h, 0, msg_size, m_ctx->peer, DESIRED_PT_IDX, 0, 0, NULL));
	}
	CHECK(PtlCTPoll(&ctx->ct_h, &test, 1, PTL_TIME_FOREVER, &event, &which));
	if (event.failure > 0) {
		fprintf(stderr, "put_msg failed \n");
		return;
	}

	if (p->type == LATENCY) {
		for (int i = 0; i < p->iterations; ++i) {
			double t0 = get_wtime();
			CHECK(PtlGet(m_ctx->md_h,
			             0,
			             msg_size,
			             m_ctx->peer,
			             DESIRED_PT_IDX,
			             0,
			             0,
			             NULL));
			CHECK(PtlCTPoll(
			    &ctx->ct_h, &test, 1, PTL_TIME_FOREVER, &event, &which));
			m_ctx->timings[i] = get_wtime() - t0;
			if (event.failure > 0) {
				fprintf(stderr, "put_msg failed\n");
				return;
			}
		}
	}
	else {
		for (int i = 0; i < p->iterations; ++i) {
			double t0 = get_wtime();
			for (int w = 0; w < p->window_size; ++w) {
				CHECK(PtlGet(m_ctx->md_h,
				             0,
				             msg_size,
				             m_ctx->peer,
				             DESIRED_PT_IDX,
				             0,
				             0,
				             NULL));
			}
			CHECK(PtlCTPoll(
			    &ctx->ct_h, &test, 1, PTL_TIME_FOREVER, &event, &which));
			m_ctx->timings[i] = get_wtime() - t0;
			if (event.failure > 0) {
				fprintf(stderr, "put_msg failed\n");
				return;
			}
		}
	}
}

void put_msg(ptl_ctx_t* ctx, parameter_t* p, msg_ctx_t* m_ctx) {
	ptl_size_t test = 0;
	ptl_ct_event_t event;
	unsigned int which = 0;
	ptl_size_t msg_size =
	    p->memory_mode == IOVEC ? p->n_iovec * msg_size : msg_size;

	for (int w = 0; w < p->warmup; ++w) {
		CHECK(PtlPut(m_ctx->md_h,
		             0,
		             msg_size,
		             PTL_CT_ACK_REQ,
		             m_ctx->peer,
		             DESIRED_PT_IDX,
		             0,
		             0,
		             NULL,
		             0));
	}
	CHECK(PtlCTPoll(&ctx->ct_h, &test, 1, PTL_TIME_FOREVER, &event, &which));
	if (event.failure > 0) {
		fprintf(stderr, "put_msg failed \n");
		return;
	}

	if (p->type == LATENCY) {
		for (int i = 0; i < p->iterations; ++i) {
			double t0 = get_wtime();
			CHECK(PtlPut(m_ctx->md_h,
			             0,
			             msg_size,
			             PTL_CT_ACK_REQ,
			             m_ctx->peer,
			             DESIRED_PT_IDX,
			             0,
			             0,
			             NULL,
			             0));
			CHECK(PtlCTPoll(
			    &ctx->ct_h, &test, 1, PTL_TIME_FOREVER, &event, &which));
			m_ctx->timings[i] = get_wtime() - t0;
			if (event.failure > 0) {
				fprintf(stderr, "put_msg failed\n");
				return;
			}
		}
	}
	else {
		for (int i = 0; i < p->iterations; ++i) {
			double t0 = get_wtime();
			for (int w = 0; w < p->window_size; ++w) {
				CHECK(PtlPut(m_ctx->md_h,
				             0,
				             msg_size,
				             PTL_CT_ACK_REQ,
				             m_ctx->peer,
				             DESIRED_PT_IDX,
				             0,
				             0,
				             NULL,
				             0));
			}
			CHECK(PtlCTPoll(
			    &ctx->ct_h, &test, 1, PTL_TIME_FOREVER, &event, &which));
			m_ctx->timings[i] = get_wtime() - t0;
			if (event.failure > 0) {
				fprintf(stderr, "put_msg failed\n");
				return;
			}
		}
	}
}

void print_result(parameter_t* p, msg_ctx_t* m_ctx) {
	if (LATENCY == p->type) {
		printf("ID\tmsg_size\tlatency\n");
		for (int i = 0; i < p->iterations; ++i) {
			printf("%i\t%lu\t%f\n", i, m_ctx->msg_size, m_ctx->timings[i]);
		}
	}
	else {
		printf("ID\tmsg_size\tbandwidth\n");
		for (int i = 0; i < p->iterations; ++i) {
			double bw = (m_ctx->msg_size * p->window_size * 1e-6) /
			            (m_ctx->timings[i] * 1e-9);
			printf("%i\t%lu\t%f\n", i, m_ctx->msg_size, bw);
		}
	}
}

int run_client(parameter_t* p) {
	ptl_ctx_t ctx;
	ptl_handle_md_t md_h;
	ptl_index_t pt_idx;
	void* msg_buffer = NULL;
	ptl_iovec_t iovecs[256];

	CHECK(PtlInit());
	init_ptl_ctx(&ctx, p);

	size_t bytes = p->msg_size ? p->msg_size : p->max_msg_size;
	unsigned int options = 0;
	ptl_md_t md;

	switch (p->memory_mode) {
		case PINNED:
			msg_buffer = alloc_pinned_memory(bytes);
			md.start = msg_buffer;
			md.length = bytes;
			break;
		case FAULT:
			msg_buffer = alloc_pinned_memory(bytes);
			md.start = NULL;
			md.length = PTL_SIZE_MAX;
			break;
		case IOVEC:
			for (int i = 0; i < p->n_iovec; ++i) {
				iovecs[i].iov_base = alloc_pinned_memory(bytes);
				iovecs[i].iov_len = bytes;
			}
			md.start = &iovecs;
			md.length = p->n_iovec;
			options |= PTL_IOVEC;
			break;
	}
	options |= PTL_MD_VOLATILE | PTL_MD_EVENT_SUCCESS_DISABLE |
	           PTL_MD_EVENT_CT_REPLY | PTL_MD_EVENT_CT_ACK;
	md.options = options;
	md.ct_handle = ctx.ct_h;
	md.eq_handle = PTL_EQ_NONE;
	CHECK(PtlMDBind(ctx.ni_h, &md, &md_h));

	msg_ctx_t m_ctx;
	m_ctx.timings = malloc(p->iterations * sizeof(double));
	m_ctx.msg_buffer = msg_buffer;
	m_ctx.msg_size = p->msg_size;
	m_ctx.md_h = md_h;
	m_ctx.peer = p->peer;

	if (p->op == PUT) {
		if (p->msg_size) {
			put_msg(&ctx, p, &m_ctx);
			print_result(p, &m_ctx);
		}
		else if (p->min_msg_size && p->max_msg_size) {
			for (ptl_size_t m = p->min_msg_size; m <= p->max_msg_size; m *= 2) {
				m_ctx.msg_size = m;
				put_msg(&ctx, p, &m_ctx);
				print_result(p, &m_ctx);
			}
		}
	}
	if (p->op == GET) {
		if (p->msg_size) {
			get_msg(&ctx, p, &m_ctx);
			print_result(p, &m_ctx);
		}
		else if (p->min_msg_size && p->max_msg_size) {
			for (ptl_size_t m = p->min_msg_size; m <= p->max_msg_size; m *= 2) {
				m_ctx.msg_size = m;
				get_msg(&ctx, p, &m_ctx);
				print_result(p, &m_ctx);
			}
		}
	}
}

int run_server(parameter_t* p) {
	ptl_ctx_t ctx;
	ptl_handle_le_t le_h = PTL_INVALID_HANDLE;
	ptl_handle_me_t me_h = PTL_INVALID_HANDLE;
	ptl_index_t pt_idx;
	void* msg_buffer = NULL;
	ptl_iovec_t iovecs[256];

	CHECK(PtlInit());
	init_ptl_ctx(&ctx, p);

	printf("Client phys_pid: %lu\nClient phys.nid: %lu\n\n",
	       ctx.p_id.phys.pid,
	       ctx.p_id.phys.nid);

	CHECK(PtlPTAlloc(ctx.ni_h, 0, ctx.eq_h, DESIRED_PT_IDX, &pt_idx));

	size_t bytes = p->msg_size ? p->msg_size : p->max_msg_size;
	unsigned int options = 0;
	ptl_event_t event;

	if (p->ni_mode == MATCHING) {
		ptl_me_t me;
		if (PINNED == p->memory_mode) {
			msg_buffer = alloc_pinned_memory(bytes);
			me.start = msg_buffer;
			me.length = bytes;
			options |= PTL_ME_IS_ACCESSIBLE;
		}
		else if (FAULT == p->memory_mode) {
			me.start = NULL;
			me.length = PTL_SIZE_MAX;
		}
		else if (IOVEC == p->memory_mode) {
			for (int i = 0; i < p->n_iovec; ++i) {
				iovecs[i].iov_base = alloc_pinned_memory(bytes);
				iovecs[i].iov_len = bytes;
			}
			me.start = &iovecs;
			me.length = p->n_iovec;
			options |= PTL_IOVEC | PTL_ME_IS_ACCESSIBLE;
		}
		options |= PTL_ME_OP_PUT | PTL_ME_OP_GET | PTL_ME_EVENT_COMM_DISABLE |
		           PTL_ME_EVENT_UNLINK_DISABLE;
		me.options = options;
		me.uid = PTL_UID_ANY;
		me.ct_handle = PTL_CT_NONE;
		CHECK(PtlMEAppend(
		    ctx.ni_h, DESIRED_PT_IDX, &me, PTL_PRIORITY_LIST, NULL, &me_h));
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
			msg_buffer = alloc_pinned_memory(bytes);
			le.start = msg_buffer;
			le.length = bytes;
			options |= PTL_LE_IS_ACCESSIBLE;
		}
		else if (FAULT == p->memory_mode) {
			le.start = NULL;
			le.length = PTL_SIZE_MAX;
		}
		else if (IOVEC == p->memory_mode) {
			for (int i = 0; i < p->n_iovec; ++i) {
				iovecs[i].iov_base = alloc_pinned_memory(bytes);
				iovecs[i].iov_len = bytes;
			}
			le.start = &iovecs;
			le.length = p->n_iovec;
			options |= PTL_IOVEC | PTL_LE_IS_ACCESSIBLE;
		}
		options |= PTL_LE_OP_PUT | PTL_LE_OP_GET | PTL_LE_EVENT_COMM_DISABLE |
		           PTL_LE_EVENT_UNLINK_DISABLE;
		le.options = options;
		le.uid = PTL_UID_ANY;
		le.ct_handle = PTL_CT_NONE;
		CHECK(PtlLEAppend(
		    ctx.ni_h, DESIRED_PT_IDX, &le, PTL_PRIORITY_LIST, NULL, &le_h));
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

	printf("Server is ready and running. Type 'exit' to quit.\n");

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
	if (!PtlHandleIsEqual(le_h, PTL_INVALID_HANDLE)) {
		CHECK(PtlLEUnlink(le_h));
	}
	if (!PtlHandleIsEqual(me_h, PTL_INVALID_HANDLE)) {
		CHECK(PtlMEUnlink(me_h));
	}
	if (NULL != msg_buffer) {
		free_pinned_memory(msg_buffer, bytes);
	}
	if (IOVEC == p->memory_mode) {
		for (int i = 0; i < p->n_iovec; ++i) {
			free_pinned_memory(iovecs[i].iov_base, iovecs[i].iov_len);
		}
	}
	CHECK(PtlPTFree(ctx.ni_h, pt_idx));
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
