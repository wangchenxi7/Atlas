#pragma once
#include <asm/ioctl.h>
#include <asm/types.h>
#define BKS_MAGIC 'B'

#define BKS_COMMAND_BASE 0x0
#define BKS_ALLOC_DMA_PAGE 0x0
#define BKS_FETCH_OBJECT 0x1
#define BKS_FETCH_SYNC 0x2
#define BKS_FREE_DMA_PAGE 0x3
#define BKS_RESET_ALL_PAGES 0x4

#define BKS_IOCTL_ALLOC_DMA_PAGE                                               \
    _IOR(BKS_MAGIC, BKS_ALLOC_DMA_PAGE, struct bks_alloc_dma_page)

#define BKS_IOCTL_FETCH_OBJECT                                                 \
    _IOW(BKS_MAGIC, BKS_FETCH_OBJECT, struct bks_fetch_object)

#define BKS_IOCTL_FETCH_SYNC                                                   \
    _IOW(BKS_MAGIC, BKS_FETCH_SYNC, struct bks_fetch_sync)

#define BKS_IOCTL_FREE_DMA_PAGE                                                \
    _IOR(BKS_MAGIC, BKS_FREE_DMA_PAGE, struct bks_free_dma_page)

#define BKS_IOCTL_RESET_ALL_PAGES _IO(BKS_MAGIC, BKS_RESET_ALL_PAGES)

struct bks_alloc_dma_page {
    __u64 handle;
    __u32 pg_off;
    __u32 pad;
};

struct bks_fetch_object {
    __u64 obj_page_addr;
    __u64 handle;
    int obj_offset;
    int obj_size;
    int sync;
    int queue_id;
    int dst_offset;
    __u32 pad;
};

struct bks_fetch_sync {
    int queue_id;
    __u32 pad;
};

struct bks_free_dma_page {
    __u64 handle;
};