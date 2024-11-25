#ifndef __COMMON_H__
#define __COMMON_H__
#include <ctype.h>
#include <mpi.h>
#include <portals4.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum { MATCHING = 1, NON_MATCHING } ni_mode_t;
typedef enum { LATENCY = 1, BANDWIDTH } benchmark_type_t;
typedef enum { PUT = 1, GET } operation_t;
typedef enum { COUNTING = 1, FULL } event_type_t;
typedef enum { COLD = 1, HOT } page_state_t;
typedef enum { COLD_CACHE = 1, HOT_CACHE } cache_state_t;
typedef enum { ONE_SIDED = 1, PINGPONG } latency_pattern_t;

typedef struct {
	ni_mode_t ni_mode;
	benchmark_type_t type;
	operation_t op;
	event_type_t event_type;
	cache_state_t cache_state;
	int iterations;
	int warmup;
	int window_size;
	size_t msg_size;
	size_t min_msg_size;
	size_t max_msg_size;
	size_t cache_size;
} benchmark_opts_t;

typedef struct {
	int iterations;
	int warmup;
	int msg_size;
	size_t cache_size;
	page_state_t local_state;
	page_state_t remote_state;
	operation_t op;
	latency_pattern_t pattern;
} memory_benchmark_opts_t;

typedef struct {
	ptl_handle_ni_t ni_h;
	ptl_handle_eq_t eq_h;
	ptl_handle_ct_t ct_h;
	ptl_process_t my_addr;
	ptl_process_t peer_addr;
} p4_ctx_t;
#endif
