#include "rswap_rdma_custom_ops.h"
#include "rswap_rdma.h"
#include <asm/pgtable_types.h>
#include <linux/mm.h>
#include <linux/swapops.h>
#include <linux/hermit_utils.h>
#include <linux/hermit.h>

#define SWAP_RA_WIN_SHIFT (PAGE_SHIFT / 2)
#define SWAP_RA_HITS_MASK ((1UL << SWAP_RA_WIN_SHIFT) - 1)
#define SWAP_RA_HITS_MAX SWAP_RA_HITS_MASK
#define SWAP_RA_WIN_MASK (~PAGE_MASK & ~SWAP_RA_HITS_MASK)

#define SWAP_RA_HITS(v) ((v)&SWAP_RA_HITS_MASK)
#define SWAP_RA_WIN(v) (((v)&SWAP_RA_WIN_MASK) >> SWAP_RA_WIN_SHIFT)
#define SWAP_RA_ADDR(v) ((v)&PAGE_MASK)

#define SWAP_RA_VAL(addr, win, hits)                                           \
    (((addr)&PAGE_MASK) | (((win) << SWAP_RA_WIN_SHIFT) & SWAP_RA_WIN_MASK) |  \
     ((hits)&SWAP_RA_HITS_MASK))

/* Initial readahead hits is 4 to start up with a small window */
#define GET_SWAP_RA_VAL(vma)                                                   \
    (atomic_long_read(&(vma)->swap_readahead_info) ?: 4)

void fs_rdma_early_map_update_ra(struct vm_area_struct *vma, u64 addr) {
    unsigned long ra_val;
    int win, hits;

    ra_val = GET_SWAP_RA_VAL(vma);
    win = SWAP_RA_WIN(ra_val);
    hits = SWAP_RA_HITS(ra_val);
    hits = min_t(int, hits + 1, SWAP_RA_HITS_MAX);
    atomic_long_set(&vma->swap_readahead_info, SWAP_RA_VAL(addr, win, hits));
}

static void do_map_directly_work(struct work_struct * work){
    struct fs_rdma_early_map_req *rdma_req =
        container_of(work, struct fs_rdma_early_map_req, work);
    struct rswap_rdma_queue *rdma_queue = rdma_req->rdma_queue;
    unsigned long addr = rdma_req->virtual_address;
    struct vm_area_struct *vma = rdma_req->vma;
    hermit_faultin_page(vma, addr, rdma_req->page, rdma_req->ptep, rdma_req->orig_pte);
    kmem_cache_free(rdma_queue->fs_rdma_early_map_req_cache,
                    rdma_req); // safe to free
}

void fs_rdma_early_map_done(struct ib_cq *cq, struct ib_wc *wc) {
    struct rswap_rdma_queue *rdma_queue = cq->cq_context;

    struct fs_rdma_early_map_req *rdma_req =
        container_of(wc->wr_cqe, struct fs_rdma_early_map_req, cqe);

    struct ib_device *ibdev = rdma_queue->rdma_session->rdma_dev->dev;
    struct vm_area_struct *vma = rdma_req->vma;
    struct mm_struct *mm = vma->vm_mm;
    struct page *page = rdma_req->page;
    unsigned long addr = rdma_req->virtual_address;
    struct work_struct* work = &rdma_req->work;
    ib_dma_unmap_page(ibdev, rdma_req->dma_addr, PAGE_SIZE, DMA_FROM_DEVICE);
    SetPageUptodate(page); // [?] will be invoked in swap_readpage().
                           // no need to do it here ?

    unlock_page(page);
    put_page(page);

    if (unlikely(wc->status != IB_WC_SUCCESS)) {
        pr_err("%s status is not success, it is=%d\n", __func__, wc->status);
        goto out;
    }

    if(addr < vma->vm_start || addr >= vma->vm_end)
        goto out;

    INIT_WORK(work, do_map_directly_work);
    queue_work(hermit_ub_wq, work);

    mmdrop(mm);
    atomic_sub(1, &rdma_queue->rdma_post_counter); // decrease outstanding rdma
                                                // request counter
    return;

out:
    mmdrop(mm);
    atomic_sub(1, &rdma_queue->rdma_post_counter); // decrease outstanding rdma
                                                   // request counter
    kmem_cache_free(rdma_queue->fs_rdma_early_map_req_cache,
                    rdma_req); // safe to free
}

int fs_post_rdma_early_map_wr(struct rdma_session_context *rdma_session,
                            struct rswap_rdma_queue *rdma_queue,
                            struct fs_rdma_early_map_req *rdma_early_map_req,
                            struct remote_chunk *remote_chunk_ptr,
                            size_t offset_within_chunk, struct page *page,
                            u64 virtual_address, struct vm_area_struct *vma,
                            pte_t *ptep, pte_t orig_pte) {

    int ret;
    int test;
    struct ib_sge sge;
    struct ib_rdma_wr rdma_wr;
    const struct ib_send_wr *bad_wr;
    struct ib_device *dev = rdma_session->rdma_dev->dev;

    rdma_early_map_req->cqe.done = fs_rdma_early_map_done;
    rdma_early_map_req->page = page;

    rdma_early_map_req->dma_addr =
        ib_dma_map_page(dev, page, 0, PAGE_SIZE, DMA_FROM_DEVICE);

    if (unlikely(ib_dma_mapping_error(dev, rdma_early_map_req->dma_addr))) {
        pr_err("%s, ib_dma_mapping_error\n", __func__);
        ret = -ENOMEM;
        goto out;
    }

    // Map the dma address to IB deivce.
    ib_dma_sync_single_for_device(dev, rdma_early_map_req->dma_addr, PAGE_SIZE,
                                  DMA_FROM_DEVICE);

    rdma_early_map_req->rdma_queue = rdma_queue;
    rdma_early_map_req->vma = vma;
    rdma_early_map_req->virtual_address = virtual_address;
    rdma_early_map_req->ptep = ptep;
    rdma_early_map_req->orig_pte = orig_pte;

    // 2) Initialize the rdma_wr
    // 2.1 local addr
    sge.addr = rdma_early_map_req->dma_addr;
    sge.length = PAGE_SIZE;
    sge.lkey = rdma_session->rdma_dev->pd->local_dma_lkey;

    // 2.2 initia rdma_wr for remote addr
    rdma_wr.wr.next = NULL;
    // assing completion handler. prepare for container_of()
    rdma_wr.wr.wr_cqe = &rdma_early_map_req->cqe;
    // (i == num_pages - 1) ? &rdma_early_map_req->cqe : NULL;
    rdma_wr.wr.sg_list = &sge;
    rdma_wr.wr.num_sge = 1;
    rdma_wr.wr.opcode = IB_WR_RDMA_READ;
    // only the last one is signaled
    rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
    rdma_wr.remote_addr = remote_chunk_ptr->remote_addr + offset_within_chunk;
    rdma_wr.rkey = remote_chunk_ptr->remote_rkey;

    // Post 1-sided RDMA read wr
    // wait and enqueue wr
    // Both 1-sided read/write queue depth are RDMA_SEND_QUEUE_DEPTH
    while (1) {
        test = atomic_add_return(1, &rdma_queue->rdma_post_counter);
        if (test < RDMA_SEND_QUEUE_DEPTH - 16) {
            // post the 1-sided RDMA write
            // Use the global RDMA context, rdma_session_global
            ret = ib_post_send(rdma_queue->qp, (struct ib_send_wr *)&rdma_wr,
                               &bad_wr);
            if (unlikely(ret)) {
                printk(KERN_ERR "%s, post 1-sided RDMA send wr failed, "
                                "return value :%d. counter %d \n",
                       __func__, ret, test);
                ret = -1;
                goto out;
            }

            // Enqueue successfully.
            // exit loop.
            goto out;
        } else {
            // RDMA send queue is full, wait for next turn.
            test = atomic_sub_return(1, &rdma_queue->rdma_post_counter);
            // schedule(); // release the core for a while.
            // cpu_relax(); // which one is better ?

            // IB_DIRECT_CQ, poll cqe directly
            drain_rdma_queue(rdma_queue);
        }
    }

out:
    if (ret)
        put_page(page);
    return ret;
}

int rswap_frontswap_load_async_early_map(pgoff_t swp_entry_offset,
                                       struct page *page, u64 address,
                                       struct vm_area_struct *vma, pte_t *ptep,
                                       pte_t orig_pte) {
    int ret = 0;
    int cpu;
    size_t page_addr;
    size_t chunk_idx;
    size_t offset_within_chunk;
    struct rswap_rdma_queue *rdma_queue;
    struct fs_rdma_early_map_req *rdma_early_map_req;
    struct remote_chunk *remote_chunk_ptr;
    address = address & PAGE_MASK;

    cpu = smp_processor_id();

    page_addr = pgoff2addr(swp_entry_offset);
    chunk_idx = page_addr >> CHUNK_SHIFT;
    offset_within_chunk = page_addr & CHUNK_MASK;

    rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_LOAD_ASYNC);
    rdma_early_map_req = (struct fs_rdma_early_map_req *)kmem_cache_alloc(
        rdma_queue->fs_rdma_early_map_req_cache, GFP_ATOMIC);
    if (!rdma_early_map_req) {
        pr_err("%s, get reserved "
               "fs_rdma_req failed. \n",
               __func__);
        goto out;
    }

    remote_chunk_ptr = &(rdma_session_global.remote_mem_pool.chunks[chunk_idx]);
    get_page(page);
    mmgrab(vma->vm_mm);

    // initialize the rdma_req
    ret = fs_post_rdma_early_map_wr(
        &rdma_session_global, rdma_queue, rdma_early_map_req, remote_chunk_ptr,
        offset_within_chunk, page, address, vma, ptep, orig_pte);
    if (unlikely(ret)) {
        pr_err("%s, post rdma req failed.\n", __func__);
        kmem_cache_free(rdma_queue->fs_rdma_early_map_req_cache,
                        rdma_early_map_req);
        goto out;
    }

    ret = 0; // reset to 0 for succss.

out:
    return ret;
}

int rswap_frontswap_pref_async_early_map(pgoff_t swp_entry_offset,
                                       struct page *page, u64 address,
                                       struct vm_area_struct *vma, pte_t *ptep,
                                       pte_t orig_pte, int cpu) {
    int ret = 0;
    size_t page_addr;
    size_t chunk_idx;
    size_t offset_within_chunk;
    struct rswap_rdma_queue *rdma_queue;
    struct fs_rdma_early_map_req *rdma_early_map_req;
    struct remote_chunk *remote_chunk_ptr;
    address = address & PAGE_MASK;

    page_addr = pgoff2addr(swp_entry_offset);
    chunk_idx = page_addr >> CHUNK_SHIFT;
    offset_within_chunk = page_addr & CHUNK_MASK;

    rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_PREF_ASYNC);
    rdma_early_map_req = (struct fs_rdma_early_map_req *)kmem_cache_alloc(
        rdma_queue->fs_rdma_early_map_req_cache, GFP_ATOMIC);
    if (!rdma_early_map_req) {
        pr_err("%s, get reserved "
               "fs_rdma_req failed. \n",
               __func__);
        goto out;
    }

    remote_chunk_ptr = &(rdma_session_global.remote_mem_pool.chunks[chunk_idx]);
    get_page(page);
    mmgrab(vma->vm_mm);

    // initialize the rdma_req
    ret = fs_post_rdma_early_map_wr(
        &rdma_session_global, rdma_queue, rdma_early_map_req, remote_chunk_ptr,
        offset_within_chunk, page, address, vma, ptep, orig_pte);
    if (unlikely(ret)) {
        pr_err("%s, post rdma req failed.\n", __func__);
        kmem_cache_free(rdma_queue->fs_rdma_early_map_req_cache,
                        rdma_early_map_req);
        goto out;
    }

    ret = 0; // reset to 0 for succss.

out:
    return ret;
}

/* [Atlas] runtime's object-in datapath */
void fs_rdma_obj_done(struct ib_cq *cq, struct ib_wc *wc) {
    struct rswap_rdma_queue *rdma_queue = cq->cq_context;

    struct fs_rdma_obj_req *rdma_req =
        container_of(wc->wr_cqe, struct fs_rdma_obj_req, cqe);

    struct ib_device *ibdev = rdma_queue->rdma_session->rdma_dev->dev;

    if (unlikely(wc->status != IB_WC_SUCCESS)) {
        pr_err("%s status is not success, it is=%d\n", __func__, wc->status);
        goto out;
    }

    ib_dma_sync_single_for_cpu(ibdev, rdma_req->dma_addr, PAGE_SIZE, DMA_FROM_DEVICE);

out:
    atomic_sub(1, &rdma_queue->rdma_post_counter); // decrease outstanding rdma
                                                   // request counter
    kmem_cache_free(rdma_queue->fs_rdma_obj_req_cache,
                    rdma_req); // safe to free
    return;
}

int fs_post_rdma_obj_wr(struct rdma_session_context *rdma_session,
                            struct rswap_rdma_queue *rdma_queue,
                            struct fs_rdma_obj_req *rdma_obj_req,
                            struct remote_chunk *remote_chunk_ptr,
                            size_t offset_within_chunk, 
                            u64 dma_addr, int dma_off,
                            int obj_offset,
                            int obj_size) {

    int ret;
    int test;
    struct ib_sge sge;
    struct ib_rdma_wr rdma_wr;
    const struct ib_send_wr *bad_wr;

    rdma_obj_req->cqe.done = fs_rdma_obj_done;

    rdma_obj_req->dma_addr = dma_addr;

    rdma_obj_req->rdma_queue = rdma_queue;

    // 2) Initialize the rdma_wr
    // 2.1 local addr
    sge.addr = rdma_obj_req->dma_addr + dma_off;
    sge.length = obj_size;
    sge.lkey = rdma_session->rdma_dev->pd->local_dma_lkey;

    // 2.2 initia rdma_wr for remote addr
    rdma_wr.wr.next = NULL;
    // assing completion handler. prepare for container_of()
    rdma_wr.wr.wr_cqe = &rdma_obj_req->cqe;
    // (i == num_pages - 1) ? &rdma_obj_req->cqe : NULL;
    rdma_wr.wr.sg_list = &sge;
    rdma_wr.wr.num_sge = 1;
    rdma_wr.wr.opcode = IB_WR_RDMA_READ;
    // only the last one is signaled
    rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
    rdma_wr.remote_addr = remote_chunk_ptr->remote_addr + offset_within_chunk + obj_offset;
    rdma_wr.rkey = remote_chunk_ptr->remote_rkey;

    // Post 1-sided RDMA read wr
    // wait and enqueue wr
    // Both 1-sided read/write queue depth are RDMA_SEND_QUEUE_DEPTH
    while (1) {
        test = atomic_add_return(1, &rdma_queue->rdma_post_counter);
        if (test < RDMA_SEND_QUEUE_DEPTH - 16) {
            // post the 1-sided RDMA write
            // Use the global RDMA context, rdma_session_global
            ret = ib_post_send(rdma_queue->qp, (struct ib_send_wr *)&rdma_wr,
                               &bad_wr);
            if (unlikely(ret)) {
                printk(KERN_ERR "%s, post 1-sided RDMA send wr failed, "
                                "return value :%d. counter %d \n",
                       __func__, ret, test);
                ret = -1;
                goto out;
            }

            // Enqueue successfully.
            // exit loop.
            goto out;
        } else {
            // RDMA send queue is full, wait for next turn.
            test = atomic_sub_return(1, &rdma_queue->rdma_post_counter);
            // schedule(); // release the core for a while.
            // cpu_relax(); // which one is better ?

            // IB_DIRECT_CQ, poll cqe directly
            drain_rdma_queue(rdma_queue);
        }
    }

out:
    return ret;
}


int rswap_atlas_pref_object(pgoff_t swp_entry_offset,
                                u64 dma_addr, int dma_off,
                                int obj_off, int obj_size, int cpu, bool sync) {
    int ret = 0;
    size_t page_addr;
    size_t chunk_idx;
    size_t offset_within_chunk;
    struct rswap_rdma_queue *rdma_queue;
    struct fs_rdma_obj_req *rdma_obj_req;
    struct remote_chunk *remote_chunk_ptr;

    page_addr = pgoff2addr(swp_entry_offset);
    chunk_idx = page_addr >> CHUNK_SHIFT;
    offset_within_chunk = page_addr & CHUNK_MASK;

    rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_PREF_ASYNC);
    rdma_obj_req = (struct fs_rdma_obj_req *)kmem_cache_alloc(
        rdma_queue->fs_rdma_obj_req_cache, GFP_ATOMIC);
    if (!rdma_obj_req) {
        pr_err("%s, get reserved "
               "fs_rdma_obj_req failed. \n",
               __func__);
        goto out;
    }

    remote_chunk_ptr = &(rdma_session_global.remote_mem_pool.chunks[chunk_idx]);

    // initialize the rdma_req
    ret = fs_post_rdma_obj_wr(
        &rdma_session_global, rdma_queue, rdma_obj_req, remote_chunk_ptr,
        offset_within_chunk, dma_addr, dma_off, obj_off, obj_size);
    if (unlikely(ret)) {
        pr_err("%s, post rdma req failed.\n", __func__);
        kmem_cache_free(rdma_queue->fs_rdma_obj_req_cache,
                        rdma_obj_req);
        goto out;
    }

    if(sync){
        drain_rdma_queue(rdma_queue);
    }

    ret = 0; // reset to 0 for succss.

out:
    return ret;
}

int rswap_atlas_page_map_dma(struct page *page, u64 *dma_addr) {
    int ret = 0;
    enum dma_data_direction dir;
    u64 tmp_addr = 0;
    struct ib_device *dev = rdma_session_global.rdma_dev->dev;

    dir = DMA_FROM_DEVICE;
    tmp_addr = ib_dma_map_page(dev, page, 0, PAGE_SIZE, dir);
    if (unlikely(ib_dma_mapping_error(dev, tmp_addr))) {
        pr_err("%s, ib_dma_mapping_error\n", __func__);
        ret = -ENOMEM;
        goto out;
    }
    ib_dma_sync_single_for_device(dev, tmp_addr, PAGE_SIZE, dir);
    *dma_addr = tmp_addr;

out:
    return ret;
}

int rswap_atlas_page_unmap_dma(u64 dma_addr) {

    int ret = 0;
    struct ib_device *dev = rdma_session_global.rdma_dev->dev;
    ib_dma_unmap_page(dev, dma_addr, PAGE_SIZE, DMA_FROM_DEVICE);
    return ret;
}

int rswap_atlas_sync(int cpu) {
    int ret = 0;
    struct rswap_rdma_queue *rdma_queue;

    rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_PREF_ASYNC);
    drain_rdma_queue(rdma_queue);
    return ret;
}

EXPORT_SYMBOL(rswap_atlas_pref_object);
EXPORT_SYMBOL(rswap_atlas_page_map_dma);
EXPORT_SYMBOL(rswap_atlas_page_unmap_dma);
EXPORT_SYMBOL(rswap_atlas_sync);