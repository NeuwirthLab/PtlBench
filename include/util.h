#ifndef __UTIL_H__
#define __UTIL_H__
#include "common.h"
#include <portals4.h>

#include "bench_stats.h"
#include "options.h"

#define MiB (1024UL * 1024UL)
#define _16MiB (16 * MiB)

_Noreturn void bench_fail(const char* message);
void p4_check(int rc, const char* call);
void mpi_check(int rc, const char* call);
#define P4_CHECK(call) p4_check((call), #call)
#define MPI_CHECK(call) mpi_check((call), #call)
void wait_event(ptl_handle_eq_t eq, ptl_event_kind_t kind, ptl_event_t* event);
void wait_count(ptl_handle_ct_t ct, ptl_size_t threshold);
double* alloc_samples(size_t count);
void report_stats(size_t size, const char* metric, const char* unit,
                  double* samples, size_t count);
void validate_buffer(const void* buffer, size_t bytes, unsigned char expected);
void check_message_size(const p4_ctx_t* ctx, size_t size);

int init_p4_ctx(p4_ctx_t* const ctx, const ni_mode_t mode);
void destroy_p4_ctx(p4_ctx_t* const ctx);
int exchange_ni_address(p4_ctx_t* const ctx, const int my_rank);
int p4_pt_alloc(p4_ctx_t* const ctx, ptl_pt_index_t* const index);
void p4_pt_free(p4_ctx_t* const ctx, ptl_pt_index_t index);
int p4_md_alloc_ct(p4_ctx_t* const ctx, ptl_handle_md_t* const md_h,
                   void* const start, const ptl_size_t length);
int p4_md_alloc_eq(p4_ctx_t* const ctx, ptl_handle_md_t* const md_h,
                   void* const start, const ptl_size_t length);
void p4_md_free(ptl_handle_md_t md_h);
int p4_le_insert(p4_ctx_t* const ctx, ptl_handle_le_t* const le_h,
                 void* const start, const ptl_size_t length,
                 const ptl_pt_index_t index);
void p4_le_remove(ptl_handle_le_t le_h);
int p4_me_insert_persistent(p4_ctx_t* const ctx, ptl_handle_me_t* const me_h,
                            void* const start, const ptl_size_t length,
                            const ptl_pt_index_t index);
int p4_me_insert_list_use_once(p4_ctx_t* const ctx, ptl_handle_me_t* const me_h,
                               void* const* start, ptl_size_t* const length,
                               const int num_entries,
                               const ptl_pt_index_t index,
                               const ptl_handle_ct_t ct_handle);
void p4_me_remove(ptl_handle_me_t me_h);
int alloc_buffer_init(void** ptr, size_t bytes);
int p4_le_insert_empty(p4_ctx_t* const ctx, ptl_handle_le_t* const le_h,
                       const ptl_pt_index_t index);
int p4_le_insert_full_comm(p4_ctx_t* const ctx, ptl_handle_le_t* const le_h,
                           void* const start, const ptl_size_t length,
                           const ptl_pt_index_t index);
int p4_le_insert_ct_comm(p4_ctx_t* const ctx, ptl_handle_le_t* const le_h,
                         void* const start, const ptl_size_t length,
                         const ptl_pt_index_t index);
int p4_md_alloc_eq_empty(p4_ctx_t* const ctx, ptl_handle_md_t* const md_h);
void invalidate_cache(int* const cache_buffer, const size_t elements);
void configure_cache_pids(int requested);
#endif
