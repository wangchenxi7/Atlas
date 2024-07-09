#pragma once
#include <runtime.h>
#include <pointer_shim.h>
#include <bks_types.h>
#include <string.h>
#include <stdlib.h>
#include "memcached.h"

extern void *global_psf;

static void inline atlas_init()
{
    runtime_init();
    /* set the psf to paging-in manually */
    memset(global_psf, 0x0, BKS_CARD_NUM);
}

static void inline atlas_enable_runtime_fetch()
{
    memset(global_psf, 0xf, BKS_CARD_NUM);
}

static void inline atlas_disable_runtime_fetch()
{
    memset(global_psf, 0x0, BKS_CARD_NUM);
}

/* [Atlas] as the orig object ptr is 0x7fxxxxx, the most siginificant bit of our unique ptr metadata must be 1 */
static inline bool is_atlas_ptr(void *ptr)
{
    return (uintptr_t)ptr >> 63;
}

#define BARRIER_ASSERT(x)                                                     \
    do                                                                        \
    {                                                                         \
        if (!(x))                                                             \
        {                                                                     \
            fprintf(stderr, "[%s:%d] Assertion failed:", __FILE__, __LINE__); \
            fputs(#x "\n", stderr);                                           \
            abort();                                                          \
        }                                                                     \
    } while (0)

static inline item * alloc_item(item* ptr){
    item* ret = malloc(ITEM_ntotal(ptr));
    memcpy(ret, ptr, ITEM_ntotal(ptr));
    return ret;
}

static inline item* atlas_get_item(item** item_ptr, atlas_unique_ptr **up, bool force_paging){
    item* ret = *item_ptr;
    if(is_atlas_ptr(ret)){
        *up = (atlas_unique_ptr*)item_ptr;
        ret = atlas_up_deref_get(*up);
        // ret = (item*)((*up)->handle >> 17);
    }else{
        *up = NULL;
    }
    return ret;
}

/* [Atlas] this should be called when holding hv lock, if the item is in the hashtable */
static inline void atlas_put_item(atlas_unique_ptr *up, void *ptr){
    const uint64_t kAddrShift = 17; /* a hack to extract the addr from metadata */
    if(up){
        BARRIER_ASSERT((up->handle >> kAddrShift) == (uint64_t)ptr);
        atlas_up_deref_put(up, ptr);
    }
}
