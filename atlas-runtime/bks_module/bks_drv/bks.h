#ifndef __BKS_H__
#define __BKS_H__
#include "bks_types.h"

struct bks_dma_page {
    struct page *page;
    u64 dma_addr;
    int id;
    struct list_head list;
};

struct bks_dev_ctx {
    struct cdev cdev;
    struct list_head dma_page_list;
    spinlock_t lock;
    struct bks_dma_page *dma_page_array[BKS_MAX_DMA_PAGES];
};

struct bks_proc_ctx {
    struct list_head dma_page_list;
    struct bks_psf *psf;
    struct bks_card *card;
};

extern struct bks_dev_ctx bks_ctx_global;

int bks_dma_page_init(struct bks_dma_page *dma_page, int id);
void bks_dma_page_destroy(struct bks_dma_page *dma_page);

int bks_open(struct inode *inode, struct file *filp);
int bks_release(struct inode *inode, struct file *filp);
long bks_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
int bks_mmap(struct file *file, struct vm_area_struct *vma);

/**
 * BKS_IOCTL_DEF_DRV() - helper macro to fill out a &struct bks_ioctl_desc
 * @ioctl: ioctl command suffix
 * @_func: handler for the ioctl
 *
 */
#define BKS_IOCTL_DEF_DRV(ioctl, _func)                                        \
    [_IOC_NR(BKS_IOCTL_##ioctl) - BKS_COMMAND_BASE] = {                        \
        .cmd = BKS_IOCTL_##ioctl,                                              \
        .func = _func,                                                         \
        .name = #ioctl,                                                        \
    }

typedef int bks_ioctl_t(struct file *filp, void __user *args);

struct bks_ioctl_desc {
    unsigned int cmd;
    bks_ioctl_t *func;
    const char *name;
};

#endif
