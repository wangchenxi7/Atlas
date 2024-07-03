#include "rswap_rdma.h"
#include "rswap_scheduler.h"
#include "rswap_rdma_custom_ops.h"
#include <linux/swap_stats.h>
#include <linux/hermit.h>

//
// ############################ Start of RDMA operation for Fronswap
// ############################
//

/**
 * Wait for the finish of ALL the outstanding rdma_request
 *
 */
void drain_rdma_queue(struct rswap_rdma_queue *rdma_queue)
{
	// [?] not disable preempt, other threads may keep enqueuing request
	// into the rdma_queue ?
	int nr_pending = atomic_read(&rdma_queue->rdma_post_counter);
	int nr_done = 0;

	uint64_t pf_ts = get_cycles_start();
	bool flag = true;
	// preempt_disable();
	while (atomic_read(&rdma_queue->rdma_post_counter) > 0) {
		int nr_completed;
		// default, IB_POLL_BATCH is 16. return when cqe reaches
		// min(16, IB_POLL_BATCH) or CQ is empty.
		nr_completed = ib_process_cq_direct(rdma_queue->cq, 4);
		if (nr_completed && flag) {
			uint64_t pf_tmp = get_cycles_end();
			accum_adc_time_stat(ADC_POLL_WAIT, pf_tmp - pf_ts);
			flag = false;
		}
		nr_done += nr_completed;
		if (nr_done >= nr_pending)
			break;
		cpu_relax();
	}
	// preempt_enable();
	accum_adc_time_stat(ADC_POLL_ALL, get_cycles_end() - pf_ts);

	return;
}

void write_drain_rdma_queue(struct rswap_rdma_queue *rdma_queue)
{
	// [?] not disable preempt, other threads may keep enqueuing request
	// into the rdma_queue ?
	int nr_pending = atomic_read(&rdma_queue->rdma_post_counter);
	int nr_done = 0;

	uint64_t pf_ts = get_cycles_start();
	bool flag = true;
	// uint64_t pf_stt = pf_ts;
	// preempt_disable();
	while (atomic_read(&rdma_queue->rdma_post_counter) > 0) {
		int nr_completed;
		// default, IB_POLL_BATCH is 16. return when cqe reaches
		// min(16, IB_POLL_BATCH) or CQ is empty.
		nr_completed = ib_process_cq_direct(rdma_queue->cq, 64);
		// nr_completed = ib_process_cq_relaxed(rdma_queue->cq, 64);
		if (nr_completed && flag) {
			uint64_t pf_tmp = get_cycles_end();
			accum_adc_time_stat(ADC_HERMIT_RMAP1_LAT,
					    pf_tmp - pf_ts);
			flag = false;
		}
		nr_done += nr_completed;
		if (nr_done >= nr_pending)
			break;
		cpu_relax();
	}
	// preempt_enable();

	accum_adc_time_stat(ADC_HERMIT_RMAP2_LAT, get_cycles_end() - pf_ts);
	return;
}

static inline int peek_rdma_queue(struct rswap_rdma_queue *rdma_queue)
{
	if (atomic_read(&rdma_queue->rdma_post_counter) > 0)
		ib_process_cq_direct(rdma_queue->cq, 4);
	return atomic_read(&rdma_queue->rdma_post_counter);
}


/**
 * Drain all the outstanding messages for a specific memory server.
 * [?] TO BE DONE. Multiple memory server
 *
 */
void drain_all_rdma_queues(int target_mem_server)
{
	int i;
	struct rdma_session_context *rdma_session = &rdma_session_global;

	for (i = 0; i < num_queues; i++) {
		drain_rdma_queue(&(rdma_session->rdma_queues[i]));
	}
}

/**
 * The callback function for rdma requests.
 *
 */
void fs_rdma_callback(struct ib_cq *cq, struct ib_wc *wc)
{
	// get the instance start address of fs_rdma_req, whose filed,
	// fs_rdma_req->cqe is pointed by wc->wr_cqe
	struct fs_rdma_req *rdma_req =
	    container_of(wc->wr_cqe, struct fs_rdma_req, cqe);
	struct rswap_rdma_queue *rdma_queue = cq->cq_context;
	struct ib_device *ibdev = rdma_queue->rdma_session->rdma_dev->dev;
	bool unlock = true;
	int cpu;
	enum rdma_queue_type type;

#if defined (RSWAP_KERNEL_SUPPORT) && RSWAP_KERNEL_SUPPORT >= 3
	unlock = !hmt_ctl_flag(HMT_LAZY_POLL);
#else
	unlock = true;
#endif

	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		pr_err("%s status is not success, it is=%d\n", __func__,
		       wc->status);
	}
	get_rdma_queue_cpu_type(&rdma_session_global, rdma_queue, &cpu, &type);
	if (type == QP_STORE) { // STORE requests
		set_page_writeback(rdma_req->page);
		unlock_page(rdma_req->page);
		end_page_writeback(rdma_req->page);
	} else if (type == QP_LOAD_SYNC) { // LOAD SYNC requests
		// originally called in swap_readpage(). Moved here for asynchrony.
		SetPageUptodate(rdma_req->page);
		if (unlock)
			unlock_page(rdma_req->page);
	} else if (type == QP_LOAD_ASYNC || type == QP_PREF_ASYNC) { // LOAD ASYNC requests
		// originally called in swap_readpage(). Moved here for asynchrony.
		SetPageUptodate(rdma_req->page);
		unlock_page(rdma_req->page);
	}

	atomic_dec(&rdma_queue->rdma_post_counter);
	ib_dma_unmap_page(ibdev, rdma_req->dma_addr, PAGE_SIZE,
			  type == QP_STORE ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
	kmem_cache_free(rdma_queue->fs_rdma_req_cache, rdma_req);

#ifdef ENABLE_VQUEUE
	rswap_proc_send_pkts_dec(rswap_vqlist_get_triple(cpu)->proc, type);
#endif
}

/**
 * When we enqueue a write/read wr,
 * the total number can't exceed the send/receive queue depth.
 * Or it will cause QP Out of Memory error.
 *
 * return :
 *  0 : success;
 *  -1 : error.
 *
 * More explanation:
 * There are 2 queues for QP, send/recv queue.
 * 1) Send queue limit the number of outstanding wr.
 *    This limits both the 1-sided/2-sided wr.
 * 2) For 2-sided RDMA, it also needs to post s recv wr to receive data.
 *    we reply on 1-sided RDMA wr to write/read data.
 *    The the depth of send queue should be much larger than recv queue.
 *
 * The depth of these 2 queues are limited by:
 * 	init_attr.cap.max_send_wr
 *	init_attr.cap.max_recv_wr
 *
 */
int fs_enqueue_send_wr(struct rdma_session_context *rdma_session,
                       struct rswap_rdma_queue *rdma_queue,
                       struct fs_rdma_req *rdma_req)
{
	int ret = 0;
	const struct ib_send_wr *bad_wr;
	int test;

	// points to the rdma_queue to be enqueued.
	rdma_req->rdma_queue = rdma_queue;

	// Post 1-sided RDMA read wr
	// wait and enqueue wr
	// Both 1-sided read/write queue depth are RDMA_SEND_QUEUE_DEPTH
	while (1) {
		test = atomic_inc_return(&rdma_queue->rdma_post_counter);
		if (test < RDMA_SEND_QUEUE_DEPTH - 16) {
			// post the 1-sided RDMA write
			// Use the global RDMA context, rdma_session_global
			ret = ib_post_send(
			    rdma_queue->qp,
			    (struct ib_send_wr *)&rdma_req->rdma_wr, &bad_wr);
			if (unlikely(ret)) {
				printk(KERN_ERR
				       "%s, post 1-sided RDMA send wr failed, "
				       "return value :%d. counter %d \n",
				       __func__, ret, test);
				ret = -1;
				goto err;
			}

			// Enqueue successfully.
			// exit loop.
			return ret;
		} else {
			// RDMA send queue is full, wait for next turn.
			test =
			    atomic_dec_return(&rdma_queue->rdma_post_counter);
			cpu_relax();

			// IB_DIRECT_CQ, poll cqe directly
			drain_rdma_queue(rdma_queue);
			pr_err("YIFAN: back pressure...\n");
		}
	}
err:
	printk(KERN_ERR " Error in %s \n", __func__);
	return -1;
}

/**
 * Build a rdma_wr for frontswap data path.
 *
 */
int fs_build_rdma_wr(struct rdma_session_context *rdma_session,
                     struct rswap_rdma_queue *rdma_queue,
                     struct fs_rdma_req *rdma_req,
                     struct remote_chunk *remote_chunk_ptr,
                     size_t offset_within_chunk, struct page *page,
                     enum rdma_queue_type type)
{
	int ret = 0;
	enum dma_data_direction dir;
	struct ib_device *dev = rdma_session->rdma_dev->dev;

	// 1) Map a single page as RDMA buffer
	rdma_req->page = page;

	dir = type == QP_STORE ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	rdma_req->dma_addr = ib_dma_map_page(dev, page, 0, PAGE_SIZE, dir);
	if (unlikely(ib_dma_mapping_error(dev, rdma_req->dma_addr))) {
		pr_err("%s, ib_dma_mapping_error\n", __func__);
		ret = -ENOMEM;
		kmem_cache_free(rdma_queue->fs_rdma_req_cache, rdma_req);
		goto out;
	}

	// Map the dma address to IB deivce.
	ib_dma_sync_single_for_device(dev, rdma_req->dma_addr, PAGE_SIZE, dir);

	rdma_req->cqe.done = fs_rdma_callback;

	// 2) Initialize the rdma_wr
	// 2.1 local addr
	rdma_req->sge.addr = rdma_req->dma_addr;
	rdma_req->sge.length = PAGE_SIZE;
	rdma_req->sge.lkey = rdma_session->rdma_dev->pd->local_dma_lkey;

	// 2.2 initia rdma_wr for remote addr
	rdma_req->rdma_wr.wr.next = NULL;
	// assing completion handler. prepare for container_of()
	rdma_req->rdma_wr.wr.wr_cqe = &rdma_req->cqe;
	rdma_req->rdma_wr.wr.sg_list = &(rdma_req->sge);
	// single page.  [?] how to support mutiple pages ?
	rdma_req->rdma_wr.wr.num_sge = 1;
	rdma_req->rdma_wr.wr.opcode =
	    (dir == DMA_TO_DEVICE ? IB_WR_RDMA_WRITE : IB_WR_RDMA_READ);
	rdma_req->rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	rdma_req->rdma_wr.remote_addr =
	    remote_chunk_ptr->remote_addr + offset_within_chunk;
	rdma_req->rdma_wr.rkey = remote_chunk_ptr->remote_rkey;

// debug
#ifdef DEBUG_MODE_BRIEF
	if (dir == DMA_FROM_DEVICE) {
		printk(KERN_INFO
		       "%s, read data from remote 0x%lx, size 0x%lx \n",
		       __func__, (size_t)rdma_req->rdma_wr.remote_addr,
		       (size_t)PAGE_SIZE);
	}
#endif

out:
	return ret;
}

/**
 * Enqueue a page into RDMA queue.
 *
 */
int rswap_rdma_send(int cpu, pgoff_t offset, struct page *page,
                    enum rdma_queue_type type)
{
	int ret = 0;
	size_t page_addr;
	size_t chunk_idx;
	size_t offset_within_chunk;
	struct rswap_rdma_queue *rdma_queue;
	struct fs_rdma_req *rdma_req;
	struct remote_chunk *remote_chunk_ptr;

// page offset, compared start of Data Region
// The real virtual address is RDMA_DATA_SPACE_START_ADDR + start_addr.
#ifdef ENABLE_SWP_ENTRY_VIRT_REMAPPING
	// calculate the remote addr
	page_addr = retrieve_swap_remapping_virt_addr_via_offset(offset)
	            << PAGE_SHIFT;
#else
	page_addr = pgoff2addr(offset);
#endif
	chunk_idx = page_addr >> CHUNK_SHIFT;
	offset_within_chunk = page_addr & CHUNK_MASK;

	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, type);
	rdma_req = (struct fs_rdma_req *)kmem_cache_alloc(
	    rdma_queue->fs_rdma_req_cache, GFP_ATOMIC);
	if (!rdma_req) {
		pr_err("%s, get reserved "
		       "fs_rdma_req failed. \n",
		       __func__);
		goto out;
	}

	remote_chunk_ptr =
	    &(rdma_session_global.remote_mem_pool.chunks[chunk_idx]);

	// initialize the rdma_req
	ret =
	    fs_build_rdma_wr(&rdma_session_global, rdma_queue, rdma_req,
	                     remote_chunk_ptr, offset_within_chunk, page, type);
	if (unlikely(ret)) {
		pr_err("%s, Build rdma_wr failed.\n", __func__);
		goto out;
	}

	// enqueue the rdma_req
	ret = fs_enqueue_send_wr(&rdma_session_global, rdma_queue, rdma_req);
	if (unlikely(ret)) {
		pr_err("%s, enqueue rdma_wr failed.\n", __func__);
		goto out;
	}

out:
	return ret;
}

//
// ############################ Start of Fronswap operations definition
// ############################
//

/**
 * Synchronously write data to memory server.
 *
 *  1.swap out is single pages in default.
 *    [?]  Can we make it support multiple pages swapout ?
 *
 *  2. This is a synchronous swapping out. Return only when pages is written
 * out. This is the assumption of frontswap store operation.
 *
 * Parameters
 *  type : used to select the swap device ?
 *  page_offset : swp_offset(entry). the offset for a page in the swap
 * partition/device. page : the handler of page.
 *
 *
 * return
 *  0 : success
 *  non-zero : failed.
 *
 */
int rswap_frontswap_store(unsigned type, pgoff_t swap_entry_offset,
                          struct page *page)
{
#ifdef ENABLE_VQUEUE
	int ret = 0;
	int cpu = -1;
	struct rswap_rdma_queue *rdma_queue;
	struct rswap_vqueue *vqueue;
	struct rswap_request vrequest = {swap_entry_offset, page};

	cpu = get_cpu(); // disable preemption

	vqueue = rswap_vqlist_get(cpu, QP_STORE);
	if (unlikely(ret = rswap_vqueue_enqueue(vqueue, &vrequest)) != 0) {
		print_err(ret);
		put_cpu();
		goto out;
	}

	put_cpu(); // enable preeempt.

	// 2.3 wait for write is done.

	// busy wait on the rdma_queue[cpu].
	// This is not exclusive.
	rswap_vqueue_drain(cpu, QP_STORE);
	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_STORE);
	write_drain_rdma_queue(rdma_queue); // poll the corresponding RDMA CQ

#else // !ENABLE_VQUEUE
	int ret = 0;
	int cpu;
	struct rswap_rdma_queue *rdma_queue;

	cpu = get_cpu();
	// 2.2 enqueue RDMA request
	ret = rswap_rdma_send(cpu, swap_entry_offset, page, QP_STORE);
	if (unlikely(ret)) {
		pr_err("%s, enqueuing rdma frontswap write failed.\n",
		       __func__);
		goto out;
	}

	put_cpu(); // enable preeempt.

	// 2.3 wait for write is done.

	// busy wait on the rdma_queue[cpu].
	// This is not exclusive.
	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_STORE);
	write_drain_rdma_queue(rdma_queue); // poll the corresponding RDMA CQ

	ret = 0; // reset to 0 for succss.
#endif

out:
	return ret;
}

int rswap_frontswap_peek_store(int core);
int rswap_frontswap_poll_store(int core);

int rswap_frontswap_store_on_core(unsigned type, pgoff_t swap_entry_offset,
                          struct page *page, int core)
{
	int ret = 0;

	static atomic_t cnters[48];

	core %= NR_WRITE_QUEUE;

	// 2.2 enqueue RDMA request
	ret = rswap_rdma_send(core, swap_entry_offset, page, QP_STORE);
	if (unlikely(ret)) {
		pr_err("%s, enqueuing rdma frontswap write failed.\n",
		       __func__);
		goto out;
	}

	ret = 0; // reset to 0 for succss.

	if (atomic_inc_return(&cnters[core]) % 16 == 0) {
		/* rswap_frontswap_peek_store(core); */
		rswap_frontswap_poll_store(core);
	}

out:
	return ret;
}

int rswap_frontswap_poll_store(int core)
{
	struct rswap_rdma_queue *rdma_queue;

	core %= NR_WRITE_QUEUE;

	rdma_queue = get_rdma_queue(&rdma_session_global, core, QP_STORE);
	write_drain_rdma_queue(rdma_queue); // poll the corresponding RDMA CQ
	return 0;
}

/**
 * Synchronously read data from memory server.
 *
 *
 * return:
 *  0 : success
 *  non-zero : failed.
 */
int rswap_frontswap_load(unsigned type, pgoff_t swap_entry_offset,
                         struct page *page)
{
#ifdef ENABLE_VQUEUE
	int ret = 0;
	int cpu = -1;
	struct rswap_vqueue *vqueue;
	// struct rswap_rdma_queue *rdma_queue;
	struct rswap_request vrequest = {swap_entry_offset, page};

	cpu = smp_processor_id();

	vqueue = rswap_vqlist_get(cpu, QP_LOAD_SYNC);
	if ((ret = rswap_vqueue_enqueue(vqueue, &vrequest)) != 0) {
		print_err(ret);
		goto out;
	}

	/* YIFAN: seems we don't need to poll here after separating STORE queue
	 * and LOAD_SYNC queue. We poll LOAD_STNC queue with poll_load().
	 */
	// rswap_vqueue_drain(vqueue);
	// rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_LOAD_SYNC);
	// drain_rdma_queue(rdma_queue); // poll the corresponding RDMA CQ

#else // !ENABLE_VQUEUE
	int ret = 0;
	int cpu;
	// struct rswap_rdma_queue *rdma_queue;

	// 2) RDMA path
	cpu = smp_processor_id();

	// 2.2 enqueue RDMA request
	ret = rswap_rdma_send(cpu, swap_entry_offset, page, QP_LOAD_SYNC);
	if (unlikely(ret)) {
		pr_err("%s, enqueuing rdma frontswap write failed.\n",
		       __func__);
		goto out;
	}

	/* YIFAN: seems we don't need to poll here after separating STORE queue
	 * and LOAD_SYNC queue. We poll LOAD_STNC queue with poll_load().
	 */
	// rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_LOAD_SYNC);
	// drain_rdma_queue(rdma_queue);

	ret = 0; // reset to 0 for succss.
#endif // ENABLE_VQUEUE

out:
	return ret;
}

int rswap_frontswap_load_async(unsigned type, pgoff_t swap_entry_offset,
                               struct page *page)
{
#ifdef ENABLE_VQUEUE
	int ret = 0;
	int cpu = -1;
	struct rswap_vqueue *vqueue;
	struct rswap_request vrequest = {swap_entry_offset, page};

	// 2) RDMA path
	cpu = smp_processor_id();

	vqueue = rswap_vqlist_get(cpu, QP_LOAD_ASYNC);
	if ((ret = rswap_vqueue_enqueue(vqueue, &vrequest)) != 0) {
		print_err(ret);
		goto out;
	}

#else //! ENABLE_VQUEUE
	int ret = 0;
	int cpu;

	// 2) RDMA path
	cpu = smp_processor_id();

	// 2.2 enqueue RDMA request
	ret = rswap_rdma_send(cpu, swap_entry_offset, page, QP_LOAD_ASYNC);
	if (unlikely(ret)) {
		pr_err("%s, enqueuing rdma frontswap write failed.\n",
		       __func__);
		goto out;
	}

	ret = 0; // reset to 0 for succss.

#endif // ENABLE_VQUEUE

out:
	return ret;
}

int rswap_frontswap_pref_async(unsigned type, pgoff_t swap_entry_offset,
                               struct page *page, int cpu)
{
	int ret = 0;

	// 2) RDMA path

	// 2.2 enqueue RDMA request
	ret = rswap_rdma_send(cpu, swap_entry_offset, page, QP_PREF_ASYNC);
	if (unlikely(ret)) {
		pr_err("%s, enqueuing rdma frontswap write failed.\n",
		       __func__);
		goto out;
	}

	ret = 0; // reset to 0 for succss.

out:
	return ret;
}

int rswap_frontswap_poll_load(int cpu)
{
#ifdef ENABLE_VQUEUE
	struct rswap_rdma_queue *rdma_queue;

	rswap_vqueue_drain(cpu, QP_LOAD_SYNC);
	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_LOAD_SYNC);
	drain_rdma_queue(rdma_queue); // poll the corresponding RDMA CQ
#else // !ENABLE_VQUEUE
	struct rswap_rdma_queue *rdma_queue;

	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_LOAD_SYNC);
	drain_rdma_queue(rdma_queue); // poll the corresponding RDMA CQ
#endif                                // ENABLE_VQUEUE
	return 0;
}

int rswap_frontswap_peek_load(int cpu)
{
	struct rswap_rdma_queue *rdma_queue =
		get_rdma_queue(&rdma_session_global, cpu, QP_LOAD_SYNC);
	return peek_rdma_queue(rdma_queue);
}

int rswap_frontswap_poll_pref(int cpu)
{
	struct rswap_rdma_queue *rdma_queue;

	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_PREF_ASYNC);
	drain_rdma_queue(rdma_queue); // poll the corresponding RDMA CQ
	return 0;
}

int rswap_frontswap_peek_pref(int cpu)
{
	struct rswap_rdma_queue *rdma_queue =
		get_rdma_queue(&rdma_session_global, cpu, QP_PREF_ASYNC);
	return peek_rdma_queue(rdma_queue);
}

int rswap_frontswap_peek_store(int cpu)
{
	struct rswap_rdma_queue *rdma_queue;
	cpu %= NR_WRITE_QUEUE;
	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_STORE);
	return peek_rdma_queue(rdma_queue);
}

static void rswap_invalidate_page(unsigned type, pgoff_t offset)
{
#ifdef DEBUG_MODE_DETAIL
	pr_info("%s, remove page_virt addr 0x%lx\n", __func__,
	        offset << PAGE_OFFSET);
#endif
	return;
}

/**
 * What's the purpose ?
 *
 */
static void rswap_invalidate_area(unsigned type)
{
#ifdef DEBUG_MODE_DETAIL
	pr_warn("%s, remove the pages of area 0x%x ?\n", __func__, type);
#endif
	return;
}

static void rswap_frontswap_init(unsigned type) {}

static struct frontswap_ops rswap_frontswap_ops = {
	.init = rswap_frontswap_init,
	.store = rswap_frontswap_store,
	.load = rswap_frontswap_load,
	.invalidate_page = rswap_invalidate_page,
	.invalidate_area = rswap_invalidate_area,
#ifdef RSWAP_KERNEL_SUPPORT
	.load_async = rswap_frontswap_load_async,
	.poll_load = rswap_frontswap_poll_load,
#if RSWAP_KERNEL_SUPPORT >= 2
	.store_on_core = rswap_frontswap_store_on_core,
	.poll_store = rswap_frontswap_poll_store,
#endif // RSWAP_KERNEL_SUPPORT >= 2
#if RSWAP_KERNEL_SUPPORT >= 3
	.peek_load = rswap_frontswap_peek_load,
	.peek_store = rswap_frontswap_peek_store,
	.load_async_early_map = rswap_frontswap_load_async_early_map,
	.pref_async_early_map = rswap_frontswap_pref_async_early_map,
	.pref_async = rswap_frontswap_pref_async,
	.peek_pref = rswap_frontswap_peek_pref,
	.poll_pref = rswap_frontswap_poll_pref,
#endif // RSWAP_KERNEL_SUPPORT >= 3
#endif
};

int rswap_register_frontswap(void)
{
	int ret = 0;
	// enable the frontswap path
	frontswap_register_ops(&rswap_frontswap_ops);

#ifdef ENABLE_VQUEUE
	ret = rswap_scheduler_init();
	if (ret) {
		printk(KERN_ERR "%s, rswap_scheduler_init failed. \n",
		       __func__);
		goto out;
	}
out:
#endif // ENABLE_VQUEUE

	pr_info("frontswap module loaded\n");
	return ret;
}

int rswap_replace_frontswap(void)
{
	frontswap_ops->init = rswap_frontswap_ops.init;
	frontswap_ops->store = rswap_frontswap_ops.store;
	frontswap_ops->load = rswap_frontswap_ops.load;
	frontswap_ops->invalidate_page = rswap_frontswap_ops.invalidate_page,
	frontswap_ops->invalidate_area = rswap_frontswap_ops.invalidate_area,
#ifdef RSWAP_KERNEL_SUPPORT
	frontswap_ops->load_async = rswap_frontswap_ops.load_async;
	frontswap_ops->poll_load = rswap_frontswap_ops.poll_load;
#if RSWAP_KERNEL_SUPPORT >= 2
	frontswap_ops->store_on_core = rswap_frontswap_ops.store_on_core;
	frontswap_ops->poll_store = rswap_frontswap_ops.poll_store;
#endif // RSWAP_KERNEL_SUPPORT >= 2
#if RSWAP_KERNEL_SUPPORT >= 3
	frontswap_ops->peek_load = rswap_frontswap_ops.peek_load;
	frontswap_ops->peek_store = rswap_frontswap_ops.peek_store;
	frontswap_ops->load_async_early_map = rswap_frontswap_ops.load_async_early_map;
	frontswap_ops->pref_async_early_map = rswap_frontswap_ops.pref_async_early_map;
	frontswap_ops->pref_async = rswap_frontswap_ops.pref_async;
	frontswap_ops->peek_pref = rswap_frontswap_ops.peek_pref;
	frontswap_ops->poll_pref = rswap_frontswap_ops.poll_pref;
#endif // RSWAP_KERNEL_SUPPORT >= 3
#endif
	pr_info("frontswap ops replaced\n");
	return 0;
}

void rswap_deregister_frontswap(void)
{
#ifdef RSWAP_KERNEL_SUPPORT
	frontswap_ops->init = NULL;
	frontswap_ops->store = NULL;
	frontswap_ops->load = NULL;
	frontswap_ops->load_async = NULL;
	frontswap_ops->poll_load = NULL;
#else
	frontswap_ops->init = NULL;
	frontswap_ops->store = NULL;
	frontswap_ops->load = NULL;
	frontswap_ops->poll_load = NULL;
#endif
	pr_info("frontswap ops deregistered\n");
}

int rswap_client_init(char *_server_ip, int _server_port, int _mem_size)
{
	int ret = 0;
	printk(KERN_INFO "%s, start \n", __func__);

	// online cores decide the parallelism. e.g. number of QP, CP etc.
	online_cores = num_online_cpus();
	num_queues = online_cores * NUM_QP_TYPE;
	server_ip = _server_ip;
	server_port = _server_port;
	rdma_session_global.remote_mem_pool.remote_mem_size = _mem_size;
	rdma_session_global.remote_mem_pool.chunk_num = _mem_size / REGION_SIZE_GB;

	pr_info("%s, num_queues : %d (Can't exceed the slots on Memory server) \n",
		__func__, num_queues);

	// init the rdma session to memory server
	ret = init_rdma_sessions(&rdma_session_global);

	// Build both the RDMA and Disk driver
	ret = rdma_session_connect(&rdma_session_global);
	if (unlikely(ret)) {
		pr_err("%s, rdma_session_connect failed. \n", __func__);
		goto out;
	}

#ifdef ENABLE_VQUEUE
	ret = rswap_scheduler_init();
	if (ret) {
		printk(KERN_ERR "%s, rswap_scheduler_init failed. \n",
		       __func__);
		goto out;
	}
#endif // ENABLE_VQUEUE
out:
	return ret;
}

void rswap_client_exit(void)
{
	int ret;
	ret = rswap_disconnect_and_collect_resource(&rdma_session_global);
	if (unlikely(ret)) {
		printk(KERN_ERR "%s,  failed.\n", __func__);
	}
	printk(KERN_INFO "%s done.\n", __func__);
#ifdef ENABLE_VQUEUE
	rswap_scheduler_stop();
#endif // ENABLE_VQUEUE
}
