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

typedef enum { MATCHING = 0, NON_MATCHING } ni_mode_t;
typedef enum { LATENCY = 0, BANDWIDTH } benchmark_type_t;
typedef enum { PUT = 0, GET } operation_t;
typedef enum { REGISTERED = 0, UNREGISTERED } memory_mode_t;
typedef enum { COUNTING = 0, FULL } event_type_t;

typedef struct {
	ni_mode_t ni_mode;
	benchmark_type_t type;
	operation_t op;
	memory_mode_t memory_mode;
	event_type_t event_type;
	int iterations;
	int warmup;
	int window_size;
	int msg_size;
	int min_msg_size;
	int max_msg_size;
} benchmark_opts_t;

typedef struct {
	ptl_handle_ni_t ni_h;
	ptl_handle_eq_t eq_h;
	ptl_handle_ct_t ct_h;
	ptl_process_t my_addr;
	ptl_process_t peer_addr;
} p4_ctx_t;
#endif