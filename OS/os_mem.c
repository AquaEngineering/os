/**
 * @file os_mem.c
 * General and portable implementation of malloc and free.
 * The dynamic memory monitoring is also supported.
 */

/*********************
 *      INCLUDES
 *********************/
#include "os_mem.h"
#include "os_tlsf.h"

/*********************
 *      DEFINES
 *********************/
/*memset the allocated memories to 0xaa and freed memories to 0xbb (just for testing purposes)*/
#define ALIGN_MASK       0x3
#define ZERO_MEM_SENTINEL  0xa1b2c3d4

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
  static void os_mem_walker(void * ptr, size_t size, int used, void * user);


/**********************
 *  STATIC VARIABLES
 **********************/
  static os_tlsf_t tlsf;
  static uint32_t cur_used;
  static uint32_t max_used;


static uint32_t zero_mem = ZERO_MEM_SENTINEL; /*Give the address of this variable if 0 byte should be allocated*/

/**********************
 *      MACROS
 **********************/
#define COPY32 *d32 = *s32; d32++; s32++;
#define COPY8 *d8 = *s8; d8++; s8++;
#define SET32(x) *d32 = x; d32++;
#define SET8(x) *d8 = x; d8++;
#define REPEAT8(expr) expr expr expr expr expr expr expr expr

os_mem_buf_t os_mem_buf[OS_MEM_BUF_MAX_NUM];
/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Initialize the dyn_mem module (work memory and other variables)
 */
void os_mem_init(void)
{
  /*Allocate a large array to store the dynamically allocated data*/
  static uint32_t work_mem_int[OS_MEM_SIZE / sizeof(uint32_t)];
  tlsf = os_tlsf_create_with_pool((void *)work_mem_int, OS_MEM_SIZE);

}

/**
 * Clean up the memory buffer which frees all the allocated memories.
 * @note It work only if `LV_MEM_CUSTOM == 0`
 */
void lv_mem_deinit(void)
{
  os_tlsf_destroy(tlsf);
  os_mem_init();
}

/**
 * Allocate a memory dynamically
 * @param size size of the memory to allocate in bytes
 * @return pointer to the allocated memory
 */
void * os_mem_alloc(size_t size)
{
    if(size == 0) {
        return &zero_mem;
    }
    void * alloc = os_tlsf_malloc(tlsf, size);
    if(alloc == NULL) {

#if LV_LOG_LEVEL <= LV_LOG_LEVEL_INFO
        os_mem_monitor_t mon;
        os_mem_monitor(&mon);
#endif
    }
    if(alloc) {
        cur_used += size;
        max_used = OS_MAX(cur_used, max_used);
    }
    return alloc;
}

/**
 * Free an allocated data
 * @param data pointer to an allocated memory
 */
void os_mem_free(void * data)
{
    if(data == &zero_mem) return;
    if(data == NULL) return;
    size_t size = os_tlsf_free(tlsf, data);
    if(cur_used > size) cur_used -= size;
    else cur_used = 0;
}

/**
 * Reallocate a memory with a new size. The old content will be kept.
 * @param data pointer to an allocated memory.
 * Its content will be copied to the new memory block and freed
 * @param new_size the desired new size in byte
 * @return pointer to the new memory
 */
void * os_mem_realloc(void * data_p, size_t new_size)
{
    if(new_size == 0) {
    	os_mem_free(data_p);
        return &zero_mem;
    }

    if(data_p == &zero_mem) return os_mem_alloc(new_size);

    void * new_p = os_tlsf_realloc(tlsf, data_p, new_size);

    if(new_p == NULL) {
        return NULL;
    }

    return new_p;
}

os_res_t os_mem_test(void)
{
    if(zero_mem != ZERO_MEM_SENTINEL) {
        return OS_RES_INV;
    }
    if(os_tlsf_check(tlsf)) {
        return OS_RES_INV;
    }

    if(os_tlsf_check_pool(os_tlsf_get_pool(tlsf))) {
        return OS_RES_INV;
    }
    return OS_RES_OK;
}

/**
 * Give information about the work memory of dynamic allocation
 * @param mon_p pointer to a lv_mem_monitor_t variable,
 *              the result of the analysis will be stored here
 */
void os_mem_monitor(os_mem_monitor_t * mon_p)
{
    /*Init the data*/
    os_memset(mon_p, 0, sizeof(os_mem_monitor_t));
    os_tlsf_walk_pool(os_tlsf_get_pool(tlsf), os_mem_walker, mon_p);
    mon_p->total_size = OS_MEM_SIZE;
    mon_p->used_pct = 100 - (100U * mon_p->free_size) / mon_p->total_size;
    if(mon_p->free_size > 0) {
        mon_p->frag_pct = mon_p->free_biggest_size * 100U / mon_p->free_size;
        mon_p->frag_pct = 100 - mon_p->frag_pct;
    }
    else {
        mon_p->frag_pct = 0; /*no fragmentation if all the RAM is used*/
    }
    mon_p->max_used = max_used;
}


/**
 * Get a temporal buffer with the given size.
 * @param size the required size
 */
void * os_mem_buf_get(uint32_t size)
{
    if(size == 0) return NULL;

    /*Try to find a free buffer with suitable size*/
    int8_t i_guess = -1;
    for(uint8_t i = 0; i < OS_MEM_BUF_MAX_NUM; i++) {
        if(os_mem_buf[i].used == 0 && os_mem_buf[i].size >= size) {
            if(os_mem_buf[i].size == size) {
            	os_mem_buf[i].used = 1;
                return os_mem_buf[i].p;
            }
            else if(i_guess < 0) {
                i_guess = i;
            }
            /*If size of `i` is closer to `size` prefer it*/
            else if(os_mem_buf[i].size < os_mem_buf[i_guess].size) {
                i_guess = i;
            }
        }
    }

    if(i_guess >= 0) {
    	os_mem_buf[i_guess].used = 1;
        return os_mem_buf[i_guess].p;
    }

    /*Reallocate a free buffer*/
    for(uint8_t i = 0; i < OS_MEM_BUF_MAX_NUM; i++) {
        if(os_mem_buf[i].used == 0) {
            /*if this fails you probably need to increase your LV_MEM_SIZE/heap size*/
            void * buf = os_mem_realloc(os_mem_buf[i].p, size);
            if(buf == NULL) return NULL;

            os_mem_buf[i].used = 1;
            os_mem_buf[i].size = size;
            os_mem_buf[i].p    = buf;
            return os_mem_buf[i].p;
        }
    }
    return NULL;
}

/**
 * Release a memory buffer
 * @param p buffer to release
 */
void os_mem_buf_release(void * p)
{
  for(uint8_t i = 0; i < OS_MEM_BUF_MAX_NUM; i++) {
      if(os_mem_buf[i].p == p) {
    	  os_mem_buf[i].used = 0;
          return;
      }
  }
}

/**
 * Free all memory buffers
 */
void os_mem_buf_free_all(void)
{
  for(uint8_t i = 0; i < OS_MEM_BUF_MAX_NUM; i++) {
      if(os_mem_buf[i].p) {
    	  os_mem_free(os_mem_buf[i].p);
    	  os_mem_buf[i].p = NULL;
    	  os_mem_buf[i].used = 0;
    	  os_mem_buf[i].size = 0;
      }
  }
}

/**
 * Same as `memcpy` but optimized for 4 byte operation.
 * @param dst pointer to the destination buffer
 * @param src pointer to the source buffer
 * @param len number of byte to copy
 */
void* os_memcpy(void * dst, const void * src, size_t len)
{
    uint8_t * d8 = dst;
    const uint8_t * s8 = src;

    os_uintptr_t d_align = (os_uintptr_t)d8 & ALIGN_MASK;
    os_uintptr_t s_align = (os_uintptr_t)s8 & ALIGN_MASK;

    /*Byte copy for unaligned memories*/
    if(s_align != d_align) {
        while(len > 32) {
            REPEAT8(COPY8);
            REPEAT8(COPY8);
            REPEAT8(COPY8);
            REPEAT8(COPY8);
            len -= 32;
        }
        while(len) {
            COPY8
            len--;
        }
        return dst;
    }

    /*Make the memories aligned*/
    if(d_align) {
        d_align = ALIGN_MASK + 1 - d_align;
        while(d_align && len) {
            COPY8;
            d_align--;
            len--;
        }
    }

    uint32_t * d32 = (uint32_t *)d8;
    const uint32_t * s32 = (uint32_t *)s8;
    while(len > 32) {
        REPEAT8(COPY32)
        len -= 32;
    }

    while(len > 4) {
        COPY32;
        len -= 4;
    }

    d8 = (uint8_t *)d32;
    s8 = (const uint8_t *)s32;
    while(len) {
        COPY8
        len--;
    }

    return dst;
}

/**
 * Same as `memset` but optimized for 4 byte operation.
 * @param dst pointer to the destination buffer
 * @param v value to set [0..255]
 * @param len number of byte to set
 */
void os_memset(void * dst, uint8_t v, size_t len)
{
  uint8_t * d8 = (uint8_t *)dst;
  uintptr_t d_align = (os_uintptr_t) d8 & ALIGN_MASK;

    /*Make the address aligned*/
    if(d_align) {
        d_align = ALIGN_MASK + 1 - d_align;
        while(d_align && len) {
            SET8(v);
            len--;
            d_align--;
        }
    }

    uint32_t v32 = (uint32_t)v + ((uint32_t)v << 8) + ((uint32_t)v << 16) + ((uint32_t)v << 24);

    uint32_t * d32 = (uint32_t *)d8;

    while(len > 32) {
        REPEAT8(SET32(v32));
        len -= 32;
    }

    while(len > 4) {
        SET32(v32);
        len -= 4;
    }

    d8 = (uint8_t *)d32;
    while(len) {
        SET8(v);
        len--;
    }
}

/**
 * Same as `memset(dst, 0x00, len)` but optimized for 4 byte operation.
 * @param dst pointer to the destination buffer
 * @param len number of byte to set
 */
void os_memset_00(void * dst, size_t len)
{
    uint8_t * d8 = (uint8_t *)dst;
    uintptr_t d_align = (os_uintptr_t) d8 & ALIGN_MASK;

    /*Make the address aligned*/
    if(d_align) {
        d_align = ALIGN_MASK + 1 - d_align;
        while(d_align && len) {
            SET8(0);
            len--;
            d_align--;
        }
    }

    uint32_t * d32 = (uint32_t *)d8;
    while(len > 32) {
        REPEAT8(SET32(0));
        len -= 32;
    }

    while(len > 4) {
        SET32(0);
        len -= 4;
    }

    d8 = (uint8_t *)d32;
    while(len) {
        SET8(0);
        len--;
    }
}

/**
 * Same as `memset(dst, 0xFF, len)` but optimized for 4 byte operation.
 * @param dst pointer to the destination buffer
 * @param len number of byte to set
 */
void os_memset_ff(void * dst, size_t len)
{
    uint8_t * d8 = (uint8_t *)dst;
    uintptr_t d_align = (os_uintptr_t) d8 & ALIGN_MASK;

    /*Make the address aligned*/
    if(d_align) {
        d_align = ALIGN_MASK + 1 - d_align;
        while(d_align && len) {
            SET8(0xFF);
            len--;
            d_align--;
        }
    }

    uint32_t * d32 = (uint32_t *)d8;
    while(len > 32) {
        REPEAT8(SET32(0xFFFFFFFF));
        len -= 32;
    }

    while(len > 4) {
        SET32(0xFFFFFFFF);
        len -= 4;
    }

    d8 = (uint8_t *)d32;
    while(len) {
        SET8(0xFF);
        len--;
    }
}


/**********************
 *   STATIC FUNCTIONS
 **********************/

static void os_mem_walker(void * ptr, size_t size, int used, void * user)
{
    OS_UNUSED(ptr);

    os_mem_monitor_t * mon_p = user;
    if(used) {
        mon_p->used_cnt++;
    }
    else {
        mon_p->free_cnt++;
        mon_p->free_size += size;
        if(size > mon_p->free_biggest_size)
            mon_p->free_biggest_size = size;
    }
}

