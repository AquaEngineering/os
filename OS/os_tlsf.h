#ifndef OS_TLSF_H
#define OS_TLSF_H

#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* os_tlsf_t: a TLSF structure. Can contain 1 to N pools. */
/* os_pool_t: a block of memory that TLSF can manage. */
typedef void * os_tlsf_t;
typedef void * os_pool_t;

/* Create/destroy a memory pool. */
os_tlsf_t os_tlsf_create(void * mem);
os_tlsf_t os_tlsf_create_with_pool(void * mem, size_t bytes);
void os_tlsf_destroy(os_tlsf_t tlsf);
os_pool_t os_tlsf_get_pool(os_tlsf_t tlsf);

/* Add/remove memory pools. */
os_pool_t os_tlsf_add_pool(os_tlsf_t tlsf, void * mem, size_t bytes);
void lv_tlsf_remove_pool(os_tlsf_t tlsf, os_pool_t pool);

/* malloc/memalign/realloc/free replacements. */
void * os_tlsf_malloc(os_tlsf_t tlsf, size_t bytes);
void * os_tlsf_memalign(os_tlsf_t tlsf, size_t align, size_t bytes);
void * os_tlsf_realloc(os_tlsf_t tlsf, void * ptr, size_t size);
size_t os_tlsf_free(os_tlsf_t tlsf, const void * ptr);

/* Returns internal block size, not original request size */
size_t os_tlsf_block_size(void * ptr);

/* Overheads/limits of internal structures. */
size_t os_tlsf_size(void);
size_t os_tlsf_align_size(void);
size_t os_tlsf_block_size_min(void);
size_t os_tlsf_block_size_max(void);
size_t os_tlsf_pool_overhead(void);
size_t os_tlsf_alloc_overhead(void);

/* Debugging. */
typedef void (*os_tlsf_walker)(void * ptr, size_t size, int used, void * user);
void os_tlsf_walk_pool(os_pool_t pool, os_tlsf_walker walker, void * user);
/* Returns nonzero if any internal consistency check fails. */
int os_tlsf_check(os_tlsf_t tlsf);
int os_tlsf_check_pool(os_pool_t pool);

#if defined(__cplusplus)
};
#endif

#endif /*OS_TLSF_H*/

