#ifndef __UTIL_H__
#define __UTIL_H__
#include <portals4.h>
#include "common.h"
int init_p4_ctx(p4_ctx_t* const ctx, const ni_mode_t mode);
void destroy_p4_ctx(p4_ctx_t* const ctx);
int exchange_ni_address(p4_ctx_t* const ctx, const int my_rank);
int p4_pt_alloc(p4_ctx_t* const ctx, ptl_index_t* const index);
void p4_pt_free(p4_ctx_t* const ctx, ptl_index_t index);
int p4_md_alloc_ct(p4_ctx_t* const ctx,
                   ptl_handle_md_t* const md_h,
                   void* const start,
                   const ptl_size_t length);
int p4_md_alloc_eq(p4_ctx_t* const ctx,
                   ptl_handle_md_t* const md_h,
                   void* const start,
                   const ptl_size_t length);
void p4_md_free(ptl_handle_md_t md_h);
int p4_le_insert(p4_ctx_t* const ctx,
                 ptl_handle_le_t* const le_h,
                 void* const start,
                 const ptl_size_t length,
                 const ptl_index_t index);
void p4_le_remove(ptl_handle_le_t le_h);
int p4_me_insert_persistent(p4_ctx_t* const ctx,
                            ptl_handle_me_t* const me_h,
                            void* const start,
                            const ptl_size_t length,
                            const ptl_index_t index);
void p4_me_remove(ptl_handle_me_t me_h);
int alloc_buffer_init(void** ptr, size_t bytes);
#endif