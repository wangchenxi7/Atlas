/*
 * mm/hermit.c - hermit virtual-address directed rmap & swap
 */
#include <linux/hermit.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/debugfs.h>
#include <linux/swap.h>
#include <linux/cpu.h>
#include <linux/pagemap.h>

#include "linux/swap_stats.h"
#include "internal.h"

/*
 * global variables for profile and control
 */
uint32_t hmt_sthd_cores[NR_CPUS];

bool hmt_ctl_flags[NUM_HMT_CTL_FLAGS];
EXPORT_SYMBOL(hmt_ctl_flags);
const char *hmt_ctl_flag_names[NUM_HMT_CTL_FLAGS] = {
	"bypass_swapcache",
	"datapath",
	"swap_thread",
	"prefetch_thread",
	"pthd_stop",
	"dsa_prefetch",
	"batch_swapout",
	"batch_tlb",
	"batch_io",
	"batch_account",
	"vaddr_swapout",
	"speculative_io",
	"speculative_lock",
	"lazy_poll",
	"prefetch_early_map",
	"async_prefetch",
	"prefetch_batch_charge",
	"prefetch_direct_poll",
	"prefetch_direct_map",
	"prefetch_always_ascend",
	"prefetch_lazy_update_metadata",
	"prefetch_populate",
	"atl_card_prof",
	"atl_card_prof_print"
};

unsigned hmt_ctl_vars[NUM_HMT_CTL_VARS];
const char *hmt_ctl_var_names[NUM_HMT_CTL_VARS] = {
	"sthd_cnt",
	"sthd_sleep_dur",
	"reclaim_mode",
	"populate_work_cnt",
	"min_sthd_cnt",
	"atl_card_prof_thres",
	"atl_card_prof_low_thres"
};

static struct kmem_cache *vaddr_cachep;
static struct kmem_cache *vpage_cachep;
static struct kmem_cache *dsa_cachep;

struct workqueue_struct * hermit_ub_wq = NULL;
struct kmem_cache* hermit_work_cache = NULL;

struct workqueue_struct * hermit_pref_wq = NULL;
struct kmem_cache* hermit_pref_work_cache = NULL;

struct workqueue_struct * hermit_umd_wq = NULL;
struct kmem_cache* hermit_umd_work_cache = NULL;

EXPORT_SYMBOL(hermit_ub_wq);

// profile DSA swap-in faults
// #define HERMIT_DBG_PF_TRACE
#ifdef HERMIT_DBG_PF_TRACE
#define VADDR_BUF_LEN (20L * 1000 * 1000)
static atomic_t vaddr_cnt;
static unsigned long *vaddr_buf;
atomic_t psf_flip_cnt;
atomic_t swapout_cnt;
atomic_t amplification;
atomic_t spf_cnt;
unsigned int *spf_lat_buf[NUM_SPF_LAT_TYPE];
#endif // HERMIT_DBG_PF_TRACE

/*
 * prefetch thread
 */
DEFINE_PER_CPU(struct pref_request_queue, pref_request_queue);

/*
 * initialization
 */
static inline void hermit_debugfs_init(void)
{
	struct dentry *root = debugfs_create_dir("hermit", NULL);
	int i;
#ifdef HERMIT_DBG_PF_TRACE
	static struct debugfs_blob_wrapper vaddr_blob;
	static struct debugfs_blob_wrapper spf_lat_blob[NUM_SPF_LAT_TYPE];
	char fname[15] = "spf_lats0\0";
#endif // HERMIT_DBG_PF_TRACE

	if (!root)
		return;

#ifdef HERMIT_DBG_PF_TRACE
	vaddr_buf =
		kvmalloc(sizeof(unsigned long) * 4 * VADDR_BUF_LEN, GFP_KERNEL);
	vaddr_blob.data = (void *)vaddr_buf;
	vaddr_blob.size = sizeof(unsigned long) * 4 * VADDR_BUF_LEN;
	atomic_set(&vaddr_cnt, 0);
	pr_err("create hermit debugfs files %p", (void *)vaddr_buf);

	debugfs_create_atomic_t("vaddr_cnt", 0666, root, &vaddr_cnt);
	debugfs_create_blob("vaddr_list", 0666, root, &vaddr_blob);

	debugfs_create_atomic_t("psf_flip_cnt", 0666, root, &psf_flip_cnt);
	atomic_set(&psf_flip_cnt, 0);
	debugfs_create_atomic_t("swapout_cnt", 0666, root, &swapout_cnt);
	atomic_set(&swapout_cnt, 0);
	debugfs_create_atomic_t("amplification", 0666, root, &amplification);
	atomic_set(&amplification, 0);

	debugfs_create_atomic_t("spf_cnt", 0666, root, &spf_cnt);
	atomic_set(&spf_cnt, 0);
	for (i = 0; i < NUM_SPF_LAT_TYPE; i++) {
		spf_lat_buf[i] =
			kvmalloc(sizeof(unsigned int) * SPF_BUF_LEN, GFP_KERNEL);
		spf_lat_blob[i].data = (void *)spf_lat_buf[i];
		spf_lat_blob[i].size = sizeof(unsigned int) * SPF_BUF_LEN;
		sprintf(fname, "spf_lats%d", i);
		debugfs_create_blob(fname, 0666, root, &spf_lat_blob[i]);
	}
#endif // HERMIT_DBG_PF_TRACE

	for (i = 0; i < NUM_HMT_CTL_FLAGS; i++)
		debugfs_create_bool(hmt_ctl_flag_names[i], 0666, root,
			    &hmt_ctl_flags[i]);

	for (i = 0; i < NUM_HMT_CTL_VARS; i++)
		debugfs_create_u32(hmt_ctl_var_names[i], 0666, root,
				   &hmt_ctl_vars[i]);
}

int __init hermit_init(void)
{
	int i;
	for (i = 0; i < NUM_HMT_CTL_FLAGS; i++)
		hmt_ctl_flags[i] = false;

	hmt_ctl_vars[HMT_STHD_CNT] = 16;
	hmt_ctl_vars[HMT_RECLAIM_MODE] = 0;
	hmt_ctl_vars[HMT_STHD_SLEEP_DUE] = 1000;
	hmt_ctl_vars[HMT_PPLT_WORK_CNT] = 16;
	hmt_ctl_vars[HMT_MIN_STHD_CNT] = 1;
	hmt_ctl_vars[ATL_CARD_PROF_THRES] = 24;
	hmt_ctl_vars[ATL_CARD_PROF_LOW_THRES] = 8;

	for (i = 0; i < hmt_ctl_var(HMT_STHD_CNT); i++)
		hmt_sthd_cores[i] = RMGRID_NR_HCORE - 1 - i;

	vaddr_cachep = kmem_cache_create("hermit_vaddr", sizeof(struct vaddr),
					 0, SLAB_PANIC, NULL);
	vpage_cachep = kmem_cache_create("hermit_vpage", sizeof(struct vpage),
					 0, SLAB_PANIC, NULL);
	dsa_cachep =
		kmem_cache_create("hermit_dsa", sizeof(struct ds_area_struct),
				  0, SLAB_PANIC, NULL);
	hermit_work_cache = kmem_cache_create("hermit_work", sizeof(struct hermit_populate_work), 0, SLAB_PANIC, NULL);
	hermit_pref_work_cache = kmem_cache_create("hermit_pref_work", sizeof(struct hermit_pref_work), 0, SLAB_PANIC, NULL);
	hermit_umd_work_cache = kmem_cache_create("hermit_umd_work", sizeof(struct hermit_umd_work), 0, SLAB_PANIC, NULL);

	hermit_debugfs_init();
	hermit_ub_wq = alloc_workqueue("hermit_ub_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if(!hermit_ub_wq)
		pr_err("%s: failed to create hermit_ub_wq\n", __func__);
	else
		printk("hermit: created hermit_ub_wq\n");
	hermit_pref_wq = alloc_workqueue("hermit_pref_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if(!hermit_pref_wq)
		pr_err("%s: failed to create hermit_pref_wq\n", __func__);
	else
		printk("hermit: created hermit_pref_wq\n");
	hermit_umd_wq = hermit_ub_wq; // [ls] does it need a seperate wq?

	return 0;
}
__initcall(hermit_init);


/*
 * [Hermit] vaddr field for reverse mapping
 */
unsigned long *hermit_page_vaddrs = NULL;
static int __init hermit_init_page_vaddrs(void)
{
	hermit_page_vaddrs =
		kvmalloc(sizeof(unsigned long) * (RMGRID_MAX_MEM / PAGE_SIZE),
			 GFP_KERNEL);
	memset(hermit_page_vaddrs, 0,
	       sizeof(unsigned long) * (RMGRID_MAX_MEM / PAGE_SIZE));
	pr_err("hermit_page_vaddrs init at 0x%lx\n",
	       (unsigned long)hermit_page_vaddrs);
	return 0;
}
early_initcall(hermit_init_page_vaddrs);

/*
 * [ls] use array as swapcache
 */
struct page **hermit_swapcache = NULL;
static int __init hermit_init_swapcache(void)
{
	hermit_swapcache =
		kvmalloc(sizeof(struct page*) * (RMGRID_MAX_SWAP / PAGE_SIZE),
			 GFP_KERNEL);
	if(!hermit_swapcache){
		pr_err("hermit_swapcache init failed\n");
		return 0;
	}
	memset(hermit_swapcache, 0,
	       sizeof(struct page*) * (RMGRID_MAX_SWAP / PAGE_SIZE));
	pr_err("hermit_swapcache init at 0x%lx\n",
	       (unsigned long)hermit_swapcache);
	return 0;
}
early_initcall(hermit_init_swapcache);

/*
 * [Hermit] per-thread virtual address list for swapping
 */
static inline struct vaddr *vaddr_alloc(void)
{
	return kmem_cache_alloc(vaddr_cachep, GFP_KERNEL);
}

static inline struct vpage *vpage_alloc(void)
{
	return kmem_cache_alloc(vpage_cachep, GFP_KERNEL);
}

/*
 * interfaces
 */

void hermit_record_refault_dist(unsigned long addr,
				unsigned long refault_dist)
{
	struct task_struct *task = current;
	struct ds_area_struct *dsa;

	if (!hmt_ctl_flag(HMT_DATAPATH))
		return;

	dsa = search_dsa_list(&task->dsa_list, &task->dsl_lock, addr);
	if (dsa) {
		dsa_update_rft_dist(dsa, refault_dist);
		return;
	}
	// 2) if 1) fails, search global dsa list then
	dsa = search_dsa_list(&task->mm->dsa_list, &task->mm->dsl_lock, addr);
	if (dsa) {
		dsa_update_rft_dist(dsa, refault_dist);
		return;
	}

	// 3) page is not in any DSA, add it into the thread private DSA
	dsa_update_rft_dist(&task->dsa, refault_dist);
}


/*
 * find the corresponding DSA for the current thread.
 * return the thread private DSA if @vaddr doesn't belong to any registered DSA.
 */
struct ds_area_struct *hermit_find_dsa(unsigned long vaddr)
{
	struct task_struct *thd = current;
	struct ds_area_struct *ret = NULL;
	struct ds_area_struct *pos;
	// if (!hermit_datapath_enabled)
	// 	return NULL;
	list_for_each_entry (pos, &thd->dsa_list, node) {
		if (in_dsa(vaddr, pos)) {
			ret = pos;
			break;
		}
	}
	if (ret)
		return ret;
	// return &thd->dsa;
	return NULL;
}

/*
 * manage vaddr
 */
static inline struct vaddr *create_vaddr(struct vm_area_struct *vma,
					 unsigned long addr)
{
	struct vaddr *vaddr = vaddr_alloc();
	if (!vaddr)
		return NULL;
	// vaddr->vma = vma;
	vaddr->address = addr;
	return vaddr;
}

static inline void free_vaddr(struct vaddr *vaddr)
{
	kmem_cache_free(vaddr_cachep, vaddr);
}

void free_vaddrs(struct list_head *vaddr_list)
{
	struct list_head *pos, *tmp;
	list_for_each_safe (pos, tmp, vaddr_list) {
		struct vaddr *vaddr = container_of(pos, struct vaddr, node);
		list_del(pos);
		free_vaddr(vaddr);
	}
	// INIT_LIST_HEAD(vaddr_list);
}

struct page *vaddr2page(struct vaddr *vaddr)
{
	const bool verbose = false;
	struct page *page;
	pte_t *pte;
	struct vm_area_struct *vma =
		hermit_find_vma(current->mm, vaddr->address);
	struct page_vma_mapped_walk pvmw = {
		.vma = vma,
		.address = vaddr->address,
	};
	if (!hermit_addr_vma_walk(&pvmw, false))
		return NULL;
	pte = pvmw.pte;
	page = vm_normal_page_silent(vma, vaddr->address, *pte);
	spin_unlock(pvmw.ptl);
	if (!page) {
		if (verbose) {
			pr_err("vaddr refers invalid page");
		}
		return NULL;
	} else if (total_mapcount(page) != 1) {
		if (verbose) {
			pr_err("page %p page_mapcount %d", page,
			       page ? page_mapcount(page) : 0);
		}
		return NULL;
	}
	if (unlikely(!get_page_unless_zero(page))) {
		return NULL;
	}
	if (isolate_lru_page(page)) {
		if (verbose)
			pr_err("isolate_lru_page failed for page %p", page);
		put_page(page);
		return NULL;
	}
	put_page(page);
	// inced page ref here. deced later when shrink_page_list_hermit
	return page;
}

static inline void record_vaddr(struct task_struct *task,
				struct vm_area_struct *vma, unsigned long addr)
{
#ifdef HERMIT_DBG_PF_TRACE
	int cnt;
	if (is_hermit_app(task->comm)) {
		cnt = atomic_inc_return(&vaddr_cnt) - 1;
		// cnt %= VADDR_BUF_LEN;
		if (cnt >= VADDR_BUF_LEN)
			return;
		vaddr_buf[cnt * 4 + 0] = task_pid_vnr(task);
		vaddr_buf[cnt * 4 + 1] = addr;
		vaddr_buf[cnt * 4 + 2] = vma->vm_start;
		vaddr_buf[cnt * 4 + 3] = vma->vm_end - vma->vm_start;
	}
#endif // HERMIT_DBG_PF_TRACE
}

inline void page_add_vaddr(pte_t *pte, struct page *page,
			   struct vm_area_struct *vma, unsigned long addr)
{
	struct task_struct *task = current;
	struct ds_area_struct *dsa;
	if (!task->mm) {
		pr_err("kthread %s", task->comm);
		return;
	}

	record_vaddr(task, vma, addr);
	if (!hmt_ctl_flag(HMT_DATAPATH))
		return;

	// 1) search thd local dsa list first
	dsa = search_dsa_list(&task->dsa_list, &task->dsl_lock, addr);
	if (dsa) {
		dsa_add_vaddr(dsa, false, vma, addr);
		return;
	}
	// 2) if 1) fails, search global dsa list then
	dsa = search_dsa_list(&task->mm->dsa_list, &task->mm->dsl_lock, addr);
	if (dsa) {
		if (dsa->type == DSA_PARFOR) {
			struct ds_area_struct *pos, *tmp;
			struct ds_area_struct *new_dsa = create_dsa(
				dsa->start, dsa->end - dsa->start, dsa->type);
			if (!new_dsa) {
				pr_err("ERROR! %s:%d cannot alloc new dsa.",
				       __func__, __LINE__);
				return;
			}
			spin_lock(&task->dsl_lock);
			/* YIFAN: dirty workaround: delete stale PARFOR DSAs */
			list_for_each_entry_safe (pos, tmp, &task->dsa_list,
						  node) {
				if (pos->type == DSA_PARFOR) {
					struct ds_area_struct *gpos, *gtmp;
					bool find = false;
					list_for_each_entry_safe (
						gpos, gtmp, &task->mm->dsa_list,
						node) {
						if (gpos->type == DSA_PARFOR &&
						    pos->start == gpos->start &&
						    pos->end == gpos->end) {
							find = true;
							break;
						}
					}
					if (!find) {
						list_del(&pos->node);
						free_dsa(pos, true);
						atomic_dec(&task->dsa_cnt);
					}
				}
			}
			list_add(&new_dsa->node, &task->dsa_list);
			atomic_inc(&task->dsa_cnt);
			spin_unlock(&task->dsl_lock);
			dsa_add_vaddr(new_dsa, false, vma, addr);
		} else {
			dsa_add_vaddr(dsa, true, vma, addr);
		}
		return;
	}

	// 3) page is not in any DSA, add it to the thread private DSA
	dsa_add_vaddr(&task->dsa, true, vma, addr);
}

/*
 * manage vpage
 */
inline struct vpage *create_vpage(struct vm_area_struct *vma,
				  unsigned long address, struct page *page,
				  pte_t *pte)
{
	struct vpage *vpage = NULL;
	vpage = vpage_alloc();
	if (!vpage)
		return NULL;
	vpage->vma = vma;
	vpage->address = address;
	vpage->page = page;
	vpage->pte = pte;
	return vpage;
}

inline void free_vpage(struct vpage *vpage)
{
	kmem_cache_free(vpage_cachep, vpage);
}

struct vpage *vaddr2vpage(struct vaddr *vaddr)
{
	struct vpage *vpage;
	struct page *page;
	pte_t *pte;
	struct vm_area_struct *vma =
		hermit_find_vma(current->mm, vaddr->address);
	struct page_vma_mapped_walk pvmw = {
		.vma = vma,
		.address = vaddr->address,
	};
	if (!hermit_addr_vma_walk(&pvmw, false))
		return NULL;
	pte = pvmw.pte;
	page = vm_normal_page_silent(vma, vaddr->address, *pte);
	spin_unlock(pvmw.ptl);
	if (!page) {
		return NULL;
	} else if (total_mapcount(page) != 1) {
		return NULL;
	}

	vpage = create_vpage(vma, vaddr->address, page, pte);
	return vpage;
}

int isolate_vaddrs(struct task_struct *cthd, struct list_head *vaddr_list,
		   int nr_to_isolate, unsigned long threshold)
{
	int ret = 0;
	if (!hmt_ctl_flag(HMT_DATAPATH) || !cthd->mm)
		return 0;

	// 1) search thread local dsa list first
	if (atomic_read(&cthd->dsa_cnt) > 0) {
		struct ds_area_struct *pos, *tmp;
		spin_lock(&cthd->dsl_lock);
		list_for_each_entry_safe (pos, tmp, &cthd->dsa_list, node) {
			int vaddr_cnt = atomic_read(&pos->vaddr_cnt);
			int nr_isolated;
			if (vaddr_cnt == 0)
				continue;
			// nr_to_isolate = min(max(32, vaddr_cnt / 8), 128);
			nr_isolated =
				dsa_isolate_vaddrs(pos, false, vaddr_list,
						   nr_to_isolate, threshold);
			ret += nr_isolated;
			// pr_err("%s:%d thread dsa %p isolates %d/%d pages",
			//        __func__, __LINE__, pos, nr_isolated, ret);

			// if (ret >= nr_to_isolate)
			// 	break;
		}
		spin_unlock(&cthd->dsl_lock);
	}
	// if (ret >= nr_to_isolate)
	// 	return ret;
	// 2) search global dsa list
	if (atomic_read(&cthd->mm->dsa_cnt) > 0) {
		struct ds_area_struct *pos, *tmp;
		spin_lock(&cthd->mm->dsl_lock);
		list_for_each_entry_safe (pos, tmp, &cthd->mm->dsa_list, node) {
			int vaddr_cnt = atomic_read(&pos->vaddr_cnt);
			int nr_isolated;
			if (vaddr_cnt == 0)
				continue;
			// nr_to_isolate = min(max(32, vaddr_cnt / 8), 128);
			nr_isolated =
				dsa_isolate_vaddrs(pos, false, vaddr_list,
						   nr_to_isolate, threshold);
			ret += nr_isolated;
			// pr_err("%s:%d global dsa %p isolates %d pages, total %d pages",
			//        __func__, __LINE__, pos, nr_isolated, ret);

			// if (ret >= nr_to_isolate)
			// 	break;
		}
		spin_unlock(&cthd->mm->dsl_lock);
	}
	// if (ret >= nr_to_isolate)
	// 	return ret;
	// 3) fall back to the original solution
	// nr_to_isolate =
	// 	min(max(32, atomic_read(&cthd->dsa.vaddr_cnt) / 8), 128);
	ret += dsa_isolate_vaddrs(&cthd->dsa, true, vaddr_list, nr_to_isolate,
				  threshold);
	return ret;
}

int isolate_vpages(struct task_struct *cthd, struct list_head *vpage_list,
		   int nr_to_isolate)
{
	unsigned long nr_isolated = 0;
	unsigned long nr_taken = 0;
	int retry = 0;

	unsigned long workingset_size = 0;
	// workingset_size = min(thd_workingset_size(cthd), 655360ul);
	workingset_size = thd_workingset_size(cthd);
	// pr_err("YIFAN: workingset size: %lu", workingset_size);

	while (nr_taken < nr_to_isolate && retry < 10) {
		LIST_HEAD(vaddr_list);
		struct list_head *pos, *tmp;
		nr_isolated += isolate_vaddrs(cthd, &vaddr_list, nr_to_isolate,
					      workingset_size);
		if (nr_isolated == 0)
			goto done;

		list_for_each_safe (pos, tmp, &vaddr_list) {
			struct vaddr *vaddr;
			struct vpage *vpage = NULL;
			struct page *page = NULL;

			vaddr = container_of(pos, struct vaddr, node);
			vpage = vaddr2vpage(vaddr);
			if (!vpage)
				continue;
			page = vpage->page;
			if (!page) {
				free_vpage(vpage);
				continue;
			}
			nr_taken++;
			list_add_tail(&vpage->node, vpage_list);
		}
		free_vaddrs(&vaddr_list);

		retry++;
	}
	if (nr_isolated < nr_taken) {
		pr_err("YIFAN: impossible!! %lu <= %lu", nr_isolated, nr_taken);
	}
	adc_counter_add(nr_isolated, ADC_HERMIT_VADDRS);
	adc_counter_add(nr_taken, ADC_HERMIT_VPAGES);
done:
	reset_all_rft_dist(cthd);
	return nr_taken;
}

void free_vpages(struct list_head *vpage_list)
{
	struct list_head *pos, *tmp;
	list_for_each_safe (pos, tmp, vpage_list) {
		struct vpage *vpage = container_of(pos, struct vpage, node);
		list_del(pos);
		free_vpage(vpage);
	}
	// INIT_LIST_HEAD(vpage_list);
}

/*
 * manage dsa
 */
static inline struct ds_area_struct *dsa_alloc(void)
{
	return kmem_cache_alloc(dsa_cachep, GFP_KERNEL);
}

struct ds_area_struct *create_dsa(unsigned long start, unsigned long len,
				  enum ds_area_type type)
{
	struct ds_area_struct *dsa = dsa_alloc();
	if (!dsa)
		return NULL;
	init_dsa(dsa);
	dsa->start = start;
	dsa->end = start + len;
	dsa->type = type;

	dsa->swapout_win = 4;
	return dsa;
}

inline void free_dsa(struct ds_area_struct *dsa, bool free_cache)
{
	if (atomic_read(&dsa->vaddr_cnt)) {
		pr_err("YIFAN: %s:%d dsa 0x%lx[%ld], vaddrs %d", __func__,
		       __LINE__, dsa->start, dsa->end - dsa->start,
		       atomic_read(&dsa->vaddr_cnt));
		free_vaddrs(&dsa->vaddr_list);
		atomic_set(&dsa->vaddr_cnt, 0);
	}
	if (free_cache)
		kmem_cache_free(dsa_cachep, dsa);
}

void free_dsas(struct list_head *dsa_list)
{
	struct list_head *pos, *tmp;
	list_for_each_safe (pos, tmp, dsa_list) {
		struct ds_area_struct *dsa =
			container_of(pos, struct ds_area_struct, node);
		list_del(pos);
		free_dsa(dsa, true);
	}
}

void insert_dsa(unsigned long ds_start, unsigned long ds_len, bool thd_local,
		enum ds_area_type type)
{
	struct task_struct *thd = current;
	struct ds_area_struct *dsa;
	unsigned cnt;

	if (!hmt_ctl_flag(HMT_DATAPATH))
		return;

	if (thd_local) {
		dsa = search_dsa_list(&thd->dsa_list, &thd->dsl_lock, ds_start);
	} else {
		dsa = search_dsa_list(&thd->mm->dsa_list, &thd->mm->dsl_lock,
				      ds_start);
	}
	if (dsa) // DSA has already been registered
		return;

	dsa = create_dsa(ds_start, ds_len, type);
	if (!dsa)
		return;

	dsa_print(&thd->dsa);

	if (thd_local) { // add to thread-private list
		cnt = atomic_inc_return(&thd->dsa_cnt);
		spin_lock(&thd->dsl_lock);
		list_add(&dsa->node, &thd->dsa_list);
		spin_unlock(&thd->dsl_lock);
		pr_err("YIFAN: %s succeed! dsa 0x%lx[%lu], type %d", __func__,
		       dsa->start, dsa->end - dsa->start, dsa->type);
	} else { // add to global list in mm_struct
		if (!thd->mm)
			return;
		atomic_inc(&thd->mm->dsa_cnt);
		spin_lock(&thd->mm->dsl_lock);
		list_add(&dsa->node, &thd->mm->dsa_list);
		spin_unlock(&thd->mm->dsl_lock);
		pr_err("YIFAN: %s succeed! dsa 0x%lx[%lu], type %d", __func__,
		       dsa->start, dsa->end - dsa->start, dsa->type);
	}
}

void remove_dsa(unsigned long ds_start, bool thd_local)
{
	struct task_struct *thd = current;
	struct ds_area_struct *pos, *tmp;

	if (!hmt_ctl_flag(HMT_DATAPATH))
		return;

	if (thd_local) {
		list_for_each_entry_safe (pos, tmp, &thd->dsa_list, node) {
			if (pos->start == ds_start) {
				atomic_dec(&thd->dsa_cnt);
				spin_lock(&thd->dsl_lock);
				list_del(&pos->node);
				spin_unlock(&thd->dsl_lock);
				pr_err("YIFAN: %s succeed! dsa 0x%lx[%lu], type %d",
				       __func__, pos->start,
				       pos->end - pos->start, pos->type);
				free_dsa(pos, true);
				break;
			}
		}
	} else {
		if (!thd->mm)
			return;
		list_for_each_entry_safe (pos, tmp, &thd->mm->dsa_list, node) {
			if (pos->start == ds_start) {
				spin_lock(&thd->mm->dsl_lock);
				atomic_dec(&thd->mm->dsa_cnt);
				list_del(&pos->node);
				pr_err("YIFAN: %s succeed! dsa 0x%lx[%lu], type %d, vaddrs %d",
				       __func__, pos->start,
				       pos->end - pos->start, pos->type,
				       atomic_read(&pos->vaddr_cnt));
				spin_unlock(&thd->mm->dsl_lock);
				free_dsa(pos, true);
				break;
			}
		}
	}
}

struct ds_area_struct *search_dsa_list(struct list_head *dsa_list,
				       spinlock_t *lock, unsigned long vaddr)
{
	struct ds_area_struct *ret = NULL;
	struct ds_area_struct *pos, *tmp;
	list_for_each_entry_safe (pos, tmp, dsa_list, node) {
		if (in_dsa(vaddr, pos)) {
			ret = pos;
			break;
		}
	}
	/* move ret to the head if it is not. maintain the LRU order for the
	 * list.
	 */
	if (ret && &ret->node != dsa_list->next) {
		spin_lock(lock);
		list_del(&ret->node);
		list_add(&ret->node, dsa_list);
		spin_unlock(lock);
	}
	return ret;
}

/*
 * maintain DSA stats and vaddr list.
 */
void dsa_add_vaddr(struct ds_area_struct *dsa, bool need_lock,
		   struct vm_area_struct *vma, unsigned long addr)
{
	atomic_inc(&dsa->swpin_cnt);
	if (dsa->type == DSA_RANDOM) {
		// do nothing for now
		return;
	} else if (dsa->type == DSA_STREAMING || dsa->type == DSA_PARFOR) {
		struct vaddr *vaddr = create_vaddr(vma, addr);
		if (!vaddr)
			return;
		atomic_inc(&dsa->vaddr_cnt);
		if (need_lock)
			spin_lock(&dsa->val_lock);
		list_add_tail(&vaddr->node, &dsa->vaddr_list);
		if (need_lock)
			spin_unlock(&dsa->val_lock);
	} else if (dsa->type == DSA_NON_DSA) { // thread private dsa
		struct vaddr *vaddr = create_vaddr(vma, addr);
		int cnt;
		if (!vaddr)
			return;
		cnt = atomic_inc_return(&dsa->vaddr_cnt);
		if (cnt > 1)
			list_add_tail(&vaddr->node, &dsa->vaddr_list);
		else {
			spin_lock(&dsa->val_lock);
			list_add_tail(&vaddr->node, &dsa->vaddr_list);
			spin_unlock(&dsa->val_lock);
		}
	}
}

int dsa_isolate_vaddrs(struct ds_area_struct *dsa, bool need_lock,
		       struct list_head *va_list, int nr_to_isolate,
		       unsigned long threshold)
{
	// int vaddr_cnt;
	if (dsa->type == DSA_RANDOM) {
		// do nothing for now
		return 0;
	}

	if (dsa_avg_refault_dist(dsa) < threshold) {
		dsa->swapout_win = nr_to_isolate = max(4, dsa->swapout_win / 2);
		return 0;
	} else {
		dsa->swapout_win = nr_to_isolate =
			min(max(4, dsa->swapout_win * 2), 64);
	}
	// vaddr_cnt = atomic_read(&pos->vaddr_cnt);
	// nr_to_isolate = min(max(32, vaddr_cnt / 8), 128);

	if (dsa->type == DSA_STREAMING || dsa->type == DSA_PARFOR) {
		int cnt = 0;
		struct list_head *pos, *tmp;

		if (need_lock)
			spin_lock(&dsa->val_lock);
		list_for_each_safe(pos, tmp, &dsa->vaddr_list) {
			list_move_tail(pos, va_list);
			cnt++;
			if (cnt >= nr_to_isolate)
				break;
		}
		if (need_lock)
			spin_unlock(&dsa->val_lock);
		// if (cnt) {
		// 	pr_err("YIFAN: %s:%d dsa %p[%ld] %d, isolate %d/%d",
		// 	       __func__, __LINE__, dsa, dsa->end - dsa->start,
		// 	       dsa->type, cnt, atomic_read(&dsa->vaddr_cnt));
		// }
		atomic_add(-cnt, &dsa->vaddr_cnt);
		atomic_add(cnt, &dsa->swpout_cnt);

		// dsa_print(dsa);
		// dsa_reset_rft_dist(dsa);

		return cnt;
	} else if (dsa->type == DSA_NON_DSA) {
		int cnt = 0;
		struct list_head *pos, *tmp;

		if (need_lock)
			spin_lock(&dsa->val_lock);
		list_for_each_safe(pos, tmp, &dsa->vaddr_list) {
			list_move_tail(pos, va_list);
			cnt++;
			if (cnt >= nr_to_isolate)
				break;
		}
		if (need_lock)
			spin_unlock(&dsa->val_lock);
		// if (cnt) {
		// 	pr_err("YIFAN: %s:%d dsa %p[%ld] %d, isolate %d/%d",
		// 	       __func__, __LINE__, dsa, dsa->end - dsa->start,
		// 	       dsa->type, cnt, atomic_read(&dsa->vaddr_cnt));
		// }
		atomic_add(-cnt, &dsa->vaddr_cnt);
		atomic_add(cnt, &dsa->swpout_cnt);

		return cnt;
	}
	return 0;
}

inline void dsa_print(struct ds_area_struct *dsa)
{
	unsigned long avg_rft_dist = dsa_avg_refault_dist(dsa);
	unsigned ft_cnt = atomic_read(&dsa->fault_cnt);
	pr_err("YIFAN: dsa 0x%lx[%ld], type %d, vaddrs %d, avg refault dist: %ld, fault cnt %d, in|out %d|%d",
	       dsa->start, dsa->end - dsa->start, dsa->type,
	       atomic_read(&dsa->vaddr_cnt), avg_rft_dist, ft_cnt,
	       atomic_read(&dsa->swpin_cnt), atomic_read(&dsa->swpout_cnt));
}

inline void dsa_print_rft_dist(struct ds_area_struct *dsa)
{
	unsigned long avg_rft_dist = dsa_avg_refault_dist(dsa);
	unsigned ft_cnt = atomic_read(&dsa->fault_cnt);
	pr_err("YIFAN: dsa 0x%lx[%ld] avg refault dist: %ld, fault cnt %d, in|out %d|%d",
	       dsa->start, dsa->end - dsa->start, avg_rft_dist, ft_cnt,
	       atomic_read(&dsa->swpin_cnt), atomic_read(&dsa->swpout_cnt));
}

inline void dsa_reset_rft_dist(struct ds_area_struct *dsa)
{
	atomic_set(&dsa->swpin_cnt, 0);
	atomic_set(&dsa->swpout_cnt, 0);
	atomic_set(&dsa->fault_cnt, 0);
	atomic64_set(&dsa->refault_dist, 0);
}

inline void dsa_update_rft_dist(struct ds_area_struct *dsa,
			     unsigned long refault_dist)
{
	atomic64_add_return(refault_dist, &dsa->refault_dist);
	atomic_inc_return(&dsa->fault_cnt);
	// dsa_print_rft_dist(dsa);
}

inline void reset_all_rft_dist(struct task_struct *cthd)
{
	struct ds_area_struct *pos, *tmp;
	spin_lock(&cthd->dsl_lock);
	list_for_each_entry_safe (pos, tmp, &cthd->dsa_list, node) {
		dsa_reset_rft_dist(pos);
	}
	spin_unlock(&cthd->dsl_lock);
	dsa_reset_rft_dist(&cthd->dsa);
}

/*
 * async prefetch threads
 */
bool pref_request_queue_initialized = false;

inline void pref_request_copy(struct pref_request *dst,
			      struct pref_request *src)
{
	memcpy(dst, src, sizeof(struct pref_request));
}

static inline void rewind_inc(int *cnt, int mod)
{
	WRITE_ONCE(*cnt, (*cnt + 1) % mod);
}

int init_pref_request_queues(void)
{
	int ret = 0;
	if (pref_request_queue_initialized)
		return 0;

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "swap_slots_cache",
				pref_request_queue_init,
				pref_request_queue_destroy);
	if (WARN_ONCE(ret < 0, "Pref_request_queue allocation failed (%s)!\n",
		      __func__))
		return -ENOMEM;
	pref_request_queue_initialized = true;
	pr_err("YIFAN: pref_request_queues initialized!\n");

	hermit_pthd_run();
	// hermit_dpthd_run();
	pr_err("YIFAN: launched hermit_pthds!\n");

	return ret;
}
// __initcall(init_pref_request_queues);

int pref_request_queue_init(unsigned int cpu)
{
	struct pref_request_queue *pr_q = &per_cpu(pref_request_queue, cpu);
	// invariant: (head + cnt) % size == tail
	atomic_set(&pr_q->cnt, 0);
	pr_q->size = PREF_REQUEST_QUEUE_SIZE;
	pr_q->head = 0;
	pr_q->tail = 0;

	spin_lock_init(&pr_q->lock);
	pr_q->reqs = (struct pref_request *)kmalloc(
		sizeof(struct pref_request) * pr_q->size, GFP_KERNEL);
	if (!pr_q->reqs)
		return -ENOMEM;

	return 0;
}

int pref_request_queue_destroy(unsigned int cpu)
{
	struct pref_request_queue *pr_q = &per_cpu(pref_request_queue, cpu);
	if (pr_q->reqs)
		kfree(pr_q->reqs);
	pr_q->reqs = NULL;

	atomic_set(&pr_q->cnt, 0);
	pr_q->size = 0;
	pr_q->head = 0;
	pr_q->tail = 0;

	return 0;
}

int pref_request_enqueue(struct pref_request *pr)
{
	int cnt;
	struct pref_request_queue *pr_q;
	if (!pr)
		return -EINVAL;

	if (pr->stt < pr->ra_info.nr_pte)
		return -EEXIST;

	pr_q = this_cpu_ptr(&pref_request_queue);
	cnt = atomic_read(&pr_q->cnt);
	if (cnt == pr_q->size) { // slow path
		pr_err("YIFAN: core %d pref_queue full!\n", smp_processor_id());
		return -ENOMEM;
	}

	// fast path. No contention if head != tail
	pref_request_copy(&pr_q->reqs[pr_q->tail], pr);
	rewind_inc(&pr_q->tail, pr_q->size);
	atomic_inc(&pr_q->cnt);

	// pr_err("%s: pr_q[%d] cnt %d\n", __func__, smp_processor_id(), cnt);

	return 0;
}

int pref_request_queue_dequeue(struct pref_request *pr, int core)
{
	int cnt;
	struct pref_request_queue *pr_q = &per_cpu(pref_request_queue, core);
	cnt = atomic_read(&pr_q->cnt);
	if (cnt == 0) {
		return -ENOENT;
	}
	// pr_err("%s: pr_q[%d] cnt %d\n", __func__, smp_processor_id(), cnt);
	if (cnt < pr_q->size) { // fast path
		pref_request_copy(pr, &pr_q->reqs[pr_q->head]);
		rewind_inc(&pr_q->head, pr_q->size);
		atomic_dec(&pr_q->cnt);
		return 0;
	} else { // slow path. cnt == size here.
		// spin_lock_irq(&pr_q->lock);
		pref_request_copy(pr, &pr_q->reqs[pr_q->head]);
		rewind_inc(&pr_q->head, pr_q->size);
		// spin_unlock_irq(&pr_q->lock);
		atomic_dec(&pr_q->cnt);
		return 0;
	}

	return -EINVAL;
}

static inline bool hmt_pthd_waken(void)
{
	return atomic_read(&(this_cpu_ptr(&pref_request_queue)->cnt));
}

static inline void print_preq_req(struct pref_request *pr)
{
	pr_err("vma: 0x%lx, faddr: 0x%lx,\n"
	       "ra_info: .win = %d, .nr_pte = %d, .ptes = 0x%lx, offset = %d\n",
	       (unsigned long)pr->vma, pr->faddr, pr->ra_info.win, pr->ra_info.nr_pte,
	       (unsigned long)pr->ra_info.ptes, pr->ra_info.offset);
}

int hermit_pthread(void *p)
{
	int log_cnt = 0;
	int total_cnt = 0;

	pr_err("hermit_pthd-%d start!\n", smp_processor_id());

	while (!hmt_ctl_flag(HMT_PTHD_STOP)) {
		int ret = 0;
		struct pref_request pr;
		int cnt = 0;

		if (!hmt_pthd_waken()) {
			usleep_range(10, 100);
			// cond_resched();
			continue;
		}

		while (true) {
			int nr_prefed = 0;
			ret = pref_request_queue_dequeue(&pr,
							 smp_processor_id());
			if (ret)
				break;
			// if (smp_processor_id() == 1)
			// 	print_preq_req(&pr);
			nr_prefed += hermit_vma_prefetch(&pr, -1, NULL, NULL);
			cnt += nr_prefed;
			// if (smp_processor_id() == 1)
			// 	pr_err("YIFAN: prefetched %d pages\n",
			// 	       nr_prefed);
		}

		if (cnt) {
			total_cnt += cnt;
			log_cnt++;
		}

		if (false && log_cnt == 100) {
			pr_err("hmt_pthd-%d, avg pref cnt %d\n",
			       smp_processor_id(), total_cnt / 1000);
			log_cnt = 0;
			total_cnt = 0;
		}
	}

	pr_err("hermit_pthd-%d exit!\n", smp_processor_id());
	return 0;
}

int hermit_dpthread(void *p)
{
	int log_cnt = 0;
	int total_cnt = 0;

	int tid = (int)p;

	pr_err("hermit_dpthd-%d start!\n", smp_processor_id());

	while (!hmt_ctl_flag(HMT_PTHD_STOP)) {
		int ret = 0;
		struct pref_request pr;
		int i;
		int cnt = 0;

		for (i = tid; i < 96; i += 4) {
			int nr_prefed = 0;
			if (!(i < 16 || (i >= RMGRID_NR_PCORE &&
					 i < RMGRID_NR_PCORE + 16)))
				continue;
			ret = pref_request_queue_dequeue(&pr, i);
			if (!ret)
				continue;
			// if (smp_processor_id() == 1)
			// 	print_preq_req(&pr);
			nr_prefed += hermit_vma_prefetch(&pr, -1, NULL, NULL);
			cnt += nr_prefed;
		}

		if (cnt) {
			total_cnt += cnt;
			log_cnt++;
			adc_counter_add(cnt, ADC_HERMIT_RECLAIM);
		}
		cond_resched();
	}

	pr_err("hermit_dpthd-%d exit!\n", smp_processor_id());
	return 0;
}

#define HMT_NR_PTHDS 32
static struct task_struct *pthds[HMT_NR_PTHDS];

void hermit_pthd_run(void)
{
	int i;

	for (i = 0; i < HMT_NR_PTHDS; i++) {
		int core = i < 16 ? i : RMGRID_NR_PCORE + i - 16;
		struct task_struct *pthd = kthread_create(
			hermit_pthread, NULL, "hermit_pthd-%d", core);
		if (IS_ERR(pthd)) {
			pr_err("Failed to start hermit_pthd-%d\n", core);
			continue;
		}

		kthread_bind(pthd, core);
		wake_up_process(pthd);
		pthds[i] = pthd;
	}
}

void hermit_dpthd_run(void)
{
	int core = 16;
	int i;
	for (i = 0; i < 4; i++) {
		struct task_struct *dpthd = kthread_create(
			hermit_dpthread, (void *)i, "hermit-dpthd-%d", core + i);
		if (IS_ERR(dpthd)) {
			pr_err("Failed to start hermit_pthd-%d\n", core + i);
			continue;
		}
		kthread_bind(dpthd, core + i);
		wake_up_process(dpthd);
		pthds[i] = dpthd;

	}
}

void hermit_pthd_stop(void)
{
	int i;
	for (i = 0; i < HMT_NR_PTHDS; i++) {
		if (pthds[i])
			kthread_stop(pthds[i]);
	}
}

/*
 * utils
 */
unsigned long thd_workingset_size(struct task_struct *thd)
{
	unsigned long workingset_size = 0;
	struct mem_cgroup *memcg = mem_cgroup_from_task(thd);
	int nid;

	if (!memcg)
		return 0;

	for_each_node_state (nid, N_MEMORY) {
		struct pglist_data *pgdat;
		struct lruvec *lruvec;
		unsigned long lruvec_ws_size = 0;
		pgdat = NODE_DATA(nid);
		if (!pgdat)
			continue;
		lruvec = mem_cgroup_lruvec(memcg, pgdat);
		if (!lruvec)
			continue;
		lruvec_ws_size += lruvec_page_state(lruvec, NR_ACTIVE_ANON);
		lruvec_ws_size += lruvec_page_state(lruvec, NR_INACTIVE_ANON);
		if (lruvec_ws_size > workingset_size)
			workingset_size = lruvec_ws_size;
	}
	return workingset_size;
}

int hermit_page_referenced(struct vpage *vpage, struct page *page,
			   int is_locked, struct mem_cgroup *memcg,
			   unsigned long *vm_flags)
{
	int we_locked = 0;
	int referenced = 0;

	unsigned long address = vpage->address;
	struct vm_area_struct *vma = vpage->vma;
	pte_t *pte = vpage->pte;

	*vm_flags = 0;
	BUG_ON(!PageAnon(page));
	BUG_ON(!page_rmapping(page));
	// BUG_ON(total_mapcount(page) != 1);
	if (total_mapcount(page) > 1) {
		pr_err("%s:%d total_mapcount > 1!\n", __func__, __LINE__);
		return page_referenced(page, is_locked, memcg, vm_flags);
	}

	if (!is_locked && (!PageAnon(page) || PageKsm(page))) {
		we_locked = trylock_page(page);
		if (!we_locked)
			return 1;
	}

	if (vma->vm_flags & VM_LOCKED) {
		// page_vma_mapped_walk_done(&pvmw);
		if (pte && !PageHuge(page))
			pte_unmap(pte);
		*vm_flags |= VM_LOCKED;
		goto walk_done;
	}

	// page_referenced_one
	if (pte) {
		if (ptep_clear_flush_young_notify(vma, address, pte)) {
			/*
			 * Don't treat a reference through
			 * a sequentially read mapping as such.
			 * If the page has been used in another mapping,
			 * we will catch it; if this other mapping is
			 * already gone, the unmap path will have set
			 * PG_referenced or activated the page.
			 */
			if (likely(!(vma->vm_flags & VM_SEQ_READ)))
				referenced++;
		}
	}
	// YIFAN: shouldn't get THP
	/* else if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)) {
		if (pmdp_clear_flush_young_notify(vma, address, pvmw.pmd))
			referenced++;
	} */
	else {
		/* unexpected pmd-mapped page? */
		WARN_ON_ONCE(1);
	}

walk_done:
	if (referenced)
		clear_page_idle(page);
	if (test_and_clear_page_young(page))
		referenced++;

	if (referenced) {
		*vm_flags |= vma->vm_flags;
	}

	return referenced;
}

void hermit_dispatch_pref_work(struct pref_request * pref_req){
	unsigned short stt = pref_req->stt;
	unsigned short nr_pte = pref_req->ra_info.nr_pte;
	struct hermit_pref_work * pref_work;
	u64 pf_ts;
	work_func_t func;
	if(stt >= nr_pte)
		return;

	pf_ts = get_cycles_start();

	pref_work = kmem_cache_alloc(hermit_pref_work_cache, GFP_ATOMIC);
	pref_work->vma = pref_req->vma;
	pref_work->faddr = pref_req->faddr;
	pref_work->stt = stt;
	pref_work->nr_pte = nr_pte;
	pref_work->offset = pref_req->ra_info.offset;
	pref_work->ptes = pref_req->ra_info.ptes;
	if(hmt_ctl_flag(HMT_PREF_DIRECT_POLL) && hmt_ctl_flag(HMT_PREF_DIRECT_MAP))
		func = hermit_vma_prefetch_direct_poll_direct_map_work;
	else if(hmt_ctl_flag(HMT_PREF_DIRECT_POLL))
		func = hermit_vma_prefetch_direct_poll_work;
	else
		func = hermit_vma_prefetch_work;
	INIT_WORK(&pref_work->work, func);
	queue_work(hermit_pref_wq, &pref_work->work);

	pf_ts = get_cycles_end() - pf_ts;
	accum_adc_time_stat(ADC_ASYNC_PREF_OVERHEAD, pf_ts);
}

struct page* hmt_sc_load_get(pgoff_t idx){
	struct page *page = NULL;
	if(idx >= RMGRID_MAX_SWAP / PAGE_SIZE){
		return NULL;
	}
	rcu_read_lock();
repeat:
	page = hmt_sc_load(idx);
	if(!page)
		goto out;
	if (!page_cache_get_speculative(page))
		goto repeat;
	/*
	 * Has the page moved or been split?
	 * This is part of the lockless pagecache protocol. See
	 * include/linux/pagemap.h for details.
	 */
	if (unlikely(page != hmt_sc_load(idx))) {
		put_page(page);
		goto repeat;
	}

out:
	rcu_read_unlock();
	return page;
}
