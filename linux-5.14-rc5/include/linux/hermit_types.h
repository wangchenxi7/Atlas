/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_TYPES_H
#define _LINUX_HERMIT_TYPES_H
/*
 * Declarations for Hermit internal data structures
 */

#include <linux/spinlock_types.h>
#include <linux/atomic.h>

/* YIFAN: for now support up to 32 sthreads */
#define HMT_MAX_NR_STHDS 32

/*
 * swap stats to control async swap out
 */
struct hmt_swap_ctrl {
	spinlock_t lock;
	atomic_t sthd_cnt;
	atomic_t active_sthd_cnt;
	uint64_t swin_ts[2];
	uint64_t nr_pg_charged[2];
	uint64_t swin_thrghpt;

	struct {
		uint64_t nr_pages;
		uint64_t total;
		uint64_t avg;
		unsigned cnt;
	} swout_dur;
	bool stop;
	bool master_up;
	short log_cnt;

	uint64_t swout_thrghpt;

	struct mem_cgroup *memcg;
};

struct list_head;

/*
 * data struct area type:
 *      sequential: swap based on LRU
 *      random: swap in/out together
 */
enum ds_area_type {
	DSA_STREAMING,
	DSA_PARFOR,
	DSA_RANDOM,
	DSA_NON_DSA,
	NUM_DSA_TYPE,
};

/*
 * This struct describes a data structure area.
 * A vma contains a list of dsas.
 */
struct ds_area_struct {
	unsigned long start;
	unsigned long end;
	struct list_head node;
	spinlock_t val_lock;
	struct list_head vaddr_list;
	atomic_t vaddr_cnt;

	enum ds_area_type type;

	// stats
	atomic_t swpin_cnt;
	atomic_t swpout_cnt;

	// prefetching related info, the same as the one in vma
	atomic_long_t swap_readahead_info;

	// refault distance
	atomic_t fault_cnt;
	atomic64_t refault_dist;

	int swapout_win;
};

#endif // _LINUX_HERMIT_TYPES_H
