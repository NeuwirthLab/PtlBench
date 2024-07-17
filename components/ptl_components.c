#include <assert.h>
#include <getopt.h>
#include <limits.h>
#include <portals4.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

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

#define MiB 1024UL * 1024UL

double get_wtime() {
#define to_nsecs(secs) (secs * (long long) 1e9)
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (double) (to_nsecs(time.tv_sec) + time.tv_nsec);
}

void print_help() {
	printf("Usage: PtlComponents [options]\n");
	printf("Options:\n");
	printf("  -m, --matching             Description for matching option\n");
	printf(
	    "  -a, --inaccessible         Description for inaccessible option\n");
	printf("  -i, --iterations <arg>     Description for iterations option\n");
	printf("  -w, --warmup <arg>         Description for warmup option\n");
	printf("  -h, --help                 Display this help message\n");
}

void print_ni_limits(const ptl_ni_limits_t ni_limits) {
	printf("max_entries: %d\n", ni_limits.max_entries);
	printf("max_unexpected_headers: %d\n", ni_limits.max_unexpected_headers);
	printf("max_mds: %d\n", ni_limits.max_mds);
	printf("max_eqs: %d\n", ni_limits.max_eqs);
	printf("max_cts: %d\n", ni_limits.max_cts);
	printf("max_pt_index: %d\n", ni_limits.max_pt_index);
	printf("max_iovecs: %d\n", ni_limits.max_iovecs);
	printf("max_list_size: %d\n", ni_limits.max_list_size);
	printf("max_triggered_ops: %d\n", ni_limits.max_triggered_ops);
	printf("max_msg_size: %d\n", ni_limits.max_msg_size);
	printf("max_atomic_size: %d\n", ni_limits.max_atomic_size);
	printf("max_fetch_atomic_size: %d\n", ni_limits.max_fetch_atomic_size);
	printf("max_waw_ordered_size: %d\n", ni_limits.max_waw_ordered_size);
	printf("max_war_ordered_size: %d\n", ni_limits.max_war_ordered_size);
	printf("max_volatile_size: %d\n", ni_limits.max_volatile_size);

	if (ni_limits.features & PTL_TARGET_BIND_INACCESSIBLE) {
		printf("features: PTL_TARGET_BIND_INACCESIBLE\n");
	}
	if (ni_limits.features & PTL_TOTAL_DATA_ORDERING) {
		printf("features: PTL_TOTAL_DATA_ORDERING\n");
	}
	if (ni_limits.features & PTL_COHERENT_ATOMICS) {
		printf("features: PTL_COHERENT_ATOMICS\n");
	}
}

void generate_filename(const char* base_filename, char* filename, size_t size) {
	pid_t pid = getpid();
	snprintf(filename, size, "%s.dat", base_filename);
}

int main(int argc, char* argv[]) {
	ptl_handle_ni_t ni_h;

	static const struct option long_opts[] = {
	    {"matching", no_argument, NULL, 'm'},
	    {"inaccessible", no_argument, NULL, 'a'},
	    {"iterations", required_argument, NULL, 'i'},
	    {"warmup", required_argument, NULL, 'w'},
	    {"limits", no_argument, NULL, 'l'},
	    {"help", no_argument, NULL, 'h'}};

	int iterations = 10, warmup = 10;
	int matching = 0;
	int bind_inaccessible = 0;
	int limits = 0;

	const char* const short_opts = "mai:w:lh";

	while (1) {
		const int opt = getopt_long(argc, argv, short_opts, long_opts, NULL);

		if (-1 == opt)
			break;

		switch (opt) {
			case 'm':
				matching = 1;
				break;
			case 'a':
				bind_inaccessible = 1;
				break;
			case 'i':
				iterations = atoi(optarg);
				break;
			case 'w':
				warmup = atoi(optarg);
				break;
			case 'l':
				limits = 1;
				break;
			case 'h':
				print_help();
			case '?':
				print_help();
			default:
				print_help();
		}
	}

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
	    .features = bind_inaccessible ? PTL_TARGET_BIND_INACCESSIBLE : 0,
	};

	CHECK(PtlInit());

	unsigned int ni_matching = matching ? PTL_NI_MATCHING : PTL_NI_NO_MATCHING;
	CHECK(PtlNIInit(PTL_IFACE_DEFAULT,
	                ni_matching | PTL_NI_PHYSICAL,
	                PTL_PID_ANY,
	                &ni_requested_limits,
	                &ni_limits,
	                &ni_h));
	if (limits) {
		print_ni_limits(ni_limits);
	}

	//MDBind Malloc
	{
		char filename[256];
		generate_filename("MD-malloc", filename, sizeof(filename));
		FILE* file = fopen(filename, "w");

		if (NULL == file) {
			fprintf(stderr, "Failed to open file");
			return EXIT_FAILURE;
		}
		fprintf(file, "Iteration\tTime\n");

		ptl_size_t size = 500 * MiB;
		void* ptr = malloc(size);
		mlock(ptr, size);

		unsigned long md_options = PTL_MD_EVENT_SUCCESS_DISABLE |
		                           PTL_MD_EVENT_CT_REPLY | PTL_MD_EVENT_CT_ACK |
		                           PTL_MD_VOLATILE;
		ptl_md_t md = {
		    .start = ptr,
		    .length = size,
		    .options = md_options,
		    .eq_handle = PTL_EQ_NONE,
		};

		ptl_handle_md_t md_h;

		double t0 = 0, t1 = 0;
		for (int i = 0; i < iterations + warmup; ++i) {
			if (i >= warmup) {
				t0 = get_wtime();
			}
			CHECK(PtlMDBind(ni_h, &md, &md_h));
			if (i >= warmup) {
				t1 = get_wtime() - t0;
				fprintf(file, "%i\t%f\n", i - warmup, t1);
			}
			CHECK(PtlMDRelease(md_h));
		}
		munlock(ptr, size);
		free(ptr);
		fclose(file);
	}

	//MDBind Virtual
	{
		char filename[256];
		generate_filename("MD-size-max", filename, sizeof(filename));
		FILE* file = fopen(filename, "w");

		if (NULL == file) {
			fprintf(stderr, "Failed to open file");
			return EXIT_FAILURE;
		}
		fprintf(file, "Iteration\tTime\n");

		unsigned long md_options = PTL_MD_EVENT_SUCCESS_DISABLE |
		                           PTL_MD_EVENT_CT_REPLY | PTL_MD_EVENT_CT_ACK |
		                           PTL_MD_VOLATILE;
		ptl_md_t md = {
		    .start = NULL,
		    .length = PTL_SIZE_MAX,
		    .options = md_options,
		    .eq_handle = PTL_EQ_NONE,
		};

		ptl_handle_md_t md_h;

		for (int w = 0; w < warmup; ++w) {
			CHECK(PtlMDBind(ni_h, &md, &md_h));
			CHECK(PtlMDRelease(md_h));
		}

		double t0 = 0, t1 = 0;
		for (int i = 0; i < iterations + warmup; ++i) {
			if (i >= warmup) {
				t0 = get_wtime();
			}
			CHECK(PtlMDBind(ni_h, &md, &md_h));
			if (i >= warmup) {
				t1 = get_wtime() - t0;
				fprintf(file, "%i\t%f\n", i - warmup, t1);
			}
			CHECK(PtlMDRelease(md_h));
		}
		fclose(file);
	}

	//LEAppend
	{
		char filename[256];
		generate_filename("LE", filename, sizeof(filename));
		FILE* file = fopen(filename, "w");

		if (NULL == file) {
			fprintf(stderr, "Failed to open file");
			return EXIT_FAILURE;
		}
		fprintf(file, "Iteration\tTime\n");

		ptl_size_t size = 500 * MiB;
		void* ptr = malloc(size);
		mlock(ptr, size);

		ptl_handle_eq_t eq_h;
		ptl_pt_index_t pt_idx;

		CHECK(PtlEQAlloc(ni_h, 16, &eq_h));
		CHECK(PtlPTAlloc(ni_h, 0, eq_h, 0, &pt_idx));

		ptl_le_t le = {
		    .start = ptr,
		    .length = size,
		    .uid = PTL_UID_ANY,
		    .options = PTL_LE_OP_PUT | PTL_LE_OP_GET |
		               PTL_LE_EVENT_SUCCESS_DISABLE | PTL_LE_EVENT_COMM_DISABLE,
		    .ct_handle = PTL_CT_NONE,
		};

		ptl_handle_le_t le_h;
		ptl_event_t event;

		double t0 = 0, t1 = 0;
		for (int i = 0; i < iterations + warmup; ++i) {
			if (i >= warmup) {
				t0 = get_wtime();
			}
			CHECK(
			    PtlLEAppend(ni_h, pt_idx, &le, PTL_PRIORITY_LIST, NULL, &le_h));
			CHECK(PtlEQWait(eq_h, &event));
			if (i >= warmup) {
				t1 = get_wtime() - t0;
				fprintf(file, "%i\t%f\n", i - warmup, t1);
			}
			CHECK(PtlLEUnlink(le_h));
		}
		fclose(file);
		munlock(ptr, size);
		free(ptr);
		CHECK(PtlEQFree(eq_h));
		CHECK(PtlPTFree(ni_h, pt_idx));
	}

	//LEAppend Virtual
	{
		char filename[256];
		generate_filename("LE-size-max", filename, sizeof(filename));
		FILE* file = fopen(filename, "w");

		if (NULL == file) {
			fprintf(stderr, "Failed to open file");
			return EXIT_FAILURE;
		}
		fprintf(file, "Iteration\tTime\n");

		ptl_handle_eq_t eq_h;
		ptl_pt_index_t pt_idx;

		CHECK(PtlEQAlloc(ni_h, 16, &eq_h));
		CHECK(PtlPTAlloc(ni_h, 0, eq_h, 0, &pt_idx));

		ptl_le_t le = {
		    .start = NULL,
		    .length = PTL_SIZE_MAX,
		    .uid = PTL_UID_ANY,
		    .options = PTL_LE_OP_PUT | PTL_LE_OP_GET |
		               PTL_LE_EVENT_SUCCESS_DISABLE | PTL_LE_EVENT_COMM_DISABLE,
		    .ct_handle = PTL_CT_NONE,
		};

		ptl_handle_le_t le_h;
		ptl_event_t event;

		double t0 = 0, t1 = 0;
		for (int i = 0; i < iterations + warmup; ++i) {
			if (i >= warmup) {
				t0 = get_wtime();
			}
			CHECK(
			    PtlLEAppend(ni_h, pt_idx, &le, PTL_PRIORITY_LIST, NULL, &le_h));
			CHECK(PtlEQWait(eq_h, &event));
			if (i >= warmup) {
				t1 = get_wtime() - t0;
				fprintf(file, "%i\t%f\n", i - warmup, t1);
			}
			CHECK(PtlLEUnlink(le_h));
		}
		fclose(file);
		CHECK(PtlEQFree(eq_h));
		CHECK(PtlPTFree(ni_h, pt_idx));
	}

	// Triggered Ops setup time
	{
		char filename[256];
		generate_filename("triggered-put", filename, sizeof(filename));
		FILE* file = fopen(filename, "w");

		if (NULL == file) {
			fprintf(stderr, "Failed to open file");
			return EXIT_FAILURE;
		}
		fprintf(file, "Iteration\tTime\n");

		ptl_handle_eq_t eq_h;
		ptl_handle_ct_t ct_h;
		ptl_pt_index_t pt_idx;

		CHECK(PtlEQAlloc(ni_h, 16, &eq_h));
		CHECK(PtlCTAlloc(ni_h, &ct_h));
		CHECK(PtlPTAlloc(ni_h, 0, eq_h, 0, &pt_idx));

		ptl_le_t le = {
		    .start = NULL,
		    .length = PTL_SIZE_MAX,
		    .uid = PTL_UID_ANY,
		    .options = PTL_LE_OP_PUT | PTL_LE_OP_GET |
		               PTL_LE_EVENT_SUCCESS_DISABLE | PTL_LE_EVENT_COMM_DISABLE,
		    .ct_handle = PTL_CT_NONE,
		};

		ptl_handle_le_t le_h;
		ptl_event_t event;

		CHECK(PtlLEAppend(ni_h, pt_idx, &le, PTL_PRIORITY_LIST, NULL, &le_h));
		CHECK(PtlEQWait(eq_h, &event));

		unsigned long md_options = PTL_MD_EVENT_SUCCESS_DISABLE |
		                           PTL_MD_EVENT_CT_REPLY | PTL_MD_EVENT_CT_ACK |
		                           PTL_MD_VOLATILE;

		ptl_md_t md = {
		    .start = NULL,
		    .length = PTL_SIZE_MAX,
		    .options = md_options,
		    .eq_handle = PTL_EQ_NONE,
		};

		ptl_handle_md_t md_h;

		CHECK(PtlMDBind(ni_h, &md, &md_h));

		ptl_process_t peer;
		peer.phys.nid = PTL_NID_ANY;
		peer.phys.pid = PTL_PID_ANY;
		ptl_size_t thresh = 1;

		double t0 = 0, t1 = 0;

		for (int i = 0; i < iterations + warmup; ++i) {
			if (i >= warmup) {
				t0 = get_wtime();
			}

			CHECK(PtlTriggeredPut(md_h,
			                      0,
			                      1024,
			                      PTL_CT_ACK_REQ,
			                      peer,
			                      pt_idx,
			                      0,
					      0,
			                      NULL,
			                      0,
			                      ct_h,
			                      thresh));

			if (i >= warmup) {
				t1 = get_wtime() - t0;
				fprintf(file, "%i\t%f\n", i - warmup, t1);
			}
			CHECK(PtlCTCancelTriggered(ct_h));
		}

		fclose(file);
		CHECK(PtlLEUnlink(le_h));
		CHECK(PtlMDRelease(md_h));
		CHECK(PtlEQFree(eq_h));
		CHECK(PtlPTFree(ni_h, pt_idx));
	}

	CHECK(PtlNIFini(ni_h));
	PtlFini();

	return EXIT_SUCCESS;
}
