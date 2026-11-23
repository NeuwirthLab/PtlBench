#include "bench_stats.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int
compare_samples(const void* a, const void* b)
{
  double left = *(const double*)a;
  double right = *(const double*)b;
  return (left > right) - (left < right);
}

int
bench_print_stats(size_t size, const char* metric, const char* unit,
                  double* samples, size_t count)
{
  if(count == 0)
    return -1;

  long double mean = 0;
  for(size_t i = 0; i < count; ++i)
  {
    if(!isfinite(samples[i]) || samples[i] < 0)
      return -1;
    mean += ((long double)samples[i] - mean) / (i + 1);
  }

  qsort(samples, count, sizeof(*samples), compare_samples);
  double lower = samples[(count - 1) / 2];
  double median = lower + (samples[count / 2] - lower) / 2.0;
  if(size != 0 && printf("%zu", size) < 0)
    return -1;
  return printf(",%s,%s,%zu,%.17g,%.17g,%.17g,%.17g\n", metric, unit, count,
                (double)mean, median, samples[0], samples[count - 1]) < 0
             ? -1
             : 0;
}
