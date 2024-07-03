#include <asm/io.h>
#include <asm/ioctl.h>
#include <asm/uaccess.h>
#include <linux/cache.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <rswap_rdma_custom_ops.h>

#include "bks.h"

int bks_major = 0;
int bks_minor = 0;

#define DEVICE_NAME "bks_dev"
char *bks_name = DEVICE_NAME;
struct bks_dev_ctx bks_ctx_global;

struct file_operations bks_fops = {.owner = THIS_MODULE,
                                   .open = bks_open,
                                   .unlocked_ioctl = bks_ioctl,
                                   .release = bks_release,
                                   .mmap = bks_mmap};
static int bks_init(void);
static void bks_exit(void);

int bks_dma_page_init(struct bks_dma_page *dma_page, int id) {
    int ret = 0;
    dma_page->page = alloc_page(GFP_DMA);
    if (!dma_page->page) {
        printk(KERN_ERR "bks: fail to allocate dma page\n");
        return -ENOMEM;
    }
    dma_page->id = id;
    ret = rswap_atlas_page_map_dma(dma_page->page, &dma_page->dma_addr);
    if (ret) {
        printk(KERN_ERR "bks: fail to map dma pages\n");
    }
    return ret;
}

void bks_dma_page_destroy(struct bks_dma_page *dma_page) {
    rswap_atlas_page_unmap_dma(dma_page->dma_addr);
    __free_page(dma_page->page);
}

static int bks_init(void) {
    dev_t devno = 0;
    int error = 0;
    int i;
    struct bks_dma_page *node, *tmp;

    error = alloc_chrdev_region(&devno, bks_minor, 1, bks_name);
    if (error < 0) {
        printk(KERN_WARNING "bks: fail to register char device\n");
        goto fail;
    }

    cdev_init(&bks_ctx_global.cdev, &bks_fops);
    bks_ctx_global.cdev.owner = THIS_MODULE;
    bks_ctx_global.cdev.ops = &bks_fops;
    error = cdev_add(&bks_ctx_global.cdev, devno, 1);
    if (error < 0) {
        printk(KERN_ERR "bks: error %d adding char device", error);
        goto cdev_fail;
    }

    INIT_LIST_HEAD(&bks_ctx_global.dma_page_list);
    for (i = 0; i < BKS_MAX_DMA_PAGES; i++) {
        node = kmalloc(sizeof(struct bks_dma_page), GFP_KERNEL);
        error = bks_dma_page_init(node, i);
        if (error) {
            kfree(node);
            goto dma_page_fail;
        }
        bks_ctx_global.dma_page_array[i] = node;
        list_add_tail(&node->list, &bks_ctx_global.dma_page_list);
    }

    spin_lock_init(&bks_ctx_global.lock);
    printk(KERN_INFO "bks: module loaded\n");
    return 0;

dma_page_fail:
    if (!list_empty(&bks_ctx_global.dma_page_list)) {
        list_for_each_entry_safe(node, tmp, &bks_ctx_global.dma_page_list,
                                 list) {
            bks_dma_page_destroy(node);
            list_del(&node->list);
            kfree(node);
        }
    }
    cdev_del(&bks_ctx_global.cdev);
cdev_fail:
    unregister_chrdev_region(devno, 1);

fail:
    return error;
}

static void bks_exit(void) {
    dev_t devno = MKDEV(bks_major, bks_minor);
    int i;
    struct bks_dma_page *node;

    cdev_del(&bks_ctx_global.cdev);
    unregister_chrdev_region(devno, 1);

    for (i = 0; i < BKS_MAX_DMA_PAGES; i++) {
        node = bks_ctx_global.dma_page_array[i];
        bks_dma_page_destroy(node);
        kfree(node);
    }
    printk(KERN_INFO "bks: module unloaded\n");
}

int bks_open(struct inode *inode, struct file *filp) {
    struct bks_proc_ctx *ctx = kzalloc(sizeof(struct bks_proc_ctx), GFP_KERNEL);
    if (!ctx) {
        printk(KERN_ERR "bks: fail to allocate proc ctx\n");
        return -ENOMEM;
    }
    INIT_LIST_HEAD(&ctx->dma_page_list);
    filp->private_data = ctx;
    return 0;
}

int bks_release(struct inode *inode, struct file *filp) {
    struct bks_proc_ctx *ctx = filp->private_data;
    struct bks_dma_page *node, *tmp;
    unsigned long flags;
    spin_lock_irqsave(&bks_ctx_global.lock, flags);
    if (!list_empty(&ctx->dma_page_list)) {
        list_for_each_entry_safe(node, tmp, &ctx->dma_page_list, list) {
            list_del(&node->list);
            list_add_tail(&node->list, &bks_ctx_global.dma_page_list);
        }
    }
    spin_unlock_irqrestore(&bks_ctx_global.lock, flags);
    if (ctx->psf)
        vfree(ctx->psf);
    if (ctx->card)
        vfree(ctx->card);
    kfree(ctx);
    return 0;
}

int bks_mmap(struct file *file, struct vm_area_struct *vma) {
    int map_size;
    struct page *page;

    vma->vm_flags |= VM_IO;
    vma->vm_flags |= VM_NORESERVE;
    vma->vm_flags |= VM_MIXEDMAP;
    vma->vm_flags &= ~VM_PFNMAP;

    if (vma->vm_pgoff != 0 && vma->vm_pgoff != BKS_PSF_MMAP_PGOFF &&
        vma->vm_pgoff != BKS_CARD_MMAP_PGOFF) {
        printk(KERN_ERR "bks: mmap fail, invalid offset\n");
        return -EINVAL;
    }

    map_size = vma->vm_end - vma->vm_start;

    if (vma->vm_pgoff == BKS_CARD_MMAP_PGOFF) {
        struct bks_proc_ctx *ctx = file->private_data;
        struct bks_card *card = NULL;
        int ret;
        if (map_size != BKS_CARD_NUM * sizeof(struct bks_card)) {
            printk(KERN_ERR "bks: card mmap size mismatch, it should be %ld\n",
                   BKS_CARD_NUM * sizeof(struct bks_card));
            return -EINVAL;
        }
        if (ctx->card)
            goto out;
        card = vmalloc_user(map_size);
        if (!card) {
            printk(KERN_ERR "bks: fail to allocate card\n");
            return -ENOMEM;
        }
        memset(card, 0, map_size);
        ret = remap_vmalloc_range(vma, (void *)card, 0);
        if (unlikely(ret)) {
            printk(KERN_ERR "bks: fail to remap card\n");
            vfree(card);
            return ret;
        }
        vma->vm_mm->card = (unsigned int *)card;
        // printk("add card to vm_mm!\n");
        ctx->card = card;
    } else if (vma->vm_pgoff == BKS_PSF_MMAP_PGOFF) {
        struct bks_proc_ctx *ctx = file->private_data;
        struct bks_psf *psf = NULL;
        int ret;
        if (map_size != BKS_CARD_NUM) {
            printk(KERN_ERR "bks: psf mmap size mismatch, it should be %ld\n",
                   BKS_CARD_NUM);
            return -EINVAL;
        }
        if (ctx->psf)
            goto out;
        psf = vmalloc_user(map_size);
        if (!psf) {
            printk(KERN_ERR "bks: fail to allocate psf\n");
            return -ENOMEM;
        }
        memset(psf, 0, map_size);
        ret = remap_vmalloc_range(vma, (void *)psf, 0);
        if (unlikely(ret)) {
            printk(KERN_ERR "bks: fail to remap psf\n");
            vfree(psf);
            return ret;
        }
        vma->vm_mm->psf = (unsigned char *)psf;
        ctx->psf = psf;
        // vma->vm_mm->atlas_mm = true;
    } else {
        int i;
        unsigned long uaddr = vma->vm_start;
        if (map_size != BKS_MAX_DMA_PAGES * PAGE_SIZE) {
            printk(KERN_ERR "bks: mmap fail, only BKS_MAX_DMA_PAGES * "
                            "PAGE_SIZE is allowed\n");
            return -EINVAL;
        }

        for (i = 0; i < BKS_MAX_DMA_PAGES; ++i) {
            page = bks_ctx_global.dma_page_array[i]->page;
            if (remap_pfn_range(vma, uaddr, page_to_pfn(page), PAGE_SIZE,
                                vma->vm_page_prot))
                return -EFAULT;
            uaddr += PAGE_SIZE;
        }
    }
out:
    return 0;
}

module_init(bks_init);
module_exit(bks_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lei Chen <chenlei2014@mails.ucas.ac.cn>");
MODULE_DESCRIPTION("Barrier Kernel Support Module");
MODULE_VERSION("0.1");
