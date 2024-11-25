#include <assert.h>
#include <getopt.h>
#include <sys/mman.h>
#include <time.h>
#include "common.h"
#include "util.h"

static int rank;
static int num_ranks;
static p4_ctx_t ctx;
static benchmark_opts_t opts;
static char processor_name[MPI_MAX_PROCESSOR_NAME];
int* cache_buffer;
size_t cache_buffer_size;

void run_triggered_ping_pong_benchmark() {
	ptl_handle_le_t le_h;
	ptl_handle_md_t md_h;
	ptl_index_t index;
	ptl_ct_event_t event;
	int eret = -1;
	void* buffer = NULL;

	double t0;
	double* rtt = malloc(opts.iterations * opts.warmup * sizeof(double));
	double* setup = malloc(opts.iterations * opts.warmup * sizeof(double));

	eret = p4_pt_alloc(&ctx, &index);
	if (PTL_OK != eret) {
		MPI_Abort(MPI_COMM_WORLD, -1);
	}

	if (0 == rank) {
		fprintf(stdout, "n,func,msg_size,rtt,setup_time\n");
	}

	alloc_buffer_init(&buffer, opts.msg_size);
	p4_le_insert_ct_comm(&ctx, &le_h, buffer, opts.msg_size, index);
	p4_md_alloc_ct(&ctx, &md_h, buffer, opts.msg_size);

	if (1 == rank) {
		for (int i = 1; i <= opts.iterations + opts.warmup; ++i) {
			t0 = MPI_Wtime();
			eret = PtlTriggeredPut(md_h,
			                       0,
			                       opts.msg_size,
			                       PTL_NO_ACK_REQ,
			                       ctx.peer_addr,
			                       index,
			                       0,
			                       0,
			                       NULL,
			                       0,
			                       ctx.ct_h,
			                       i);
			if (PTL_OK != eret) {
				fprintf(stderr, "PtlTriggeredPut failed with %i\n", eret);
				MPI_Abort(MPI_COMM_WORLD, -1);
			}
			setup[i - 1] = (MPI_Wtime() - t0) * 1e6;
		}
	}

	MPI_Barrier(MPI_COMM_WORLD);

	for (int i = 1; i <= opts.iterations + opts.warmup; ++i) {
		if (0 == rank) {
			t0 = MPI_Wtime();
			eret = PtlPut(md_h,
			              0,
			              opts.msg_size,
			              PTL_NO_ACK_REQ,
			              ctx.peer_addr,
			              index,
			              0,
			              0,
			              NULL,
			              0);

			if (PTL_OK != eret) {
				fprintf(stderr, "PtlPut failed with %i\n", eret);
				MPI_Abort(MPI_COMM_WORLD, -1);
			}
			eret = PtlCTWait(ctx.ct_h, i, &event);
			if (PTL_OK != eret) {
				fprintf(stderr, "PtlCTWait failed with %i\n", eret);
				MPI_Abort(MPI_COMM_WORLD, -1);
			}

			assert(0 == event.failure);
			assert(i == event.success);

			rtt[i - 1] = (MPI_Wtime() - t0) * 1e6;
		}
	}

	MPI_Barrier(MPI_COMM_WORLD);

	if (0 == rank)
		MPI_Recv(setup,
		         opts.iterations + opts.warmup,
		         MPI_DOUBLE,
		         1,
		         0,
		         MPI_COMM_WORLD,
		         MPI_STATUS_IGNORE);
	else
		MPI_Send(setup,
		         opts.iterations + opts.warmup,
		         MPI_DOUBLE,
		         0,
		         0,
		         MPI_COMM_WORLD);

	if (0 == rank) {
		for (int i = opts.warmup; i < opts.iterations + opts.warmup; ++i) {
			fprintf(stdout,
			        "%i,PtlTriggeredPut,%lu,%.4f,%.4f\n",
			        i - opts.warmup,
			        opts.msg_size,
			        rtt[i],
			        setup[i]);
		}
	}

	MPI_Barrier(MPI_COMM_WORLD);
	p4_le_remove(le_h);
	p4_md_free(md_h);
	free(buffer);
	free(rtt);
	free(setup);
	p4_pt_free(&ctx, index);
}

void run_ping_pong_benchmark() {
	ptl_handle_le_t le_h;
	ptl_handle_md_t md_h;
	ptl_index_t index;
	ptl_ct_event_t event;
	int eret = -1;
	void* buffer = NULL;

	double t0;
	double* time = malloc(opts.iterations * opts.warmup * sizeof(double));

	eret = p4_pt_alloc(&ctx, &index);
	if (PTL_OK != eret) {
		MPI_Abort(MPI_COMM_WORLD, -1);
	}

	if (0 == rank) {
		fprintf(stdout, "n,func,msg_size,rtt\n");
	}

	alloc_buffer_init(&buffer, opts.msg_size);
	p4_le_insert_ct_comm(&ctx, &le_h, buffer, opts.msg_size, index);
	p4_md_alloc_ct(&ctx, &md_h, buffer, opts.msg_size);

	MPI_Barrier(MPI_COMM_WORLD);

	for (int i = 1; i <= opts.iterations + opts.warmup; ++i) {
		if (0 == rank) {
			t0 = MPI_Wtime();
			eret = PtlPut(md_h,
			              0,
			              opts.msg_size,
			              PTL_NO_ACK_REQ,
			              ctx.peer_addr,
			              index,
			              0,
			              0,
			              NULL,
			              0);

			if (PTL_OK != eret) {
				fprintf(stderr, "PtlPut failed with %i\n", eret);
				MPI_Abort(MPI_COMM_WORLD, -1);
			}
			eret = PtlCTWait(ctx.ct_h, i, &event);
			if (PTL_OK != eret) {
				fprintf(stderr, "PtlCTWait failed with %i\n", eret);
				MPI_Abort(MPI_COMM_WORLD, -1);
			}

			assert(0 == event.failure);
			assert(i == event.success);

			time[i - 1] = (MPI_Wtime() - t0) * 1e6;
		}
		else {
			eret = PtlCTWait(ctx.ct_h, i, &event);
			if (PTL_OK != eret) {
				fprintf(stderr, "PtlCTWait failed with %i\n", eret);
				MPI_Abort(MPI_COMM_WORLD, -1);
			}

			assert(0 == event.failure);
			assert(i == event.success);

			eret = PtlPut(md_h,
			              0,
			              opts.msg_size,
			              PTL_NO_ACK_REQ,
			              ctx.peer_addr,
			              index,
			              0,
			              0,
			              NULL,
			              0);

			if (PTL_OK != eret) {
				fprintf(stderr, "PtlPut failed with %i\n", eret);
				MPI_Abort(MPI_COMM_WORLD, -1);
			}
		}
	}

	if (0 == rank) {
		for (int i = opts.warmup; i < opts.iterations + opts.warmup; ++i) {
			fprintf(stdout,
			        "%i,PtlPut,%lu,%.4f\n",
			        i - opts.warmup,
			        opts.msg_size,
			        time[i]);
		}
	}
	MPI_Barrier(MPI_COMM_WORLD);
	p4_le_remove(le_h);
	p4_md_free(md_h);
	free(buffer);
	free(time);
	p4_pt_free(&ctx, index);
}

void print_help_message() {
	printf("Usage: program_name [OPTIONS]\n");
	printf("Options:\n");
	printf(
	    "  -i, --iterations <arg>    Specify the number of iterations to run "
	    "(required argument).\n");
	printf(
	    "  -w, --warmup <arg>        Specify the number of warmup iterations "
	    "(required argument).\n");
	printf(
	    "  -m, --msg_size <arg>      Specify the message size in bytes "
	    "(required argument).\n");
	printf("  -h, --help                Display this help message and exit.\n");
}

int main(int argc, char* argv[]) {
	int eret = -1;
	ptl_ct_event_t zct = {.failure = 0, .success = 0};
	static const struct option long_opts[] = {
	    {"iterations", required_argument, NULL, 'i'},
	    {"warmup", required_argument, NULL, 'w'},
	    {"msg_size", required_argument, NULL, 'm'},
	    {"triggered", no_argument, NULL, 't'},
	    {"help", no_argument, NULL, 'h'}};

	const char* const short_opts = "i:w:m:th";

	opts.iterations = 5000;
	opts.msg_size = 512;
	opts.cache_size = _16MiB;
	opts.warmup = 100;
	int triggered = 0;

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
			case 'w':
				opts.warmup = atoi(optarg);
				break;
			case 't':
				triggered = 1;
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

	if (triggered)
		run_triggered_ping_pong_benchmark();
	else
		run_ping_pong_benchmark();

END:
	free(cache_buffer);
	destroy_p4_ctx(&ctx);
	PtlFini();
	MPI_Finalize();
	return eret;
}