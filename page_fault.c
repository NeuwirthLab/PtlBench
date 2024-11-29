#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ITERATIONS 10000
#define WARMUP 100
#define PAGES (ITERATIONS + WARMUP)

static inline double Wtime() {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return ((time.tv_sec * 1e9) + time.tv_nsec);
}

int main(int argc, char* argv[]) {
	int* chace_buffer = NULL;
	size_t cache_buffer_size;
	void** page_buffer = NULL;
	size_t page_size = sysconf(_SC_PAGESIZE);
	double t0, t;

	srand(time(0));
	page_buffer = malloc(PAGES * sizeof(void*));

	for (int i = 0; i < PAGES; ++i) {
		page_buffer[i] = mmap(NULL,
		                      page_size,
		                      PROT_READ | PROT_WRITE,
		                      MAP_PRIVATE | MAP_ANONYMOUS,
		                      -1,
		                      0);
		if (MAP_FAILED == page_buffer[i]) {
			fprintf(stderr, "mmaping failed\n");
			goto END;
		}
	}

	fprintf(stdout, "count,time\n");

	for (int i = 0; i < PAGES; ++i) {
		int* page = (int*) page_buffer[i];
		t0 = Wtime();
		page[rand() % 1024] = 0x92;
		t = Wtime() - t0;
		fprintf(stdout, "%i,%.6f\n", i, t);
	}

END:
	for (int i = 0; i < PAGES; ++i) {
		if (MAP_FAILED != page_buffer[i])
			munmap(page_buffer[i], page_size);
	}
	free(page_buffer);
	return 0;
}