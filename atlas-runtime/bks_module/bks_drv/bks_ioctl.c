#include <asm/io.h>
#include <asm/ioctl.h>
#include <asm/uaccess.h>
#include <bks_ioctl.h>
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
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/swap_stats.h>
#include <linux/swapops.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <rswap_rdma_custom_ops.h>

#include "bks.h"

static int bks_alloc_dma_page_ioctl(struct file *filp, void __user *args) {
    struct bks_alloc_dma_page alloc_dma_page_args;
    int ret;
    struct bks_dma_page *dma_page;
    struct bks_proc_ctx *proc_ctx = filp->private_data;
    unsigned long flags;
    spin_lock_irqsave(&bks_ctx_global.lock, flags);
    if (list_empty(&bks_ctx_global.dma_page_list)) {
        printk(KERN_ERR "bks: no more dma page\n");
        spin_unlock_irqrestore(&bks_ctx_global.lock, flags);
        return -ENOMEM;
    }

    dma_page = list_last_entry(&bks_ctx_global.dma_page_list,
                               struct bks_dma_page, list);
    list_del(&dma_page->list);
    alloc_dma_page_args.handle = (u64)dma_page;
    alloc_dma_page_args.pg_off = dma_page->id;

    ret = copy_to_user(args, &alloc_dma_page_args, sizeof(alloc_dma_page_args));
    if (unlikely(ret)) {
        printk(KERN_ERR "bks: %s fail to copy to user\n", __func__);
        list_add_tail(&dma_page->list, &bks_ctx_global.dma_page_list);
        spin_unlock_irqrestore(&bks_ctx_global.lock, flags);
        return -EFAULT;
    }

    list_add_tail(&dma_page->list, &proc_ctx->dma_page_list);
    spin_unlock_irqrestore(&bks_ctx_global.lock, flags);
    return 0;
}

/* Currently, we don't support cross-page objects and we require users to check
 * the boundary */
static int bks_fetch_object_impl(struct bks_fetch_object *args,
                                 void *__user user_args) {
    int ret;
    struct mm_struct *mm = current->mm;
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    pte_t pte_entry;
    swp_entry_t swp_entry;
    u64 query_addr = args->obj_page_addr;
    int cpu;
    struct bks_dma_page *dma_page;

    if (args->obj_offset + args->obj_size > PAGE_SIZE ||
        args->dst_offset + args->obj_size > PAGE_SIZE) {
        printk(KERN_ERR "bks: %s: not support cross-page object\n", __func__);
        return -EINVAL;
    }

    cpu = smp_processor_id();
    dma_page = (struct bks_dma_page *)args->handle;

#define FAIL_IF(cond)                                                          \
    do {                                                                       \
        if (cond) {                                                            \
            return -EFAULT;                                                    \
        }                                                                      \
    } while (0)

    pgd = pgd_offset(mm, query_addr);
    FAIL_IF(pgd_none(*pgd));

    p4d = p4d_offset(pgd, query_addr);
    FAIL_IF(p4d_none(*p4d));

    pud = pud_offset(p4d, query_addr);
    FAIL_IF(pud_none(*pud));

    pmd = pmd_offset(pud, query_addr);
    FAIL_IF(pmd_none(*pmd));

    pte = pte_offset_kernel(pmd, query_addr);
    pte_entry = *pte;
    FAIL_IF(pte_none(pte_entry));

    FAIL_IF(pte_present(pte_entry));
    swp_entry = pte_to_swp_entry(pte_entry);
    FAIL_IF(unlikely(non_swap_entry(swp_entry)));
    FAIL_IF(atlas_swap_check_and_duplicate(swp_entry));
    smp_mb();
    if (unlikely(pte_to_swp_entry(*pte).val != swp_entry.val))
        goto err;

    ret = rswap_atlas_pref_object(swp_offset(swp_entry), dma_page->dma_addr,
                                  args->dst_offset, args->obj_offset,
                                  args->obj_size, cpu, args->sync);
    if (ret) {
        goto err;
    }

    if (!args->sync) {
        args->queue_id = cpu;
        ret = copy_to_user(&((struct bks_fetch_object *)user_args)->queue_id,
                           &args->queue_id, sizeof(args->queue_id));
        if (ret) {
            printk(KERN_ERR "bks: %s fail to copy to user\n", __func__);
            return -EFAULT;
        }
    }

err:
    swap_free(swp_entry);
    return ret;
}

static int bks_fetch_object_ioctl(struct file *filp, void __user *args) {
    struct bks_fetch_object fetch_object_args;
    int ret;
    ret = copy_from_user(&fetch_object_args, args,
                         sizeof(struct bks_fetch_object));
    if (ret) {
        printk(KERN_ERR "bks: %s fail to copy from user\n", __func__);
        return -EFAULT;
    }
    ret = bks_fetch_object_impl(&fetch_object_args, args);
    return ret;
}

static int bks_fetch_sync_ioctl(struct file *filp, void __user *args) {
    struct bks_fetch_sync fetch_sync_args;
    int ret, cpu;
    ret = copy_from_user(&fetch_sync_args, args, sizeof(struct bks_fetch_sync));
    if (ret) {
        printk(KERN_ERR "bks: %s fail to copy from user\n", __func__);
        return -EFAULT;
    }
    cpu = fetch_sync_args.queue_id;
    rswap_atlas_sync(cpu);
    return 0;
}

static int bks_free_dma_page_ioctl(struct file *filp, void __user *args) {
    struct bks_free_dma_page free_args;
    int ret;
    struct bks_dma_page *dma_page;
    unsigned long flags;
    ret = copy_from_user(&free_args, args, sizeof(struct bks_free_dma_page));
    if (ret) {
        printk(KERN_ERR "bks: %s fail to copy from user\n", __func__);
        return -EFAULT;
    }
    dma_page = (struct bks_dma_page *)free_args.handle;
    spin_lock_irqsave(&bks_ctx_global.lock, flags);
    list_del(&dma_page->list);
    list_add_tail(&dma_page->list, &bks_ctx_global.dma_page_list);
    spin_unlock_irqrestore(&bks_ctx_global.lock, flags);
    return 0;
}

static int bks_reset_all_pages_ioctl(struct file *filp, void __user *args) {
    int i;
    unsigned long flags;
    spin_lock_irqsave(&bks_ctx_global.lock, flags);
    for (i = 0; i < BKS_MAX_DMA_PAGES; i++) {
        list_del(&bks_ctx_global.dma_page_array[i]->list);
    }
    INIT_LIST_HEAD(&bks_ctx_global.dma_page_list);
    for (i = 0; i < BKS_MAX_DMA_PAGES; i++) {
        list_add_tail(&bks_ctx_global.dma_page_array[i]->list,
                      &bks_ctx_global.dma_page_list);
    }
    spin_unlock_irqrestore(&bks_ctx_global.lock, flags);
    return 0;
}

static const struct bks_ioctl_desc bks_ioctls[] = {
    BKS_IOCTL_DEF_DRV(ALLOC_DMA_PAGE, bks_alloc_dma_page_ioctl),
    BKS_IOCTL_DEF_DRV(FETCH_OBJECT, bks_fetch_object_ioctl),
    BKS_IOCTL_DEF_DRV(FETCH_SYNC, bks_fetch_sync_ioctl),
    BKS_IOCTL_DEF_DRV(FREE_DMA_PAGE, bks_free_dma_page_ioctl),
    BKS_IOCTL_DEF_DRV(RESET_ALL_PAGES, bks_reset_all_pages_ioctl),
};

long bks_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    int nr = _IOC_NR(cmd) - BKS_COMMAND_BASE;

    if (_IOC_TYPE(cmd) != BKS_MAGIC) {
        printk(KERN_ERR "bks: invalid ioctl type: %c\n", _IOC_TYPE(cmd));
        return -ENOTTY;
    }

    if (nr >= ARRAY_SIZE(bks_ioctls)) {
        printk(KERN_ERR "bks: invalid ioctl cmd: %x\n", cmd);
        return -EINVAL;
    }

    return bks_ioctls[nr].func(filp, (void __user *)arg);
}