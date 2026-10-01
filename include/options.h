#ifndef PTLBENCH_OPTIONS_H
#define PTLBENCH_OPTIONS_H
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static inline void
option_error(const char* message)
{
  fprintf(stderr, "%s\n", message);
  exit(EXIT_FAILURE);
}

static inline size_t
parse_size(const char* value)
{
  char* end;
  errno = 0;
  unsigned long long n = strtoull(value, &end, 10);
  if(value[0] < '0' || value[0] > '9' || *end || errno || n > SIZE_MAX)
    option_error("Expected an unsigned decimal integer within size_t range");
  return (size_t)n;
}

static inline int
parse_int(const char* value)
{
  size_t n = parse_size(value);
  if(n > INT_MAX)
    option_error("Integer exceeds INT_MAX");
  return (int)n;
}

static inline size_t
parse_mib(const char* value)
{
  size_t n = parse_size(value);
  if(n == 0 || n > SIZE_MAX / (1024UL * 1024UL))
    option_error("Cache size must be a positive number of MiB");
  return n * (1024UL * 1024UL);
}

static inline void
validate_iterations(int iterations, int warmup)
{
  if(iterations <= 0 || warmup < 0 || iterations >= INT_MAX - warmup ||
     (size_t)(iterations + warmup) > SIZE_MAX / sizeof(double))
    option_error("Invalid iterations/warmup: need positive iterations and a "
                 "bounded sum");
}

static inline void
validate_range(size_t minimum, size_t maximum, int window)
{
  if(!minimum || minimum > maximum || maximum > SIZE_MAX / 2 || window <= 0 ||
     maximum > SIZE_MAX / (size_t)window)
    option_error(
        "Invalid message range or window size (zero, reversed, or overflow)");
}
#endif
