#include "include/options.h"
#include <getopt.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double
wtime(void)
{
  struct timespec time;
  if(clock_gettime(CLOCK_MONOTONIC, &time) != 0)
  {
    perror("clock_gettime");
    exit(EXIT_FAILURE);
  }
  return (double)time.tv_sec * 1e9 + time.tv_nsec;
}

int
main(int argc, char* argv[])
{
  int iterations = 10000, warmup = 100, table = 0, result = EXIT_FAILURE;
  unsigned int seed = 1;
  static const struct option options[] = {
      {"iterations", required_argument, NULL, 'i'},
      {"warmup", required_argument, NULL, 'w'},
      {"seed", required_argument, NULL, 's'},
      {"table", no_argument, NULL, 't'},
      {"help", no_argument, NULL, 'h'},
      {0, 0, 0, 0}};
  int opt;
  while((opt = getopt_long(argc, argv, "i:w:s:th", options, NULL)) != -1)
  {
    switch(opt)
    {
    case 'i':
      iterations = parse_int(optarg);
      break;
    case 'w':
      warmup = parse_int(optarg);
      break;
    case 's':
      seed = (unsigned int)parse_int(optarg);
      break;
    case 't':
      table = 1;
      break;
    case 'h':
      puts("Usage: pf_bench [-i iterations] [-w warmup] [--seed N] [--table]\n"
           "Defaults: 10000 iterations, 100 warmup, seed 1; CSV time in ns");
      return EXIT_SUCCESS;
    default:
      return EXIT_FAILURE;
    }
  }
  if(optind != argc)
    option_error("Unexpected positional argument");
  validate_iterations(iterations, warmup);
  int pages = iterations + warmup;
  long system_page_size = sysconf(_SC_PAGESIZE);
  if(system_page_size < (long)sizeof(int) ||
     (size_t)pages > SIZE_MAX / sizeof(void*))
    option_error("Invalid page size or page count");
  size_t page_size = (size_t)system_page_size;
  size_t mapped = 0;
  void** page_buffer = calloc((size_t)pages, sizeof(void*));
  size_t cache_bytes = 16UL * 1024 * 1024;
  int* cache_buffer = malloc(cache_bytes);
  if(!page_buffer || !cache_buffer)
  {
    fprintf(stderr, "Allocation failed\n");
    goto end;
  }
  srand(seed);
  for(int i = 0; i < pages; ++i)
  {
    page_buffer[i] = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(page_buffer[i] == MAP_FAILED)
    {
      perror("mmap");
      goto end;
    }
    ++mapped;
  }
  fprintf(stderr,
          "iterations=%d warmup=%d page_bytes=%zu seed=%u "
          "cache_sweep_bytes=%zu; first store time in ns\n",
          iterations, warmup, page_size, seed, cache_bytes);
  puts(table ? "Sample       First store (ns)" : "count,time");
  for(int i = 0; i < pages; ++i)
  {
    volatile int* page = page_buffer[i];
    volatile int* cache = cache_buffer;
    cache[0] = 1;
    for(size_t j = 1; j < cache_bytes / sizeof(int); ++j)
      cache[j] = cache[j - 1];
    size_t offset = (size_t)rand() % (page_size / sizeof(int));
    double t0 = wtime();
    page[offset] = 0x92;
    double t = wtime() - t0;
    if(i >= warmup)
      printf(table ? "%6d %22.6f\n" : "%i,%.6f\n", i, t);
  }
  result = EXIT_SUCCESS;
end:
  for(size_t i = 0; i < mapped; ++i)
    if(munmap(page_buffer[i], page_size) != 0)
    {
      perror("munmap");
      result = EXIT_FAILURE;
    }
  free(page_buffer);
  free(cache_buffer);
  return result;
}
