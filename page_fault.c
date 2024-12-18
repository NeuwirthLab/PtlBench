#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ITERATIONS 10000
#define WARMUP 100
#define PAGES (ITERATIONS + WARMUP)
#define MiB 1024UL * 1024UL
#define _16MiB 16 * MiB

static inline double Wtime() {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return ((time.tv_sec * 1e9) + time.tv_nsec);
}

void invalidate_cache(int* const cache_buffer, const size_t elements) {
	cache_buffer[0] = 1;
	for (size_t i = 1; i < elements; ++i) {
		cache_buffer[i] = cache_buffer[i - 1];
	}
}

int main(int argc, char* argv[]) {
	int* cache_buffer = NULL;
	size_t cache_buffer_size;
	void** page_buffer = NULL;
	size_t page_size = sysconf(_SC_PAGESIZE);
	double t0, t;

	srand(time(0));
	page_buffer = malloc(PAGES * sizeof(void*));
	if (NULL == page_buffer)
		goto END;

	cache_buffer_size = _16MiB;
	cache_buffer = malloc(cache_buffer_size);
	if (NULL == cache_buffer)
		goto END;

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
		invalidate_cache(cache_buffer, cache_buffer_size / sizeof(int));
		t0 = Wtime();
		page[rand() % 1024] = 0x92;
		t = Wtime() - t0;
		fprintf(stdout, "%i,%.6f\n", i, t);
	}

END:
	fflush(stdout);
	for (int i = 0; i < PAGES; ++i) {
		if (MAP_FAILED != page_buffer[i])
			munmap(page_buffer[i], page_size);
	}
	free(page_buffer);
	free(cache_buffer);
	return 0;
}