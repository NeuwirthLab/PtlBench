#include <getopt.h>
#include "common.h"
#include "util.h"

static int rank;
static int num_ranks;
static benchmark_opts_t opts;
static p4_ctx_t ctx;

int* cache_buffer;
size_t cache_buffer_size;

int p4_put_latency() {
	int eret = -1;
	ptl_handle_md_t md_h;
	ptl_handle_le_t le_h;
	ptl_handle_me_t me_h;
	ptl_index_t index;
	ptl_ct_event_t ct_event;
	ptl_ct_event_t zero = {.success = 0, .failure = 0};
	ptl_event_t event;
	void* buffer = NULL;
	double t0, t;

	eret = p4_pt_alloc(&ctx, &index);
	if (PTL_OK != eret)
		return eret;
	ptl_match_bits_t match_bits = opts.ni_mode == MATCHING ? 0xDEADBEEF : 0;

	//print header
	if (0 == rank)
		fprintf(stdout, "func,msg_size,latency\n");

	for (size_t msg_size = opts.min_msg_size; msg_size <= opts.max_msg_size;
	     msg_size *= 2) {
		eret = alloc_buffer_init(&buffer, msg_size);
		if (0 > eret)
			return eret;

		if (1 == rank) {
			if (MATCHING == opts.ni_mode) {
				eret = p4_me_insert_persistent(
				    &ctx, &me_h, buffer, msg_size, index);
			}
			else {
				eret = p4_le_insert(&ctx, &le_h, buffer, msg_size, index);
			}
			if (PTL_OK != eret) {
				fprintf(stderr, "List entry insertion failed\n");
				return eret;
			}
		}

		MPI_Barrier(MPI_COMM_WORLD);

		if (0 == rank) {
			if (COUNTING == opts.event_type)
				eret = p4_md_alloc_ct(&ctx, &md_h, buffer, msg_size);
			else
				eret = p4_md_alloc_eq(&ctx, &md_h, buffer, msg_size);
			if (PTL_OK != eret) {
				free(buffer);
				fprintf(stderr, "md alloc failed with %i\n", eret);
				return eret;
			}

			for (int i = 0; i < opts.iterations + opts.warmup; ++i) {
				if (opts.cache_state == COLD_CACHE) {
					invalidate_cache(cache_buffer, cache_buffer_size);
				}
				if (i >= opts.warmup) {
					t0 = MPI_Wtime();
				}

				eret = PtlPut(md_h,
				              0,
				              msg_size,
				              PTL_ACK_REQ,
				              ctx.peer_addr,
				              index,
				              match_bits,
				              0,
				              NULL,
				              0);

				if (PTL_OK != eret) {
					fprintf(stderr, "PtlPut failed with %i\n", eret);
					p4_md_free(md_h);
					free(buffer);
					return eret;
				}
				if (COUNTING == opts.event_type) {
					eret = PtlCTWait(ctx.ct_h, i + 1, &ct_event);
					if (PTL_OK != eret || ct_event.failure > 0) {
						fprintf(stderr,
						        "PtlCTWait error %i %lu\n",
						        eret,
						        ct_event.failure);
						p4_md_free(md_h);
						free(buffer);
						return eret;
					}
				}
				else {
					eret = PtlEQWait(ctx.eq_h, &event);
					if (PTL_OK != eret || event.ni_fail_type != PTL_NI_OK) {
						fprintf(stderr, "fail_type %i\n", event.ni_fail_type);
						p4_md_free(md_h);
						free(buffer);
						return eret;
					}
				}
				if (i >= opts.warmup) {
					t = MPI_Wtime() - t0;
					fprintf(stdout, "put,%lu,%.4f\n", msg_size, t * 1e6);
					fflush(stdout);
				}
			}
			if (0 == rank && COUNTING == opts.event_type) {
				eret = PtlCTSet(ctx.ct_h, zero);
				if (PTL_OK != eret) {
					fprintf(stderr, "PtlCTSet failed %i\n", eret);
					return eret;
				}
			}
			p4_md_free(md_h);
		}
		MPI_Barrier(MPI_COMM_WORLD);
		if (1 == rank) {
			if (MATCHING == opts.ni_mode)
				p4_me_remove(me_h);
			else
				p4_le_remove(le_h);
		}
		free(buffer);
	}
	return 0;
}

int p4_get_latency() {
	int eret = -1;
	ptl_handle_md_t md_h;
	ptl_handle_le_t le_h;
	ptl_handle_me_t me_h;
	ptl_index_t index;
	ptl_ct_event_t ct_event;
	ptl_ct_event_t zero = {.success = 0, .failure = 0};
	ptl_event_t event;
	void* buffer = NULL;
	double t0, t;

	eret = p4_pt_alloc(&ctx, &index);
	if (PTL_OK != eret)
		return eret;
	ptl_match_bits_t match_bits = opts.ni_mode == MATCHING ? 0xDEADBEEF : 0;
	//print header
	if (0 == rank)
		fprintf(stdout, "func,msg_size,latency\n");

	for (size_t msg_size = opts.min_msg_size; msg_size <= opts.max_msg_size;
	     msg_size *= 2) {
		eret = alloc_buffer_init(&buffer, msg_size);
		if (0 > eret)
			return eret;

		if (1 == rank) {
			if (MATCHING == opts.ni_mode) {
				p4_me_insert_persistent(&ctx, &me_h, buffer, msg_size, index);
			}
			else {
				p4_le_insert(&ctx, &le_h, buffer, msg_size, index);
			}
		}

		MPI_Barrier(MPI_COMM_WORLD);

		if (0 == rank) {
			if (COUNTING == opts.event_type)
				eret = p4_md_alloc_ct(&ctx, &md_h, buffer, msg_size);
			else
				eret = p4_md_alloc_eq(&ctx, &md_h, buffer, msg_size);
			if (PTL_OK != eret) {
				free(buffer);
				return eret;
			}

			for (int i = 0; i < opts.iterations + opts.warmup; ++i) {
				if (opts.cache_state == COLD_CACHE) {
					invalidate_cache(cache_buffer, cache_buffer_size);
				}
				if (i >= opts.warmup) {
					t0 = MPI_Wtime();
				}
				eret = PtlGet(md_h,
				              0,
				              msg_size,
				              ctx.peer_addr,
				              index,
				              match_bits,
				              0,
				              NULL);
				if (PTL_OK != eret) {
					p4_md_free(md_h);
					free(buffer);
					return eret;
				}
				if (COUNTING == opts.event_type) {
					eret = PtlCTWait(ctx.ct_h, i + 1, &ct_event);
					if (PTL_OK != eret || ct_event.failure > 0) {
						p4_md_free(md_h);
						free(buffer);
						return eret;
					}
				}
				else {
					eret = PtlEQWait(ctx.eq_h, &event);
					if (PTL_OK != eret || event.ni_fail_type != PTL_NI_OK) {
						p4_md_free(md_h);
						free(buffer);
						return eret;
					}
				}
				if (i >= opts.warmup) {
					t = MPI_Wtime() - t0;
					fprintf(stdout, "get,%lu,%.4f\n", msg_size, t * 1e6);
					fflush(stdout);
				}
			}
			if (0 == rank && COUNTING == opts.event_type) {
				eret = PtlCTSet(ctx.ct_h, zero);
				if (PTL_OK != eret) {
					fprintf(stderr, "PtlCTSet failed with %i\n");
					return eret;
				}
			}
			p4_md_free(md_h);
		}
		MPI_Barrier(MPI_COMM_WORLD);
		if (1 == rank) {
			if (MATCHING == opts.ni_mode)
				p4_me_remove(me_h);
			else
				p4_le_remove(le_h);
		}
		free(buffer);
	}
	return 0;
}

int p4_put_bandwidth() {
	int eret = -1;
	ptl_handle_md_t md_h;
	ptl_handle_le_t le_h;
	ptl_handle_me_t me_h;
	ptl_size_t offset;
	ptl_index_t index;
	ptl_ct_event_t ct_event;
	ptl_ct_event_t zero = {.success = 0, .failure = 0};
	ptl_event_t event;
	void* buffer = NULL;
	double t0, t;

	eret = p4_pt_alloc(&ctx, &index);
	if (PTL_OK != eret)
		return eret;
	ptl_match_bits_t match_bits = opts.ni_mode == MATCHING ? 0xDEADBEEF : 0;
	//print header
	if (0 == rank)
		fprintf(stdout, "func,msg_size,bandwidth\n");

	for (size_t msg_size = opts.min_msg_size; msg_size <= opts.max_msg_size;
	     msg_size *= 2) {
		size_t bytes = opts.window_size * msg_size;
		eret = alloc_buffer_init(&buffer, bytes);
		if (0 > eret)
			return eret;

		if (1 == rank) {
			if (MATCHING == opts.ni_mode) {
				p4_me_insert_persistent(&ctx, &me_h, buffer, bytes, index);
			}
			else {
				p4_le_insert(&ctx, &le_h, buffer, bytes, index);
			}
		}

		MPI_Barrier(MPI_COMM_WORLD);

		if (0 == rank) {
			if (COUNTING == opts.event_type)
				eret = p4_md_alloc_ct(&ctx, &md_h, buffer, bytes);
			else
				eret = p4_md_alloc_eq(&ctx, &md_h, buffer, bytes);
			if (PTL_OK != eret) {
				free(buffer);
				return eret;
			}

			for (int i = 0; i < opts.iterations + opts.warmup; ++i) {
				if (opts.cache_state == COLD_CACHE) {
					invalidate_cache(cache_buffer, cache_buffer_size);
				}
				if (i >= opts.warmup) {
					t0 = MPI_Wtime();
				}
				for (int w = 0; w < opts.window_size; ++w) {
					offset = w * msg_size;
					eret = PtlPut(md_h,
					              offset,
					              msg_size,
					              PTL_ACK_REQ,
					              ctx.peer_addr,
					              index,
					              match_bits,
					              offset,
					              NULL,
					              0);
					if (PTL_OK != eret) {
						fprintf(stderr, "PtlPut failed with %i\n", eret);
						p4_md_free(md_h);
						free(buffer);
						return eret;
					}
				}
				if (COUNTING == opts.event_type) {
					eret = PtlCTWait(
					    ctx.ct_h, (i + 1) * opts.window_size, &ct_event);
					if (PTL_OK != eret || ct_event.failure > 0) {
						fprintf(stderr, "PtlCTWait failed\n");
						p4_md_free(md_h);
						free(buffer);
						return eret;
					}
				}
				else {
					eret = PtlEQWait(ctx.eq_h, &event);
					if (PTL_OK != eret || event.ni_fail_type != PTL_NI_OK) {
						p4_md_free(md_h);
						free(buffer);
						return eret;
					}
				}
				if (i >= opts.warmup) {
					t = MPI_Wtime() - t0;
					fprintf(stdout,
					        "put,%lu,%.4f\n",
					        msg_size,
					        (msg_size * opts.window_size * 1e-6) / t);
					fflush(stdout);
				}
			}
			if (0 == rank && COUNTING == opts.event_type) {
				eret = PtlCTSet(ctx.ct_h, zero);
				if (PTL_OK != eret) {
					fprintf(stderr, "PtlCTSet failed with %i\n");
					return eret;
				}
			}
			p4_md_free(md_h);
		}
		MPI_Barrier(MPI_COMM_WORLD);
		if (1 == rank) {
			if (MATCHING == opts.ni_mode)
				p4_me_remove(me_h);
			else
				p4_le_remove(le_h);
		}
		free(buffer);
	}
	return 0;
}

int p4_get_bandwidth() {
	int eret = -1;
	ptl_handle_md_t md_h;
	ptl_handle_le_t le_h;
	ptl_handle_me_t me_h;
	ptl_size_t offset;
	ptl_index_t index;
	ptl_ct_event_t ct_event;
	ptl_ct_event_t zero = {.success = 0, .failure = 0};
	ptl_event_t event;
	void* buffer = NULL;
	double t0, t;

	eret = p4_pt_alloc(&ctx, &index);
	if (PTL_OK != eret)
		return eret;
	ptl_match_bits_t match_bits = opts.ni_mode == MATCHING ? 0xDEADBEEF : 0;
	//print header
	if (0 == rank)
		fprintf(stdout, "func,msg_size,bandwidth\n");

	for (size_t msg_size = opts.min_msg_size; msg_size <= opts.max_msg_size;
	     msg_size *= 2) {
		size_t bytes = opts.window_size * msg_size;
		eret = alloc_buffer_init(&buffer, bytes);
		if (0 > eret)
			return eret;

		if (1 == rank) {
			if (MATCHING == opts.ni_mode) {
				p4_me_insert_persistent(&ctx, &me_h, buffer, bytes, index);
			}
			else {
				p4_le_insert(&ctx, &le_h, buffer, bytes, index);
			}
		}

		MPI_Barrier(MPI_COMM_WORLD);

		if (0 == rank) {
			if (COUNTING == opts.event_type)
				eret = p4_md_alloc_ct(&ctx, &md_h, buffer, bytes);
			else
				eret = p4_md_alloc_eq(&ctx, &md_h, buffer, bytes);
			if (PTL_OK != eret) {
				free(buffer);
				return eret;
			}

			for (int i = 0; i < opts.iterations + opts.warmup; ++i) {
				if (opts.cache_state == COLD_CACHE) {
					invalidate_cache(cache_buffer, cache_buffer_size);
				}
				if (i >= opts.warmup) {
					t0 = MPI_Wtime();
				}
				for (int w = 0; w < opts.window_size; ++w) {
					offset = w * msg_size;
					eret = PtlGet(md_h,
					              offset,
					              msg_size,
					              ctx.peer_addr,
					              index,
					              match_bits,
					              offset,
					              NULL);
					if (PTL_OK != eret) {
						p4_md_free(md_h);
						free(buffer);
						return eret;
					}
				}
				if (COUNTING == opts.event_type) {
					eret = PtlCTWait(
					    ctx.ct_h, (i + 1) * opts.window_size, &ct_event);
					if (PTL_OK != eret || ct_event.failure > 0) {
						p4_md_free(md_h);
						free(buffer);
						return eret;
					}
				}
				else {
					eret = PtlEQWait(ctx.eq_h, &event);
					if (PTL_OK != eret || event.ni_fail_type != PTL_NI_OK) {
						p4_md_free(md_h);
						free(buffer);
						return eret;
					}
				}
				if (i >= opts.warmup) {
					t = MPI_Wtime() - t0;
					fprintf(stdout,
					        "get,%lu,%.4f\n",
					        msg_size,
					        (msg_size * opts.window_size * 1e-6) / t);
					fflush(stdout);
				}
			}
			if (0 == rank && COUNTING == opts.event_type) {
				eret = PtlCTSet(ctx.ct_h, zero);
				if (PTL_OK != eret) {
					fprintf(stderr, "PtlCTSet failed with %i\n");
					return eret;
				}
			}
			p4_md_free(md_h);
		}
		MPI_Barrier(MPI_COMM_WORLD);
		if (1 == rank) {
			if (MATCHING == opts.ni_mode)
				p4_me_remove(me_h);
			else
				p4_le_remove(le_h);
		}
		free(buffer);
	}
	return 0;
}

void print_help_message() {
	fprintf(stdout, "Usage: [options]\n");
	fprintf(stdout, "Options:\n");
	fprintf(stdout,
	        "  -m, --matching                 Enable matching mode (no "
	        "argument required)\n");
	fprintf(stdout,
	        "  -b, --bandwidth                Enable bandwidth mode (no "
	        "argument required)\n");
	fprintf(stdout,
	        "  -g, --get                      Enable get operation (no "
	        "argument required)\n");
	fprintf(stdout,
	        "  -i, --iterations <value>       Specify the number of iterations "
	        "(required argument)\n");
	fprintf(stdout,
	        "  -x, --warmup <value>           Specify the number of warmup "
	        "iterations (required argument)\n");
	fprintf(stdout,
	        "  --msg_size <value>             Specify the message size "
	        "(required argument)\n");
	fprintf(stdout,
	        "  --min_msg_size <value>         Specify the minimum message size "
	        "(required argument)\n");
	fprintf(stdout,
	        "  --max_msg_size <value>         Specify the maximum message size "
	        "(required argument)\n");
	fprintf(stdout,
	        "  -w, --window-size <value>      Specify the window size "
	        "(required argument)\n");
	fprintf(stdout,
	        "  -c, --cache_size <value>       Specify the cache size (required "
	        "argument)\n");
	fprintf(stdout,
	        "  --cold_cache <value>           Enable cold cache mode with "
	        "specified cache size (required argument)\n");
	fprintf(stdout,
	        "  -f, --full                     Enable full mode (no argument "
	        "required)\n");
	fprintf(stdout,
	        "  -h, --help                     Display this help message (no "
	        "argument required)\n");
	fflush(stdout);
}

void print_benchmark_opts() {
	fprintf(stderr, "Benchmark Configuration:\n\n");
	fprintf(stderr,
	        "ni_mode: %s\n",
	        opts.ni_mode == MATCHING ? "MATCHING" : "NON MATCHING");
	fprintf(stderr, "op: %s\n", opts.op == PUT ? "PUT" : "GET");
	fprintf(
	    stdout, "type: %s\n", opts.type == LATENCY ? "LATENCY" : "BANDWIDTH");
	fprintf(stderr,
	        "event_type: %s\n",
	        opts.event_type == COUNTING ? "COUNTING" : "FULL");
	fprintf(stderr, "iterations: %i\n", opts.iterations);
	fprintf(stderr, "warmup: %i\n", opts.warmup);
	fprintf(stderr, "window_size: %i\n", opts.window_size);
	fprintf(stderr, "msg_size: %i\n", opts.msg_size);
	fprintf(stderr, "min_msg_size: %i\n", opts.min_msg_size);
	fprintf(stderr, "max_msg_size: %i\n\n", opts.max_msg_size);
	fflush(stderr);
}

int main(int argc, char* argv[]) {
	int eret = -1;

	static const struct option long_opts[] = {
	    {"matching", no_argument, NULL, 'm'},
	    {"bandwidth", no_argument, NULL, 'b'},
	    {"get", no_argument, NULL, 'g'},
	    {"iterations", required_argument, NULL, 'i'},
	    {"warmup", required_argument, NULL, 'x'},
	    {"msg_size", required_argument, NULL, 1},
	    {"min_msg_size", required_argument, NULL, 2},
	    {"max_msg_size", required_argument, NULL, 3},
	    {"window-size", required_argument, NULL, 'w'},
	    {"cache_size", required_argument, NULL, 'c'},
	    {"cold_cache", required_argument, NULL, 4},
	    {"full", no_argument, NULL, 'f'},
	    {"help", no_argument, NULL, 'h'}};

	const char* const short_opts = "mbgi:x:w:c:fh";

	opts.ni_mode = NON_MATCHING;
	opts.op = PUT;
	opts.type = LATENCY;
	opts.iterations = 10;
	opts.warmup = 10;
	opts.window_size = 64;
	opts.msg_size = 1024;
	opts.min_msg_size = 1;
	opts.max_msg_size = 4194304;
	opts.event_type = COUNTING;
	opts.cache_size = _16MiB;
	opts.cache_state = HOT_CACHE;

	while (1) {
		const int opt = getopt_long(argc, argv, short_opts, long_opts, NULL);

		if (-1 == opt)
			break;

		switch (opt) {
			case 'm':
				opts.ni_mode = MATCHING;
				break;
			case 'b':
				opts.type = BANDWIDTH;
				break;
			case 'g':
				opts.op = GET;
				break;
			case 'i':
				opts.iterations = atoi(optarg);
				break;
			case 'x':
				opts.warmup = atoi(optarg);
				break;
			case 'c':
				opts.cache_size = atoi(optarg);
				opts.cache_size *= MiB;
				break;
			case 1:
				opts.msg_size = atoi(optarg);
				opts.min_msg_size = opts.msg_size;
				opts.max_msg_size = opts.msg_size;
				break;
			case 2:
				opts.min_msg_size = atol(optarg);
				break;
			case 3:
				opts.max_msg_size = atol(optarg);
				break;
			case 4:
				opts.cache_state = COLD_CACHE;
				break;
			case 'w':
				opts.window_size = atol(optarg);
				break;
			case 'f':
				opts.event_type = FULL;
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
	} // end while

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

	if (2 != num_ranks) {
		fprintf(stdout, "Benchmark requires exactly two processes\n");
		MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
	}

	if (0 == rank)
		print_benchmark_opts();

	PtlInit();
	eret = init_p4_ctx(&ctx, opts.ni_mode);
	if (PTL_OK != eret)
		goto END;

	cache_buffer = malloc(opts.cache_size);
	if (NULL == cache_buffer)
		goto END;
	cache_buffer_size = opts.cache_size / sizeof(int);

	eret = exchange_ni_address(&ctx, rank);

	if (0 > eret)
		goto END;

	if (LATENCY == opts.type) {
		if (opts.op == PUT) {
			p4_put_latency();
		}
		else {
			p4_get_latency();
		}
	}
	else if (BANDWIDTH == opts.type) {
		if (opts.op == PUT) {
			p4_put_bandwidth();
		}
		else {
			p4_get_bandwidth();
		}
	}

END:
	destroy_p4_ctx(&ctx);
	PtlFini();
	MPI_Finalize();
	return eret;
}
