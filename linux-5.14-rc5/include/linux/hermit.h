/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_H
#define _LINUX_HERMIT_H
/*
 * Declarations for Hermit functions in mm/hermit.c
 */

#include <linux/rmap.h>
#include <linux/hermit_types.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/*
 * global variables for profile and control
 */
enum hmt_ctl_flag_type {
	HMT_BPS_SCACHE,
	HMT_DATAPATH,
	HMT_SWP_THD,
	HMT_PREF_THD,
	HMT_PTHD_STOP,
	HMT_DSA_PRFCH,
	HMT_BATCH_OUT,
	HMT_BATCH_TLB,
	HMT_BATCH_IO,
	HMT_BATCH_ACCOUNT,
	HMT_VADDR_OUT,
	HMT_SPEC_IO,
	HMT_SPEC_LOCK,
	HMT_LAZY_POLL,
	HMT_PREFETCH_EARLY_MAP,
	HMT_ASYNC_PREFETCH,
	HMT_PREF_BATCH_CHARGE,
	HMT_PREF_DIRECT_POLL,
	HMT_PREF_DIRECT_MAP,
	HMT_PREF_ALWYS_ASCEND,
	HMT_PREF_LAZY_UMD,
	HMT_PREF_POPULATE,
	ATL_CARD_PROF,
	ATL_CARD_PROF_PRINT,
	NUM_HMT_CTL_FLAGS
};

enum hmt_ctl_var_type {
	HMT_STHD_CNT,
	HMT_STHD_SLEEP_DUE,
	HMT_RECLAIM_MODE,
	HMT_PPLT_WORK_CNT,
	HMT_MIN_STHD_CNT,
	ATL_CARD_PROF_THRES,
	ATL_CARD_PROF_LOW_THRES,
	NUM_HMT_CTL_VARS
};

extern bool hmt_ctl_flags[NUM_HMT_CTL_FLAGS];
extern unsigned hmt_ctl_vars[NUM_HMT_CTL_VARS];

extern uint32_t hmt_sthd_cores[];

static inline bool hmt_ctl_flag(enum hmt_ctl_flag_type type)
{
	return READ_ONCE(hmt_ctl_flags[type]);
}

static inline unsigned hmt_ctl_var(enum hmt_ctl_var_type type)
{
	return READ_ONCE(hmt_ctl_vars[type]);
}

static inline void hermit_set_sthd_cores(uint32_t *buf, unsigned long buf_len)
{
	int num_elems = buf_len / sizeof(uint32_t);
	int i;
	pr_err("%s:%d %d, %lu\n", __func__, __LINE__, num_elems, buf_len);
	for (i = 0; i < num_elems; i++)
		hmt_sthd_cores[i] = buf[i];

	printk("set sthd_cores to:\n");
	for (i = 0; i < 96; i++)
		printk("%d", hmt_sthd_cores[i]);
}

/*
 * [Hermit] vaddr field for reverse mapping
 */
// vaddr array matches page struct array for phy pages
extern unsigned long *hermit_page_vaddrs;

static inline unsigned long hmt_get_page_vaddr(struct page *page) {
	if (!hermit_page_vaddrs)
		return 0;
	return hermit_page_vaddrs[page_to_pfn(page)];
}

static inline void hmt_set_page_vaddr(struct page *page, unsigned long vaddr) {
	if (!hermit_page_vaddrs)
		return;
	hermit_page_vaddrs[page_to_pfn(page)] = vaddr;
}

extern struct page **hermit_swapcache;

// # maximum swap size supported in bytes. For now support 128GB at most.
#define RMGRID_MAX_SWAP (128UL * 1024 * 1024 * 1024)

static inline struct page* hmt_sc_load(pgoff_t idx){
	return (struct page*)atomic64_read((atomic64_t*)&hermit_swapcache[idx]);
}

struct page* hmt_sc_load_get(pgoff_t idx);

static inline void hmt_sc_store(pgoff_t idx, struct page* page){
	BUG_ON(idx >= RMGRID_MAX_SWAP / PAGE_SIZE);
	atomic64_set((atomic64_t*)&hermit_swapcache[idx], (long)page);
}

/*
 * [Hermit] per-thread virtual address list for swapping
 */
struct vaddr {
	// struct vm_area_struct *vma;
	unsigned long address;
	struct list_head node;
};

struct vpage {
	struct vm_area_struct *vma;
	unsigned long address;
	pte_t *pte;
	spinlock_t *ptl;
	struct page *page;
	struct list_head node;
};

/*
 * interfaces
 */
void page_add_vaddr(pte_t *pte, struct page *page, struct vm_area_struct *vma,
		    unsigned long);

void hermit_record_refault_dist(unsigned long addr, unsigned long refault_dist);

/*
 * manage vaddr
 */
struct page *vaddr2page(struct vaddr *vaddr);
int scan_vaddr_list(void *p, struct list_head *vaddr_list,
		    struct list_head *page_list);
void free_vaddrs(struct list_head *vaddr_list);

/*
 * manage vpage
 */
struct vpage *create_vpage(struct vm_area_struct *vma, unsigned long address,
			   struct page *page, pte_t *pte);
struct vpage *vaddr2vpage(struct vaddr *vaddr);
int isolate_vaddrs(struct task_struct *cthd, struct list_head *vaddr_list,
		   int nr_to_isolate, unsigned long threshold);
int isolate_vpages(struct task_struct *cthd, struct list_head *vpage_list,
		   int nr_to_isolate);
void free_vpage(struct vpage *vpage);
void free_vpages(struct list_head *vpage_list);

/*
 * manage dsa
 */
static inline void init_dsa(struct ds_area_struct *dsa)
{
	if (!dsa)
		return;
	dsa->start = 0;
	dsa->end = 0;
	dsa->type = DSA_NON_DSA;
	atomic_set(&dsa->vaddr_cnt, 0);
	INIT_LIST_HEAD(&dsa->vaddr_list);
	spin_lock_init(&dsa->val_lock);
	atomic_set(&dsa->swpin_cnt, 0);
	atomic_set(&dsa->swpout_cnt, 0);
	atomic_set(&dsa->fault_cnt, 0);
	atomic64_set(&dsa->refault_dist, 0);
}

struct ds_area_struct *create_dsa(unsigned long start, unsigned long len,
				  enum ds_area_type type);
void insert_dsa(unsigned long ds_start, unsigned long ds_len, bool thd_local,
		enum ds_area_type type);
void remove_dsa(unsigned long ds_start, bool thd_local);
void free_dsa(struct ds_area_struct *dsa, bool free_cache);
void free_dsas(struct list_head *dsa_list);
struct ds_area_struct *search_dsa_list(struct list_head *dsa_list,
				       spinlock_t *lock, unsigned long vaddr);
struct ds_area_struct *hermit_find_dsa(unsigned long vaddr);

void dsa_add_vaddr(struct ds_area_struct *dsa, bool need_lock,
		   struct vm_area_struct *vma, unsigned long addr);
int dsa_isolate_vaddrs(struct ds_area_struct *dsa, bool need_lock,
		       struct list_head *va_list, int nr_to_isolate,
		       unsigned long threshold);
void dsa_reset_rft_dist(struct ds_area_struct *dsa);
void reset_all_rft_dist(struct task_struct *cthd);
void dsa_update_rft_dist(struct ds_area_struct *dsa,
			 unsigned long refault_dist);

void dsa_print(struct ds_area_struct *dsa);
void dsa_print_rft_dist(struct ds_area_struct *dsa);

static inline bool in_dsa(unsigned long addr, struct ds_area_struct *dsa)
{
	return addr >= dsa->start && addr < dsa->end;
}

static inline unsigned long dsa_avg_refault_dist(struct ds_area_struct *dsa)
{
	unsigned long avg_rft_dist;
	unsigned ft_cnt;
	avg_rft_dist = atomic64_read(&dsa->refault_dist);
	ft_cnt = atomic_read(&dsa->fault_cnt);
	avg_rft_dist = ft_cnt ? avg_rft_dist / ft_cnt : 0;
	return avg_rft_dist;
}

/*
 * async prefetch thread
 */
struct pref_request_queue {
	struct pref_request *reqs;
	atomic_t cnt;
	unsigned size;
	unsigned head;
	unsigned tail;

	spinlock_t lock;
};
#define PREF_REQUEST_QUEUE_SIZE 4096
// per-cpu variable defined & initialized in hermit.c
extern bool pref_request_queue_initialized;

int pref_request_queue_init(unsigned int cpu);
int pref_request_queue_destroy(unsigned int cpu);
int pref_request_enqueue(struct pref_request *pr);
int pref_request_dequeue(struct pref_request *pr);

void hermit_pthd_run(void);
void hermit_dpthd_run(void);
void hermit_pthd_stop(void);

extern void pref_request_copy(struct pref_request *src,
			      struct pref_request *dst);

/*
 * utils
 */
unsigned long thd_workingset_size(struct task_struct *thd);

int hermit_page_referenced(struct vpage *vpage, struct page *page,
			   int is_locked, struct mem_cgroup *memcg,
			   unsigned long *vm_flags);
// implemented in rmap.c & pagewalk.c
void hermit_try_to_unmap(struct vpage *vpage, struct page *page,
			 enum ttu_flags flags);

bool hermit_addr_vma_walk(struct page_vma_mapped_walk *pvmw, bool force_lock);
bool hermit_addr_vma_walk_nolock(struct page_vma_mapped_walk *pvmw);

// debug

// YIFAN: hard-coded thread names for different applications.
static inline bool is_cassandra_thd(const char *str)
{
	return strncmp(str, "Native-Transpor", 14) == 0 ||
	       strncmp(str, "ReadStage-", 10) == 0 ||
	       strncmp(str, "MutationStage-", 13) == 0;
}

static inline bool is_spark_thd(const char *str)
{
	return strncmp(str, "Executor ", 9) == 0;
}

static inline bool is_gc_thd(const char *str)
{
	return strncmp(str, "G1 Conc", 7) == 0 ||
	       strncmp(str, "GC Thread#", 10) == 0;
	//        strncmp(str, "java", 4) == 0 ||
	//        strncmp(str, "G1 Refine", 9) == 0 ||
}

static inline bool is_hermit_app(const char *str)
{
	return strncmp(str, "xlinpack", 8) == 0 ||
	       strncmp(str, "xgboost", 7) == 0 ||
	       strncmp(str, "baseline", 8) == 0 ||
	       //        strncmp(str, "python3", 7) == 0 ||
	       strncmp(str, "memcached", 9) == 0 ||
	       //        strncmp(str, "dataframe", 9) == 0 ||
	       //        strncmp(str, "quicksort", 9) == 0 ||
	       //        strncmp(str, "java", 4) == 0 ||
	       is_cassandra_thd(str) ||
	       is_spark_thd(str);
}

static inline bool is_specable_thd(const char *str)
{
	return strncmp(str, "silotpcc-she", 12) == 0 ||
	       is_gc_thd(str) ||
	       is_cassandra_thd(str) ||
	       is_spark_thd(str);
}

/*
 * debug
 */
void hermit_dbg(int type, void *buf, unsigned long buf_len);

struct hermit_populate_work{
	struct work_struct work;
	struct vm_area_struct *vma;
	unsigned long start;
	unsigned long end;
	int nr_work;
	int id;
};

struct hermit_pref_work {
	struct work_struct work;
	struct vm_area_struct *vma;
	unsigned long faddr;
	unsigned short stt;
	unsigned short nr_pte;
	unsigned short offset;
	pte_t *ptes;
};

struct hermit_umd_work {
	struct work_struct work;
	struct vm_area_struct *vma;
	struct page *page;
	unsigned long address;
	spinlock_t *ptl;
	swp_entry_t entry;
};

extern struct workqueue_struct * hermit_ub_wq;
extern struct kmem_cache* hermit_work_cache;

extern struct workqueue_struct * hermit_pref_wq;
extern struct kmem_cache* hermit_pref_work_cache;

extern struct workqueue_struct * hermit_umd_wq;
extern struct kmem_cache *hermit_umd_work_cache;

void hermit_dispatch_pref_work(struct pref_request * pref_req);

#endif // _LINUX_HERMIT_H
