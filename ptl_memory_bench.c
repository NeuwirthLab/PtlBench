#include <getopt.h>
#include <sys/mman.h>
#include <time.h>
#include "common.h"
#include "util.h"

#define MiB 1024UL * 1024UL
#define _8MiB 8 * MiB

static int rank;
static int num_ranks;
static p4_ctx_t ctx;
static benchmark_opts_t opts;
static size_t page_size;

int* cache_buffer;
void** page_buffer;

int get_random_index() {
	int ints_per_page = page_size / sizeof(int);
	return rand() % ints_per_page;
}

void invalidate_cache() {
	size_t elements = opts.cache_size / sizeof(int);
	cache_buffer[0] = 1;

	for (size_t i = 1; i < elements; ++i) {
		cache_buffer[i] = cache_buffer[i - 1];
	}
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

void local_cold() {
	ptl_handle_le_t le_h;
	ptl_handle_md_t md_h;
	ptl_index_t index;
	ptl_event_t event;
	int eret = -1;
	void* buffer = NULL;

	p4_pt_alloc(&ctx, &index);

	if (0 == rank) {
		p4_md_alloc_eq_empty(&ctx, &md_h);
		get_cold_pages(opts.iterations);
	}
	else {
		alloc_buffer_init(&buffer, opts.iterations * page_size);
		p4_le_insert(&ctx, &le_h, buffer, opts.iterations * page_size, index);
	}

	MPI_Barrier(MPI_COMM_WORLD);

	if (0 == rank) {
		for (int i = 0; i < opts.iterations; ++i) {
			int* page = page_buffer[i];
			int idx = get_random_index();
			ptl_size_t rem_offset = i * page_size + idx * sizeof(int);

			invalidate_cache();

			double t0 = MPI_Wtime();
			eret = PtlPut(md_h,
			              (ptl_size_t) &page[idx],
			              512,
			              PTL_ACK_REQ,
			              ctx.peer_addr,
			              index,
			              0,
			              rem_offset,
			              NULL,
			              0);
			PtlEQWait(ctx.eq_h, &event);
			double t = MPI_Wtime() - t0;
			fprintf(stderr, "local_cold,%.4f\n", t * 1e6);
		}
		p4_md_free(md_h);
		free_cold_pages(opts.iterations);
	}
	MPI_Barrier(MPI_COMM_WORLD);
	if (1 == rank) {
		p4_le_remove(le_h);
		free(buffer);
	}
	p4_pt_free(&ctx, index);
}

void remote_cold() {
}

void cold_cold() {
	ptl_handle_le_t le_h;
	ptl_handle_md_t md_h;
	ptl_index_t index;
	ptl_event_t event;
	int eret = -1;
	unsigned long* addresses = NULL;

	p4_pt_alloc(&ctx, &index);
	get_cold_pages(opts.iterations);
	addresses = malloc(opts.iterations * sizeof(unsigned long));

	if (0 == rank) {
		p4_md_alloc_eq_empty(&ctx, &md_h);
		MPI_Recv(addresses,
		         opts.iterations,
		         MPI_UNSIGNED_LONG,
		         1,
		         1,
		         MPI_COMM_WORLD,
		         MPI_STATUS_IGNORE);
		//touch_cold_pages(opts.iterations);
	}
	else {
		p4_le_insert_empty(&ctx, &le_h, index);
		for (int i = 0; i < opts.iterations; ++i) {
			addresses[i] = (unsigned long) page_buffer[i];
		}
		MPI_Send(addresses,
		         opts.iterations,
		         MPI_UNSIGNED_LONG,
		         0,
		         1,
		         MPI_COMM_WORLD);
	}

	MPI_Barrier(MPI_COMM_WORLD);

	if (0 == rank) {
		for (int i = 0; i < opts.iterations; ++i) {
			int* page = page_buffer[i];
			int idx = get_random_index();
			ptl_size_t rem_offset = addresses[i] + idx * sizeof(int);

			invalidate_cache();

			double t0 = MPI_Wtime();
			eret = PtlPut(md_h,
			              (ptl_size_t) &page[idx],
			              512,
			              PTL_ACK_REQ,
			              ctx.peer_addr,
			              index,
			              0,
			              rem_offset,
			              NULL,
			              0);
			PtlEQWait(ctx.eq_h, &event);
			double t = MPI_Wtime() - t0;
			fprintf(stderr, "cold_cold,%.4f\n", t * 1e6);
		}
		p4_md_free(md_h);
	}
	MPI_Barrier(MPI_COMM_WORLD);
	free_cold_pages(opts.iterations);
	if (1 == rank) {
		p4_le_remove(le_h);
	}
	free(addresses);
	p4_pt_free(&ctx, index);
}

void warm_warm() {
	ptl_handle_le_t le_h;
	ptl_handle_md_t md_h;
	ptl_index_t index;
	ptl_event_t event;
	int eret = -1;
	void* buffer = NULL;

	p4_pt_alloc(&ctx, &index);

	if (0 == rank) {
		alloc_buffer_init(&buffer, opts.iterations * page_size);
		p4_md_alloc_eq(&ctx, &md_h, buffer, opts.iterations * page_size);
	}
	else {
		alloc_buffer_init(&buffer, opts.iterations * page_size);
		p4_le_insert(&ctx, &le_h, buffer, opts.iterations * page_size, index);
	}

	MPI_Barrier(MPI_COMM_WORLD);

	if (0 == rank) {
		for (int i = 0; i < opts.iterations; ++i) {

			int idx = get_random_index();
			ptl_size_t offset = i * page_size + idx * sizeof(int);

			invalidate_cache();
			double t0 = MPI_Wtime();
			eret = PtlPut(md_h,
			              offset,
			              512,
			              PTL_ACK_REQ,
			              ctx.peer_addr,
			              index,
			              0,
			              offset,
			              NULL,
			              0);
			PtlEQWait(ctx.eq_h, &event);
			double t = MPI_Wtime() - t0;
			fprintf(stderr, "warm_warm,%.4f\n", t * 1e6);
		}
		p4_md_free(md_h);
	}
	MPI_Barrier(MPI_COMM_WORLD);
	if (1 == rank) {
		p4_le_remove(le_h);
	}
	free(buffer);
	p4_pt_free(&ctx, index);
}
void print_help_message() {};

int main(int argc, char* argv[]) {
	int eret = -1;

	static const struct option long_opts[] = {
	    {"matching", no_argument, NULL, 'm'},
	    {"registered", no_argument, NULL, 'r'},
	    {"get", no_argument, NULL, 'g'},
	    {"iterations", required_argument, NULL, 'i'},
	    {"cache-size", required_argument, NULL, 'c'},
	    {"msg_size", required_argument, NULL, 1},
	    {"help", no_argument, NULL, 'h'}};

	const char* const short_opts = "mrghi:c:";

	opts.op = PUT;
	opts.iterations = 10;
	opts.msg_size = 1024;
	opts.cache_size = _8MiB;

	while (1) {
		const int opt = getopt_long(argc, argv, short_opts, long_opts, NULL);
		if (-1 == opt) {
			break;
		}
		switch (opt) {
			case 'm':
				opts.ni_mode = MATCHING;
				break;
			case 'r':
				opts.memory_mode = REGISTERED;
				break;
			case 'g':
				opts.op = GET;
				break;
			case 'i':
				opts.iterations = atoi(optarg);
				break;
			case 'c':
				opts.cache_size = atoi(optarg);
				opts.cache_size *= MiB;
				break;
			case 1:
				opts.msg_size = atoi(optarg);
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
	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

	if (2 != num_ranks) {
		fprintf(stderr, "Benchmark requires exactly two processes\n");
		MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
	}

	PtlInit();
	eret = init_p4_ctx(&ctx, opts.ni_mode);
	if (PTL_OK != eret)
		goto END;

	eret = exchange_ni_address(&ctx, rank);

	if (0 > eret)
		goto END;

	cache_buffer = malloc(opts.cache_size);
	if (NULL == cache_buffer)
		goto END;

	page_size = sysconf(_SC_PAGESIZE);

	page_buffer = malloc(sizeof(void*) * opts.iterations);
	if (NULL == page_buffer)
		goto END;

	srand(time(0));

	//warm_warm();
	//local_cold();
	cold_cold();

END:
	free(cache_buffer);
	destroy_p4_ctx(&ctx);
	PtlFini();
	MPI_Finalize();
	return eret;
}