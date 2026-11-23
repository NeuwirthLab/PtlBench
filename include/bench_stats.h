#ifndef PTLBENCH_STATS_H
#define PTLBENCH_STATS_H

#include <stddef.h>

#define BENCH_STATS_OPTION 1002
#define BENCH_STATS_HEADER                                                     \
  "msg_size_bytes,metric,unit,sample_count,mean,median,min,max"

// Sorts the measured samples in place. A size of zero omits the size field.
int bench_print_stats(size_t size, const char* metric, const char* unit,
                      double* samples, size_t count);

#endif
