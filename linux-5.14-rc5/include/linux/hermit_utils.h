/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_UTILS_H
#define _LINUX_HERMIT_UTILS_H
/*
 * Declarations for Hermit functions in mm/hermit_utils.c
 */
#include <linux/adc_macros.h>
#include <linux/adc_timer.h>
#include <linux/swap.h>
#include <linux/hermit_types.h>

/***
 * util functions
 */
void hermit_cleanup_thread(struct task_struct *thd);
void hermit_init_mm(struct mm_struct *mm);
void hermit_cleanup_mm(struct mm_struct *mm);

unsigned hmt_get_sthd_cnt(struct mem_cgroup *memcg, struct hmt_swap_ctrl *sc);
void hmt_update_swap_ctrl(struct mem_cgroup *memcg, struct hmt_swap_ctrl *sc);
void hermit_faultin_vm_range(struct vm_area_struct *vma, unsigned long start, unsigned long end);
void hermit_faultin_page(struct vm_area_struct *vma,
		unsigned long addr, struct page * page, pte_t*pte, pte_t orig_pte);
#endif // _LINUX_HERMIT_UTILS_H
