/*
 * mm/hermit_utils.c - hermit utils functions
 */
#include <linux/mm_types.h>
#include <linux/hermit_types.h>
#include <linux/hermit_utils.h>
#include <linux/hermit.h>
#include <linux/swap.h>
#include <linux/swap_stats.h>
#include <linux/mm.h>

#include "internal.h"

/***
 * async swapout inline utils
 */
static inline uint64_t hmt_calc_throughput(uint64_t nr_pgs, uint64_t dur)
{
	return nr_pgs * 1000 * 1000 * RMGRID_CPU_FREQ / dur; // #(pgs)/s
}

/***
 * async swapout dynamic control utilities
 */
inline unsigned hmt_get_sthd_cnt(struct mem_cgroup *memcg,
				 struct hmt_swap_ctrl *sc)
{
	int MAX_THD_CNT = hmt_ctl_vars[HMT_STHD_CNT];
	uint64_t mem_limit = READ_ONCE(memcg->memory.max);
	uint64_t nr_avail_pgs = mem_limit -
				page_counter_read(&memcg->memory);

	// 0 for our adaptive scheduling. 1 for social network, 2 for memcached
	unsigned mode = hmt_ctl_vars[HMT_RECLAIM_MODE];
	if (mode == 0 && sc->swin_thrghpt && sc->swout_thrghpt) {
		int max_thd_cnt, min_thd_cnt, thd_cnt;
		max_thd_cnt = min(MAX_THD_CNT,
				  (int)(sc->swin_thrghpt / sc->swout_thrghpt));

		min_thd_cnt = 0;
		if (nr_avail_pgs / 128 < max_thd_cnt * 16)
			min_thd_cnt = max(hmt_ctl_vars[HMT_MIN_STHD_CNT], 1U);
		thd_cnt = min(max(max_thd_cnt - (int)(nr_avail_pgs / 128),
				  min_thd_cnt),
			      MAX_THD_CNT);
		return thd_cnt;
	}
	else if (mode == 1) {
		if (nr_avail_pgs < 2048)
			return MAX_THD_CNT;
		else
			return 0;
	}
	/*
	else if (mode == 1) {
		if (nr_avail_pgs > 2 * 78643)
			return 0;
		else if (nr_avail_pgs > 78643)
			return 1;
		else if (nr_avail_pgs > 8192)
			return 2;
		else if (nr_avail_pgs > 2048)
			return 3;
		else if (nr_avail_pgs > 512)
			return 4;
		else if (nr_avail_pgs > 128)
			return 6;
		else
			return 12;
	}
	else if (mode == 2) {
		if (nr_avail_pgs > 8192)
			return 0;
		else if (nr_avail_pgs > 2048)
			return 1;
		else if (nr_avail_pgs > 256)
			return 2;
		else if (nr_avail_pgs > 128)
			return 4;
		else
			return 6;
	}
	*/
	else {
		if (nr_avail_pgs < 2048)
			return 1;
		return 0;
	}
}

// must hold sc->lock
inline void hmt_update_swap_ctrl(struct mem_cgroup *memcg,
				 struct hmt_swap_ctrl *sc)
{
	// update swap-in throughput per UPD_PERIOD us
	const int UPD_PERIOD = 1000;
	if (sc->swin_ts[0] == 0) {
		memset(sc, 0, sizeof(struct hmt_swap_ctrl));
		sc->swin_ts[0] = get_cycles_light();
		sc->nr_pg_charged[0] =
			atomic64_read(&memcg->total_pg_charge);
		return;
	}
	sc->swin_ts[1] = get_cycles_light();
	if (sc->swin_ts[1] - sc->swin_ts[0] < UPD_PERIOD * RMGRID_CPU_FREQ)
		return;

	sc->nr_pg_charged[1] = atomic64_read(&memcg->total_pg_charge);
	sc->swin_thrghpt = max(sc->swin_thrghpt,
		hmt_calc_throughput(sc->nr_pg_charged[1] - sc->nr_pg_charged[0],
				    sc->swin_ts[1] - sc->swin_ts[0]));

	sc->swin_ts[0] = sc->swin_ts[1];
	sc->nr_pg_charged[0] = sc->nr_pg_charged[1];

	sc->log_cnt++;
	// log for debug
	if (false && sc->log_cnt % 10000 == 0 && sc->swin_thrghpt &&
	    sc->swout_dur.avg) {
		uint64_t nr_avail_pgs = 0;
		uint64_t reclaim_time_budget = 0;
		nr_avail_pgs = READ_ONCE(memcg->memory.max) -
			       page_counter_read(&memcg->memory);
		reclaim_time_budget =
			nr_avail_pgs * 1000 * 1000 / sc->swin_thrghpt; // in us
		spin_unlock_irq(&sc->lock);
		pr_err("swin_thrghput: %8llupg/s,"
		       "swout_thrghput: %8llupg/s, "
		       "swout_duration: %8lluus, "
		       "budget %8lluus, %llupgs\n",
		       sc->swin_thrghpt, sc->swout_thrghpt,
		       sc->swout_dur.avg / RMGRID_CPU_FREQ, reclaim_time_budget,
		       nr_avail_pgs);
		spin_lock_irq(&sc->lock);
		sc->log_cnt = 0;
	}
}

static inline void accum_swout_dur(struct hmt_swap_ctrl *sc, uint64_t dur,
				   unsigned nr_reclaimed)
{
	sc->swout_dur.nr_pages += nr_reclaimed;
	sc->swout_dur.total += dur;
	sc->swout_dur.cnt++;
	sc->swout_dur.avg = sc->swout_dur.total / sc->swout_dur.cnt;

	// sc->swout_thrghpt = 100 * 1000;
	sc->swout_thrghpt = hmt_calc_throughput(sc->swout_dur.nr_pages,
						sc->swout_dur.total);
}

static unsigned long hermit_reclaim_high(struct task_struct *cthd,
					 struct hmt_swap_ctrl *sc, bool master,
					 unsigned int nr_pages, gfp_t gfp_mask)
{
	unsigned long total_reclaimed = 0;
	struct mem_cgroup *memcg = mem_cgroup_from_task(cthd);

	do {
#ifdef ADC_PROFILE_PF_BREAKDOWN
		uint64_t pf_breakdown[NUM_ADC_PF_BREAKDOWN_TYPE] = { 0 };
#else
		uint64_t *pf_breakdown = NULL;
#endif
		uint64_t swout_dur;
		unsigned long nr_reclaimed = 0;
		// unsigned long pflags;

		swout_dur = -pf_cycles_start();
		// psi_memstall_enter(&pflags);
		nr_reclaimed = hermit_try_to_free_mem_cgroup_pages(
			memcg, nr_pages, gfp_mask, true, cthd, NULL,
			pf_breakdown);
		total_reclaimed += nr_reclaimed;
		// psi_memstall_leave(&pflags);
		swout_dur += pf_cycles_end();
		adc_pf_breakdown_end(pf_breakdown, ADC_TOTAL_PF, swout_dur);
		accum_adc_pf_breakdown(pf_breakdown, ADC_HMT_OUT_SPF);
		if (master)
			accum_swout_dur(sc, swout_dur, nr_reclaimed);
	} while ((memcg = parent_mem_cgroup(memcg)) &&
		 !mem_cgroup_is_root(memcg));

	return total_reclaimed;
}


static void hermit_high_work_func(struct work_struct *work)
{
	int id = *(int *)(work + 1); // dirty hack to get work id via mm->sthds
	bool master = id == 0;
	struct mm_struct *mm =
		container_of(work, struct mm_struct, sthds[id].work);
	struct hmt_swap_ctrl *sc = &mm->hmt_sc;
	struct mem_cgroup *memcg = NULL;
	struct task_struct *cthd = NULL;

	if (READ_ONCE(sc->stop))
		return;

	rcu_read_lock();
	cthd = rcu_dereference(mm->owner);
	if (cthd)
		memcg = mem_cgroup_from_task(cthd);
	rcu_read_unlock();
	if (!memcg)
		return;
	css_get(&memcg->css);

	atomic_inc(&sc->active_sthd_cnt);
	if (id < hmt_get_sthd_cnt(memcg, sc)) {
		hermit_reclaim_high(cthd, sc, master, MEMCG_CHARGE_BATCH,
				    GFP_KERNEL);
	}
	if (id < hmt_get_sthd_cnt(memcg, sc)) {
		schedule_work_on(hmt_sthd_cores[id], &mm->sthds[id].work);
	}

	css_put(&memcg->css);
	atomic_dec(&sc->active_sthd_cnt);
}

void hermit_init_mm(struct mm_struct *mm)
{
	int i;
	if (!mm)
		return;
	atomic_set(&mm->hmt_sc.sthd_cnt, 0);
	spin_lock_init(&mm->dsl_lock);
	atomic_set(&mm->dsa_cnt, 0);
	INIT_LIST_HEAD(&mm->dsa_list);
	mm->dsa_cache = NULL;

	memset(&mm->hmt_sc, 0, sizeof(mm->hmt_sc));
	mm->hmt_sc.lock = __SPIN_LOCK_UNLOCKED(init_mm.dsl_lock);
	for (i = 0; i < HMT_MAX_NR_STHDS; i++) {
		INIT_WORK(&mm->sthds[i].work, hermit_high_work_func);
		mm->sthds[i].id = i;
	}
	mm->hmt_sc.master_up = true;
	mm->psf = NULL;
	mm->card = NULL;
	mm->closing = false;
}

void hermit_cleanup_mm(struct mm_struct *mm)
{
	if (!mm)
		return;
	spin_lock(&mm->prof_lock);
	mm->closing = true;
	spin_unlock(&mm->prof_lock);
	if (atomic_read(&mm->dsa_cnt)) {
		pr_err("mm %p dsa_cnt %d", mm, atomic_read(&mm->dsa_cnt));
		spin_lock(&mm->dsl_lock);
		free_dsas(&mm->dsa_list);
		spin_unlock(&mm->dsl_lock);
		atomic_set(&mm->dsa_cnt, 0);
	}

	WRITE_ONCE(mm->hmt_sc.stop, true);
	while (atomic_read(&mm->hmt_sc.active_sthd_cnt)) {
		cpu_relax();
		// cond_resched();
	}
}

void hermit_cleanup_thread(struct task_struct *thd)
{
	// cleanup thread local dsas
	if (atomic_read(&thd->dsa_cnt)) {
		pr_err("curr %s thd dsa_cnt %d", thd->comm,
		       atomic_read(&thd->dsa_cnt));
		spin_lock(&thd->dsl_lock);
		free_dsas(&thd->dsa_list);
		spin_unlock(&thd->dsl_lock);
		atomic_set(&thd->dsa_cnt, 0);
	}

	// cleanup the thread private dsa
	free_dsa(&thd->dsa, false);
}

void hermit_faultin_vm_range(struct vm_area_struct *vma,
		unsigned long start, unsigned long end){
	BUG();
}
EXPORT_SYMBOL(hermit_faultin_vm_range);

void hermit_faultin_page(struct vm_area_struct *vma,
		unsigned long addr, struct page * page, pte_t*pte, pte_t orig_pte){
	struct mm_struct *mm = vma->vm_mm;
	vm_fault_t ret;
	pmd_t * pmd;
	struct vm_fault vmf;

	VM_BUG_ON(addr & PAGE_MASK);
	pmd = mm_find_pmd(mm, addr);
	if(!pmd)
		return;

	mmap_read_lock(mm);
	vmf.vma = vma;
	vmf.gfp_mask = GFP_HIGHUSER_MOVABLE;
	vmf.address = addr;
	vmf.pgoff = linear_page_index(vma, addr);
	vmf.pte = pte;
	vmf.orig_pte = orig_pte;
	vmf.page = page;
	vmf.pmd = pmd;
	vmf.flags = FAULT_FLAG_WRITE | FAULT_FLAG_REMOTE | FAULT_FLAG_ALLOW_RETRY | FAULT_FLAG_RETRY_NOWAIT;
	ret = do_swap_page_map_pte_profiling(&vmf, NULL, NULL);
	mmap_read_unlock(mm);
	if(ret == 0)
		adc_counter_add(1, ADC_EARLY_MAP_PTE);
}
EXPORT_SYMBOL(hermit_faultin_page);
