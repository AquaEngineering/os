#include <limits.h>
#include "os_tlsf.h"
#include "os_mem.h"

#define TLSF_MAX_POOL_SIZE OS_MEM_SIZE
#define TLSF_FLS(n) TLSF_FLS32(n)


#define TLSF_FLS32(n) ((n) & 0xffff0000 ? 16 + TLSF_FLS16((n) >> 16) : TLSF_FLS16(n))
#define TLSF_FLS16(n) ((n) & 0xff00     ?  8 + TLSF_FLS8 ((n) >>  8) : TLSF_FLS8 (n))
#define TLSF_FLS8(n)  ((n) & 0xf0       ?  4 + TLSF_FLS4 ((n) >>  4) : TLSF_FLS4 (n))
#define TLSF_FLS4(n)  ((n) & 0xc        ?  2 + TLSF_FLS2 ((n) >>  2) : TLSF_FLS2 (n))
#define TLSF_FLS2(n)  ((n) & 0x2        ?  1 + TLSF_FLS1 ((n) >>  1) : TLSF_FLS1 (n))
#define TLSF_FLS1(n)  ((n) & 0x1        ?  1 : 0)

/*
** Returns round up value of log2(n).
** Note: it is used at compile time.
*/
#define TLSF_LOG2_CEIL(n) ((n) & (n - 1) ? TLSF_FLS(n) : TLSF_FLS(n) - 1)

static int tlsf_ffs(unsigned int word)
{
    return __builtin_ffs(word) - 1;
}

static int tlsf_fls(unsigned int word)
{
    const int bit = word ? 32 - __builtin_clz(word) : 0;
    return bit - 1;
}

#define tlsf_fls_sizet tlsf_fls

/*
** Constants.
*/

/* Public constants: may be modified. */
enum tlsf_public {
    /* log2 of number of linear subdivisions of block sizes. Larger
    ** values require more memory in the control structure. Values of
    ** 4 or 5 are typical.
    */
    SL_INDEX_COUNT_LOG2 = 5,
};

/* Private constants: do not modify. */
enum tlsf_private {
    /* All allocation sizes and addresses are aligned to 4 bytes. */
    ALIGN_SIZE_LOG2 = 2,
    ALIGN_SIZE = (1 << ALIGN_SIZE_LOG2),

    /*
    ** We support allocations of sizes up to (1 << FL_INDEX_MAX) bits.
    ** However, because we linearly subdivide the second-level lists, and
    ** our minimum size granularity is 4 bytes, it doesn't make sense to
    ** create first-level lists for sizes smaller than SL_INDEX_COUNT * 4,
    ** or (1 << (SL_INDEX_COUNT_LOG2 + 2)) bytes, as there we will be
    ** trying to split size ranges into more slots than we have available.
    ** Instead, we calculate the minimum threshold size, and place all
    ** blocks below that size into the 0th first-level list.
    */
    FL_INDEX_MAX = TLSF_LOG2_CEIL(TLSF_MAX_POOL_SIZE),
    SL_INDEX_COUNT = (1 << SL_INDEX_COUNT_LOG2),
    FL_INDEX_SHIFT = (SL_INDEX_COUNT_LOG2 + ALIGN_SIZE_LOG2),
    FL_INDEX_COUNT = (FL_INDEX_MAX - FL_INDEX_SHIFT + 1),
    SMALL_BLOCK_SIZE = (1 << FL_INDEX_SHIFT),
};

/*
** Cast and min/max macros.
*/

#define tlsf_cast(t, exp)   ((t) (exp))
#define tlsf_min(a, b)      ((a) < (b) ? (a) : (b))
#define tlsf_max(a, b)      ((a) > (b) ? (a) : (b))


/*
** Static assertion mechanism.
*/

#define _tlsf_glue2(x, y) x ## y
#define _tlsf_glue(x, y) _tlsf_glue2(x, y)
#define tlsf_static_assert(exp) \
    typedef char _tlsf_glue(static_assert, __LINE__) [(exp) ? 1 : -1]

/* This code has been tested on 32- and 64-bit (LP/LLP) architectures. */
tlsf_static_assert(sizeof(int) * CHAR_BIT == 32);
tlsf_static_assert(sizeof(size_t) * CHAR_BIT >= 32);
tlsf_static_assert(sizeof(size_t) * CHAR_BIT <= 64);

/* SL_INDEX_COUNT must be <= number of bits in sl_bitmap's storage type. */
tlsf_static_assert(sizeof(unsigned int) * CHAR_BIT >= SL_INDEX_COUNT);

/* Ensure we've properly tuned our sizes. */
tlsf_static_assert(ALIGN_SIZE == SMALL_BLOCK_SIZE / SL_INDEX_COUNT);

/*
** Data structures and associated constants.
*/

/*
** Block header structure.
**
** There are several implementation subtleties involved:
** - The prev_phys_block field is only valid if the previous block is free.
** - The prev_phys_block field is actually stored at the end of the
**   previous block. It appears at the beginning of this structure only to
**   simplify the implementation.
** - The next_free / prev_free fields are only valid if the block is free.
*/
typedef struct block_header_t {
    /* Points to the previous physical block. */
    struct block_header_t * prev_phys_block;

    /* The size of this block, excluding the block header. */
    size_t size;

    /* Next and previous free blocks. */
    struct block_header_t * next_free;
    struct block_header_t * prev_free;
} block_header_t;

/*
** Since block sizes are always at least a multiple of 4, the two least
** significant bits of the size field are used to store the block status:
** - bit 0: whether block is busy or free
** - bit 1: whether previous block is busy or free
*/
static const size_t block_header_free_bit = 1 << 0;
static const size_t block_header_prev_free_bit = 1 << 1;

/*
** The size of the block header exposed to used blocks is the size field.
** The prev_phys_block field is stored *inside* the previous free block.
*/
static const size_t block_header_overhead = sizeof(size_t);

/* User data starts directly after the size field in a used block. */
static const size_t block_start_offset =
    offsetof(block_header_t, size) + sizeof(size_t);

/*
** A free block must be large enough to store its header minus the size of
** the prev_phys_block field, and no larger than the number of addressable
** bits for FL_INDEX.
*/
static const size_t block_size_min =
    sizeof(block_header_t) - sizeof(block_header_t *);
static const size_t block_size_max = tlsf_cast(size_t, 1) << FL_INDEX_MAX;


/* The TLSF control structure. */
typedef struct control_t {
    /* Empty lists point at this block to indicate they are free. */
    block_header_t block_null;

    /* Bitmaps for free lists. */
    unsigned int fl_bitmap;
    unsigned int sl_bitmap[FL_INDEX_COUNT];

    /* Head of free lists. */
    block_header_t * blocks[FL_INDEX_COUNT][SL_INDEX_COUNT];
} control_t;

/* A type used for casting when doing pointer arithmetic. */
typedef ptrdiff_t tlsfptr_t;

/*
** block_header_t member functions.
*/

static size_t block_size(const block_header_t * block)
{
    return block->size & ~(block_header_free_bit | block_header_prev_free_bit);
}

static void block_set_size(block_header_t * block, size_t size)
{
    const size_t oldsize = block->size;
    block->size = size | (oldsize & (block_header_free_bit | block_header_prev_free_bit));
}

static int block_is_last(const block_header_t * block)
{
    return block_size(block) == 0;
}

static int block_is_free(const block_header_t * block)
{
    return tlsf_cast(int, block->size & block_header_free_bit);
}

static void block_set_free(block_header_t * block)
{
    block->size |= block_header_free_bit;
}

static void block_set_used(block_header_t * block)
{
    block->size &= ~block_header_free_bit;
}

static int block_is_prev_free(const block_header_t * block)
{
    return tlsf_cast(int, block->size & block_header_prev_free_bit);
}

static void block_set_prev_free(block_header_t * block)
{
    block->size |= block_header_prev_free_bit;
}

static void block_set_prev_used(block_header_t * block)
{
    block->size &= ~block_header_prev_free_bit;
}

static block_header_t * block_from_ptr(const void * ptr)
{
    return tlsf_cast(block_header_t *,
                     tlsf_cast(unsigned char *, ptr) - block_start_offset);
}

static void * block_to_ptr(const block_header_t * block)
{
    return tlsf_cast(void *,
                     tlsf_cast(unsigned char *, block) + block_start_offset);
}

/* Return location of next block after block of given size. */
static block_header_t * offset_to_block(const void * ptr, size_t size)
{
    return tlsf_cast(block_header_t *, tlsf_cast(tlsfptr_t, ptr) + size);
}

/* Return location of previous block. */
static block_header_t * block_prev(const block_header_t * block)
{
    return block->prev_phys_block;
}

/* Return location of next existing block. */
static block_header_t * block_next(const block_header_t * block)
{
    block_header_t * next = offset_to_block(block_to_ptr(block),
                                            block_size(block) - block_header_overhead);
    return next;
}

/* Link a new block with its physical neighbor, return the neighbor. */
static block_header_t * block_link_next(block_header_t * block)
{
    block_header_t * next = block_next(block);
    next->prev_phys_block = block;
    return next;
}

static void block_mark_as_free(block_header_t * block)
{
    /* Link the block to the next block, first. */
    block_header_t * next = block_link_next(block);
    block_set_prev_free(next);
    block_set_free(block);
}

static void block_mark_as_used(block_header_t * block)
{
    block_header_t * next = block_next(block);
    block_set_prev_used(next);
    block_set_used(block);
}

static size_t align_up(size_t x, size_t align)
{
    return (x + (align - 1)) & ~(align - 1);
}

static size_t align_down(size_t x, size_t align)
{
    return x - (x & (align - 1));
}

static void * align_ptr(const void * ptr, size_t align)
{
    const tlsfptr_t aligned =
        (tlsf_cast(tlsfptr_t, ptr) + (align - 1)) & ~(align - 1);
    return tlsf_cast(void *, aligned);
}

/*
** Adjust an allocation size to be aligned to word size, and no smaller
** than internal minimum.
*/
static size_t adjust_request_size(size_t size, size_t align)
{
    size_t adjust = 0;
    if(size) {
        const size_t aligned = align_up(size, align);

        /* aligned sized must not exceed block_size_max or we'll go out of bounds on sl_bitmap */
        if(aligned < block_size_max) {
            adjust = tlsf_max(aligned, block_size_min);
        }
    }
    return adjust;
}

/*
** TLSF utility functions. In most cases, these are direct translations of
** the documentation found in the white paper.
*/

static void mapping_insert(size_t size, int * fli, int * sli)
{
    int fl, sl;
    if(size < SMALL_BLOCK_SIZE) {
        /* Store small blocks in first list. */
        fl = 0;
        sl = tlsf_cast(int, size) / (SMALL_BLOCK_SIZE / SL_INDEX_COUNT);
    }
    else {
        fl = tlsf_fls_sizet(size);
        sl = tlsf_cast(int, size >> (fl - SL_INDEX_COUNT_LOG2)) ^ (1 << SL_INDEX_COUNT_LOG2);
        fl -= (FL_INDEX_SHIFT - 1);
    }
    *fli = fl;
    *sli = sl;
}

/* This version rounds up to the next block size (for allocations) */
static void mapping_search(size_t size, int * fli, int * sli)
{
    if(size >= SMALL_BLOCK_SIZE) {
        const size_t round = (1 << (tlsf_fls_sizet(size) - SL_INDEX_COUNT_LOG2)) - 1;
        size += round;
    }
    mapping_insert(size, fli, sli);
}

static block_header_t * search_suitable_block(control_t * control, int * fli, int * sli)
{
    int fl = *fli;
    int sl = *sli;

    /*
    ** First, search for a block in the list associated with the given
    ** fl/sl index.
    */
    unsigned int sl_map = control->sl_bitmap[fl] & (~0U << sl);
    if(!sl_map) {
        /* No block exists. Search in the next largest first-level list. */
        const unsigned int fl_map = control->fl_bitmap & (~0U << (fl + 1));
        if(!fl_map) {
            /* No free blocks available, memory has been exhausted. */
            return 0;
        }

        fl = tlsf_ffs(fl_map);
        *fli = fl;
        sl_map = control->sl_bitmap[fl];
    }
    sl = tlsf_ffs(sl_map);
    *sli = sl;

    /* Return the first block in the free list. */
    return control->blocks[fl][sl];
}

/* Remove a free block from the free list.*/
static void remove_free_block(control_t * control, block_header_t * block, int fl, int sl)
{
    block_header_t * prev = block->prev_free;
    block_header_t * next = block->next_free;
    next->prev_free = prev;
    prev->next_free = next;

    /* If this block is the head of the free list, set new head. */
    if(control->blocks[fl][sl] == block) {
        control->blocks[fl][sl] = next;

        /* If the new head is null, clear the bitmap. */
        if(next == &control->block_null) {
            control->sl_bitmap[fl] &= ~(1U << sl);

            /* If the second bitmap is now empty, clear the fl bitmap. */
            if(!control->sl_bitmap[fl]) {
                control->fl_bitmap &= ~(1U << fl);
            }
        }
    }
}

/* Insert a free block into the free block list. */
static void insert_free_block(control_t * control, block_header_t * block, int fl, int sl)
{
    block_header_t * current = control->blocks[fl][sl];
    block->next_free = current;
    block->prev_free = &control->block_null;
    current->prev_free = block;

    /*
    ** Insert the new block at the head of the list, and mark the first-
    ** and second-level bitmaps appropriately.
    */
    control->blocks[fl][sl] = block;
    control->fl_bitmap |= (1U << fl);
    control->sl_bitmap[fl] |= (1U << sl);
}

/* Remove a given block from the free list. */
static void block_remove(control_t * control, block_header_t * block)
{
    int fl, sl;
    mapping_insert(block_size(block), &fl, &sl);
    remove_free_block(control, block, fl, sl);
}

/* Insert a given block into the free list. */
static void block_insert(control_t * control, block_header_t * block)
{
    int fl, sl;
    mapping_insert(block_size(block), &fl, &sl);
    insert_free_block(control, block, fl, sl);
}

static int block_can_split(block_header_t * block, size_t size)
{
    return block_size(block) >= sizeof(block_header_t) + size;
}

/* Split a block into two, the second of which is free. */
static block_header_t * block_split(block_header_t * block, size_t size)
{
    /* Calculate the amount of space left in the remaining block. */
    block_header_t * remaining =
        offset_to_block(block_to_ptr(block), size - block_header_overhead);

    block_set_size(block, size);
    block_mark_as_free(remaining);

    return remaining;
}

/* Absorb a free block's storage into an adjacent previous free block. */
static block_header_t * block_absorb(block_header_t * prev, block_header_t * block)
{
    /* Note: Leaves flags untouched. */
    prev->size += block_size(block) + block_header_overhead;
    block_link_next(prev);
    return prev;
}

/* Merge a just-freed block with an adjacent previous free block. */
static block_header_t * block_merge_prev(control_t * control, block_header_t * block)
{
    if(block_is_prev_free(block)) {
        block_header_t * prev = block_prev(block);
        block_remove(control, prev);
        block = block_absorb(prev, block);
    }

    return block;
}

/* Merge a just-freed block with an adjacent free block. */
static block_header_t * block_merge_next(control_t * control, block_header_t * block)
{
    block_header_t * next = block_next(block);
    if(block_is_free(next)) {
        block_remove(control, next);
        block = block_absorb(block, next);
    }
    return block;
}

/* Trim any trailing block space off the end of a block, return to pool. */
static void block_trim_free(control_t * control, block_header_t * block, size_t size)
{
    if(block_can_split(block, size)) {
        block_header_t * remaining_block = block_split(block, size);
        block_link_next(block);
        block_set_prev_free(remaining_block);
        block_insert(control, remaining_block);
    }
}

/* Trim any trailing block space off the end of a used block, return to pool. */
static void block_trim_used(control_t * control, block_header_t * block, size_t size)
{
    if(block_can_split(block, size)) {
        /* If the next block is free, we must coalesce. */
        block_header_t * remaining_block = block_split(block, size);
        block_set_prev_used(remaining_block);

        remaining_block = block_merge_next(control, remaining_block);
        block_insert(control, remaining_block);
    }
}

static block_header_t * block_trim_free_leading(control_t * control, block_header_t * block, size_t size)
{
    block_header_t * remaining_block = block;
    if(block_can_split(block, size)) {
        /* We want the 2nd block. */
        remaining_block = block_split(block, size - block_header_overhead);
        block_set_prev_free(remaining_block);

        block_link_next(block);
        block_insert(control, block);
    }

    return remaining_block;
}

static block_header_t * block_locate_free(control_t * control, size_t size)
{
    int fl = 0, sl = 0;
    block_header_t * block = 0;

    if(size) {
        mapping_search(size, &fl, &sl);

        /*
        ** mapping_search can futz with the size, so for excessively large sizes it can sometimes wind up
        ** with indices that are off the end of the block array.
        ** So, we protect against that here, since this is the only callsite of mapping_search.
        ** Note that we don't need to check sl, since it comes from a modulo operation that guarantees it's always in range.
        */
        if(fl < FL_INDEX_COUNT) {
            block = search_suitable_block(control, &fl, &sl);
        }
    }

    if(block) {
        remove_free_block(control, block, fl, sl);
    }

    return block;
}

static void * block_prepare_used(control_t * control, block_header_t * block, size_t size)
{
    void * p = 0;
    if(block) {
        block_trim_free(control, block, size);
        block_mark_as_used(block);
        p = block_to_ptr(block);
    }
    return p;
}

/* Clear structure and point all empty lists at the null block. */
static void control_constructor(control_t * control)
{
    int i, j;

    control->block_null.next_free = &control->block_null;
    control->block_null.prev_free = &control->block_null;

    control->fl_bitmap = 0;
    for(i = 0; i < FL_INDEX_COUNT; ++i) {
        control->sl_bitmap[i] = 0;
        for(j = 0; j < SL_INDEX_COUNT; ++j) {
            control->blocks[i][j] = &control->block_null;
        }
    }
}

/*
** Debugging utilities.
*/

typedef struct integrity_t {
    int prev_status;
    int status;
} integrity_t;


static void integrity_walker(void * ptr, size_t size, int used, void * user)
{
    block_header_t * block = block_from_ptr(ptr);
    integrity_t * integ = tlsf_cast(integrity_t *, user);

    const int this_status = block_is_free(block) ? 1 : 0;

    int status = 0;
    OS_UNUSED(used);

    integ->prev_status = this_status;
    integ->status += status;
}

int os_tlsf_check(os_tlsf_t tlsf)
{
    int i, j;

    control_t * control = tlsf_cast(control_t *, tlsf);
    int status = 0;

    /* Check that the free lists and bitmaps are accurate. */
    for(i = 0; i < FL_INDEX_COUNT; ++i) {
        for(j = 0; j < SL_INDEX_COUNT; ++j) {
            const block_header_t * block = control->blocks[i][j];

            /* Check that there is at least one free block. */

            while(block != &control->block_null) {
                int fli, sli;
                mapping_insert(block_size(block), &fli, &sli);
                block = block->next_free;
            }
        }
    }

    return status;
}

#undef tlsf_insist

static void default_walker(void * ptr, size_t size, int used, void * user)
{
    OS_UNUSED(user);
}

void os_tlsf_walk_pool(os_pool_t pool, os_tlsf_walker walker, void * user)
{
	os_tlsf_walker pool_walker = walker ? walker : default_walker;
    block_header_t * block =
        offset_to_block(pool, -(int)block_header_overhead);

    while(block && !block_is_last(block)) {
        pool_walker(
            block_to_ptr(block),
            block_size(block),
            !block_is_free(block),
            user);
        block = block_next(block);
    }
}

size_t os_tlsf_block_size(void * ptr)
{
    size_t size = 0;
    if(ptr) {
        const block_header_t * block = block_from_ptr(ptr);
        size = block_size(block);
    }
    return size;
}

int os_tlsf_check_pool(os_pool_t pool)
{
    /* Check that the blocks are physically correct. */
    integrity_t integ = { 0, 0 };
    os_tlsf_walk_pool(pool, integrity_walker, &integ);

    return integ.status;
}

/*
** Size of the TLSF structures in a given memory block passed to
** os_tlsf_create, equal to the size of a control_t
*/
size_t os_tlsf_size(void)
{
    return sizeof(control_t);
}

size_t os_tlsf_align_size(void)
{
    return ALIGN_SIZE;
}

size_t os_tlsf_block_size_min(void)
{
    return block_size_min;
}

size_t os_tlsf_block_size_max(void)
{
    return block_size_max;
}

/*
** Overhead of the TLSF structures in a given memory block passed to
** os_tlsf_add_pool, equal to the overhead of a free block and the
** sentinel block.
*/
size_t os_tlsf_pool_overhead(void)
{
    return 2 * block_header_overhead;
}

size_t os_tlsf_alloc_overhead(void)
{
    return block_header_overhead;
}

os_pool_t os_tlsf_add_pool(os_tlsf_t tlsf, void * mem, size_t bytes)
{
    block_header_t * block;
    block_header_t * next;

    const size_t pool_overhead = os_tlsf_pool_overhead();
    const size_t pool_bytes = align_down(bytes - pool_overhead, ALIGN_SIZE);

    if(((ptrdiff_t)mem % ALIGN_SIZE) != 0) {
        return 0;
    }

    if(pool_bytes < block_size_min || pool_bytes > block_size_max) {
        return 0;
    }

    /*
    ** Create the main free block. Offset the start of the block slightly
    ** so that the prev_phys_block field falls outside of the pool -
    ** it will never be used.
    */
    block = offset_to_block(mem, -(tlsfptr_t)block_header_overhead);
    block_set_size(block, pool_bytes);
    block_set_free(block);
    block_set_prev_used(block);
    block_insert(tlsf_cast(control_t *, tlsf), block);

    /* Split the block to create a zero-size sentinel block. */
    next = block_link_next(block);
    block_set_size(next, 0);
    block_set_used(next);
    block_set_prev_free(next);

    return mem;
}

void os_tlsf_remove_pool(os_tlsf_t tlsf, os_pool_t pool)
{
    control_t * control = tlsf_cast(control_t *, tlsf);
    block_header_t * block = offset_to_block(pool, -(int)block_header_overhead);

    int fl = 0, sl = 0;

    mapping_insert(block_size(block), &fl, &sl);
    remove_free_block(control, block, fl, sl);
}


os_tlsf_t os_tlsf_create(void * mem)
{

    if(((tlsfptr_t)mem % ALIGN_SIZE) != 0) {
        return 0;
    }

    control_constructor(tlsf_cast(control_t *, mem));

    return tlsf_cast(os_tlsf_t, mem);
}

os_tlsf_t os_tlsf_create_with_pool(void * mem, size_t bytes)
{
	os_tlsf_t tlsf = os_tlsf_create(mem);
    os_tlsf_add_pool(tlsf, (char *)mem + os_tlsf_size(), bytes - os_tlsf_size());
    return tlsf;
}

void os_tlsf_destroy(os_tlsf_t tlsf)
{
    /* Nothing to do. */
    OS_UNUSED(tlsf);
}

os_pool_t os_tlsf_get_pool(os_tlsf_t tlsf)
{
    return tlsf_cast(os_pool_t, (char *)tlsf + os_tlsf_size());
}

void * os_tlsf_malloc(os_tlsf_t tlsf, size_t size)
{
    control_t * control = tlsf_cast(control_t *, tlsf);
    const size_t adjust = adjust_request_size(size, ALIGN_SIZE);
    block_header_t * block = block_locate_free(control, adjust);
    return block_prepare_used(control, block, adjust);
}

void * os_tlsf_memalign(os_tlsf_t tlsf, size_t align, size_t size)
{
    control_t * control = tlsf_cast(control_t *, tlsf);
    const size_t adjust = adjust_request_size(size, ALIGN_SIZE);

    /*
    ** We must allocate an additional minimum block size bytes so that if
    ** our free block will leave an alignment gap which is smaller, we can
    ** trim a leading free block and release it back to the pool. We must
    ** do this because the previous physical block is in use, therefore
    ** the prev_phys_block field is not valid, and we can't simply adjust
    ** the size of that block.
    */
    const size_t gap_minimum = sizeof(block_header_t);
    const size_t size_with_gap = adjust_request_size(adjust + align + gap_minimum, align);

    /*
    ** If alignment is less than or equals base alignment, we're done.
    ** If we requested 0 bytes, return null, as os_tlsf_malloc(0) does.
    */
    const size_t aligned_size = (adjust && align > ALIGN_SIZE) ? size_with_gap : adjust;

    block_header_t * block = block_locate_free(control, aligned_size);

    /* This can't be a static assert. */

    if(block) {
        void * ptr = block_to_ptr(block);
        void * aligned = align_ptr(ptr, align);
        size_t gap = tlsf_cast(size_t,
                               tlsf_cast(tlsfptr_t, aligned) - tlsf_cast(tlsfptr_t, ptr));

        /* If gap size is too small, offset to next aligned boundary. */
        if(gap && gap < gap_minimum) {
            const size_t gap_remain = gap_minimum - gap;
            const size_t offset = tlsf_max(gap_remain, align);
            const void * next_aligned = tlsf_cast(void *,
                                                  tlsf_cast(tlsfptr_t, aligned) + offset);

            aligned = align_ptr(next_aligned, align);
            gap = tlsf_cast(size_t,
                            tlsf_cast(tlsfptr_t, aligned) - tlsf_cast(tlsfptr_t, ptr));
        }

        if(gap) {
            block = block_trim_free_leading(control, block, gap);
        }
    }

    return block_prepare_used(control, block, adjust);
}

size_t os_tlsf_free(os_tlsf_t tlsf, const void * ptr)
{
    size_t size = 0;
    /* Don't attempt to free a NULL pointer. */
    if(ptr) {
        control_t * control = tlsf_cast(control_t *, tlsf);
        block_header_t * block = block_from_ptr(ptr);
        size = block->size;
        block_mark_as_free(block);
        block = block_merge_prev(control, block);
        block = block_merge_next(control, block);
        block_insert(control, block);
    }

    return size;
}

/*
** The TLSF block information provides us with enough information to
** provide a reasonably intelligent implementation of realloc, growing or
** shrinking the currently allocated block as required.
**
** This routine handles the somewhat esoteric edge cases of realloc:
** - a non-zero size with a null pointer will behave like malloc
** - a zero size with a non-null pointer will behave like free
** - a request that cannot be satisfied will leave the original buffer
**   untouched
** - an extended buffer size will leave the newly-allocated area with
**   contents undefined
*/
void * os_tlsf_realloc(os_tlsf_t tlsf, void * ptr, size_t size)
{
    control_t * control = tlsf_cast(control_t *, tlsf);
    void * p = 0;

    /* Zero-size requests are treated as free. */
    if(ptr && size == 0) {
    	os_tlsf_free(tlsf, ptr);
    }
    /* Requests with NULL pointers are treated as malloc. */
    else if(!ptr) {
        p = os_tlsf_malloc(tlsf, size);
    }
    else {
        block_header_t * block = block_from_ptr(ptr);
        block_header_t * next = block_next(block);

        const size_t cursize = block_size(block);
        const size_t combined = cursize + block_size(next) + block_header_overhead;
        const size_t adjust = adjust_request_size(size, ALIGN_SIZE);
        if(size > cursize && adjust == 0) {
            /* The request is probably too large, fail */
            return NULL;
        }

        /*
        ** If the next block is used, or when combined with the current
        ** block, does not offer enough space, we must reallocate and copy.
        */
        if(adjust > cursize && (!block_is_free(next) || adjust > combined)) {
            p = os_tlsf_malloc(tlsf, size);
            if(p) {
                const size_t minsize = tlsf_min(cursize, size);
                os_memcpy(p, ptr, minsize);
                os_tlsf_free(tlsf, ptr);
            }
        }
        else {
            /* Do we need to expand to the next block? */
            if(adjust > cursize) {
                block_merge_next(control, block);
                block_mark_as_used(block);
            }

            /* Trim the resulting block and return the original pointer. */
            block_trim_used(control, block, adjust);
            p = ptr;
        }
    }

    return p;
}

