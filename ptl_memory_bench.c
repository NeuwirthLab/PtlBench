#include <getopt.h>
#include <sys/mman.h>
#include <time.h>
#include "common.h"
#include "util.h"

static int rank;
static int num_ranks;
static p4_ctx_t ctx;
static memory_benchmark_opts_t opts;
static size_t page_size;
static char processor_name[MPI_MAX_PROCESSOR_NAME];

int* cache_buffer;
size_t cache_buffer_size;
void** page_buffer;

int get_random_index() {
	int blocks = page_size / opts.msg_size;
	return rand() % blocks;
}

void touch_cold_pages(const int n_pages) {
	size_t ints_per_page = page_size / sizeof(int);
	int** buffer = (int**) page_buffer;
	for (int p = 0; p < n_pages; ++p)
		for (size_t i = 0; i < ints_per_page; ++i) {
			buffer[p][i] = i;
		}
}

int get_cold_pages(const int n_pages) {
	for (int p = 0; p < n_pages; ++p) {
		page_buffer[p] = mmap(NULL,
		                      page_size,
		                      PROT_READ | PROT_WRITE,
		                      MAP_PRIVATE | MAP_ANONYMOUS,
		                      -1,
		                      0);
	}
}

void free_cold_pages(const int n_pages) {
	size_t page_size = sysconf(_SC_PAGESIZE);
	for (int i = 0; i < n_pages; ++i) {
		munmap(page_buffer[i], page_size);
	}
}

void run_benchmark() {
	ptl_handle_le_t le_h;
	ptl_handle_md_t md_h;
	ptl_index_t index;
	ptl_event_t event;
	int eret = -1;

	p4_pt_alloc(&ctx, &index);

	if (0 == rank) {
		// print header
		fprintf(stdout,
		        "local_page_state,remote_page_state,msg_size,latency\n");
	}

	for (int i = 0; i < opts.iterations; ++i) {
		get_cold_pages(1);

		if (1 == rank) {
			p4_le_insert(&ctx, &le_h, page_buffer[0], page_size, index);
			if (HOT == opts.remote_state) {
				touch_cold_pages(1);
			}
		}
		else {
			p4_md_alloc_eq(&ctx, &md_h, page_buffer[0], page_size);
			if (HOT == opts.local_state) {
				touch_cold_pages(1);
			}
		}

		MPI_Barrier(MPI_COMM_WORLD);

		if (0 == rank) {
			ptl_size_t block_offset = get_random_index() * opts.msg_size;
			invalidate_cache(cache_buffer, cache_buffer_size);

			double t0 = MPI_Wtime();
			eret = PtlPut(md_h,
			              block_offset,
			              opts.msg_size,
			              PTL_ACK_REQ,
			              ctx.peer_addr,
			              index,
			              0,
			              block_offset,
			              NULL,
			              0);

			PtlEQWait(ctx.eq_h, &event);
			if (PTL_NI_OK != event.ni_fail_type) {
				fprintf(stderr, "PtlPut failed with %i\n", event.ni_fail_type);
				MPI_Abort(MPI_COMM_WORLD, -1);
			}
			double t = MPI_Wtime() - t0;
			fprintf(stdout,
			        "%s,%s,%i,%.4f\n",
			        opts.local_state == COLD ? "cold" : "hot",
			        opts.remote_state == COLD ? "cold" : "hot",
			        opts.msg_size,
			        t * 1e6);
			p4_md_free(md_h);
		}
		MPI_Barrier(MPI_COMM_WORLD);
		if (1 == rank) {
			p4_le_remove(le_h);
		}
		free_cold_pages(1);
	}
	p4_pt_free(&ctx, index);
}

void print_help_message() {
	printf("Usage: ptl_memory_bench [options]\n");
	printf("Options:\n");
	printf(
	    "  -i, --iterations <num>    Specify the number of iterations "
	    "(required argument)\n");
	printf(
	    "  -c, --cache-size <size>   Set the cache size in bytes (required "
	    "argument)\n");
	printf("  -l, --local-hot           Enable local-hot mode (no argument)\n");
	printf(
	    "  -r, --remote-hot          Enable remote-hot mode (no argument)\n");
	printf(
	    "  -m, --msg_size <size>     Set the message size in bytes (required "
	    "argument)\n");
	printf(
	    "  -h, --help                Display this help message (no "
	    "argument)\n");
}

int main(int argc, char* argv[]) {
	int eret = -1;

	static const struct option long_opts[] = {
	    {"iterations", required_argument, NULL, 'i'},
	    {"cache-size", required_argument, NULL, 'c'},
	    {"local-hot", no_argument, NULL, 'l'},
	    {"remote-hot", no_argument, NULL, 'r'},
	    {"msg_size", required_argument, NULL, 'm'},
	    {"help", no_argument, NULL, 'h'}};

	const char* const short_opts = "i:c:m:lrh";

	opts.iterations = 10;
	opts.msg_size = 512;
	opts.cache_size = _8MiB;
	opts.remote_state = COLD;
	opts.local_state = COLD;

	while (1) {
		const int opt = getopt_long(argc, argv, short_opts, long_opts, NULL);
		if (-1 == opt) {
			break;
		}
		switch (opt) {
			case 'i':
				opts.iterations = atoi(optarg);
				break;
			case 'c':
				opts.cache_size = atoi(optarg);
				opts.cache_size *= MiB;
				break;
			case 'm':
				opts.msg_size = atoi(optarg);
				break;
			case 'l':
				opts.local_state = HOT;
				break;
			case 'r':
				opts.remote_state = HOT;
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
	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);
	MPI_Get_processor_name(processor_name, &name_len);

	if (2 != num_ranks) {
		fprintf(stderr, "Benchmark requires exactly two processes\n");
		MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
	}

	fprintf(stderr, "Proc %i on %s\n", rank, processor_name);

	PtlInit();
	eret = init_p4_ctx(&ctx, PTL_NI_NO_MATCHING);
	if (PTL_OK != eret)
		goto END;

	eret = exchange_ni_address(&ctx, rank);

	if (0 > eret)
		goto END;

	cache_buffer = malloc(opts.cache_size);
	if (NULL == cache_buffer)
		goto END;
	cache_buffer_size = opts.cache_size / sizeof(int);

	page_size = sysconf(_SC_PAGESIZE);

	page_buffer = malloc(sizeof(void*) * opts.iterations);
	if (NULL == page_buffer)
		goto END;

	srand(time(0));

	run_benchmark();

END:
	free(cache_buffer);
	free(page_buffer);
	destroy_p4_ctx(&ctx);
	PtlFini();
	MPI_Finalize();
	return eret;
}