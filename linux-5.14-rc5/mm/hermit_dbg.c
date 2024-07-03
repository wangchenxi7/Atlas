/*
 * mm/hermit_dbg.c - hermit debug utilities. Functions declared in hermit.h.
 */
#include <linux/hermit.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/pagewalk.h>

#include <asm/tlb.h>

#include "internal.h"

enum HMT_DEBUG_TYPE {
	HMT_EVICT = 0,
	HMT_SET_STHD_CPUS = 1,
};

/*
 * [Hermit] mimicing `madvise_cold_or_pageout_pte_range`.
 * Warning: don't support THPs and huge pages.
 */
static int hermit_evict_pte_range(pmd_t *pmd, unsigned long addr,
				  unsigned long end, struct mm_walk *walk)
{
	struct mmu_gather *tlb = walk->private;
	struct mm_struct *mm = tlb->mm;
	struct vm_area_struct *vma = walk->vma;
	pte_t *orig_pte, *pte, ptent;
	spinlock_t *ptl;
	struct page *page = NULL;
	LIST_HEAD(page_list);
	LIST_HEAD(vpage_list);

	if (fatal_signal_pending(current))
		return -EINTR;

	// [Hermit] doesn't support THPs
	if (pmd_trans_huge(*pmd))
		return -EINVAL;
	if (pmd_trans_unstable(pmd))
		return 0;

	tlb_change_page_size(tlb, PAGE_SIZE);
	orig_pte = pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	flush_tlb_batched_pending(mm);
	arch_enter_lazy_mmu_mode();
	for (; addr < end; pte++, addr += PAGE_SIZE) {
		ptent = *pte;

		if (pte_none(ptent))
			continue;

		if (!pte_present(ptent))
			continue;

		page = vm_normal_page(vma, addr, ptent);
		if (!page)
			continue;

		/*
		 * Creating a THP page is expensive so split it only if we
		 * are sure it's worth. Split it if we are only owner.
		 */
		if (PageTransCompound(page)) {
			if (page_mapcount(page) != 1)
				break;
			get_page(page);
			if (!trylock_page(page)) {
				put_page(page);
				break;
			}
			pte_unmap_unlock(orig_pte, ptl);
			if (split_huge_page(page)) {
				unlock_page(page);
				put_page(page);
				pte_offset_map_lock(mm, pmd, addr, &ptl);
				break;
			}
			unlock_page(page);
			put_page(page);
			pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
			pte--;
			addr -= PAGE_SIZE;
			continue;
		}

		/* Do not interfere with other mappings of this page */
		if (page_mapcount(page) != 1)
			continue;

		VM_BUG_ON_PAGE(PageTransCompound(page), page);

		if (pte_young(ptent)) {
			ptent = ptep_get_and_clear_full(mm, addr, pte,
							tlb->fullmm);
			ptent = pte_mkold(ptent);
			set_pte_at(mm, addr, pte, ptent);
			tlb_remove_tlb_entry(tlb, pte, addr);
		}

		/*
		 * We are deactivating a page for accelerating reclaiming.
		 * VM couldn't reclaim the page unless we clear PG_young.
		 * As a side effect, it makes confuse idle-page tracking
		 * because they will miss recent referenced history.
		 */
		ClearPageReferenced(page);
		test_and_clear_page_young(page);

		if (!isolate_lru_page(page)) {
			if (PageUnevictable(page))
				putback_lru_page(page);
			else {
				list_add(&page->lru, &page_list);
				if (hmt_ctl_flag(HMT_DATAPATH)) {
					struct vpage *vpage = create_vpage(
						vma, addr, page, pte);
					list_add(&vpage->node, &vpage_list);
				}
			}
		}
	}

	arch_leave_lazy_mmu_mode();
	pte_unmap_unlock(orig_pte, ptl);
	if (hmt_ctl_flag(HMT_DATAPATH))
		hermit_reclaim_vpages(&vpage_list, true);
	else
		hermit_reclaim_pages(&page_list, true);
	cond_resched();

	return 0;
}

static const struct mm_walk_ops hermit_evict_ops = {
	.pmd_entry = hermit_evict_pte_range,
};

// [Hermit] mimicing madvise_vma
static void hermit_pageout_page_range(struct mmu_gather *tlb,
				      struct vm_area_struct *vma,
				      unsigned long addr, unsigned long end)
{
	tlb_start_vma(tlb, vma);
	// Given that we only need @tlb, we use it directly as walk->private.
	walk_page_range(vma->vm_mm, addr, end, &hermit_evict_ops, tlb);
	tlb_end_vma(tlb, vma);
}

static inline bool can_do_pageout(struct vm_area_struct *vma)
{
	if (vma_is_anonymous(vma))
		return true;
	if (!vma->vm_file)
		return false;
	/*
	 * paging out pagecache only for non-anonymous mappings that correspond
	 * to the files the calling process could (if tried) open for writing;
	 * otherwise we'd be including shared non-exclusive mappings, which
	 * opens a side channel.
	 */
	return inode_owner_or_capable(&init_user_ns,
				      file_inode(vma->vm_file)) ||
	       file_permission(vma->vm_file, MAY_WRITE) == 0;
}

static int hermit_evict_vma_range(struct vm_area_struct *vma,
				  struct vm_area_struct **prev,
				  unsigned long start, unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	struct mmu_gather tlb;

	*prev = vma;
	if (!can_madv_lru_vma(vma))
		return -EINVAL;

	if (!can_do_pageout(vma))
		return 0;

	pr_err("%s: vma [0x%lu, 0x%lu), evict_range: [0x%lu, 0x%lu)\n",
	       __func__, vma->vm_start, vma->vm_end, start, end);
	lru_add_drain_all();
	tlb_gather_mmu(&tlb, mm);
	hermit_pageout_page_range(&tlb, vma, start, end);
	tlb_finish_mmu(&tlb);

	return 0;
}

// [Hermit] mimicing do_madvise
int hermit_evict_mm_range(struct mm_struct *mm, unsigned long start, size_t len_in)
{
	unsigned long end, tmp;
	struct vm_area_struct *vma, *prev;
	int unmapped_error = 0;
	int error = -EINVAL;
	int write;
	size_t len;
	struct blk_plug plug;

	start = untagged_addr(start);

	// if (!PAGE_ALIGNED(start))
	// 	return error;
	start = PAGE_ALIGN(start);
	len = PAGE_ALIGN(len_in);

	/* Check to see whether len was rounded up from small -ve to zero */
	if (len_in && !len)
		return error;

	end = start + len;
	if (end < start)
		return error;

	error = 0;
	if (end == start)
		return error;

	// [Hermit] we do page eviction below, which shouldn't write to pages
	write = false;
	if (write) {
		if (mmap_write_lock_killable(mm))
			return -EINTR;
	} else {
		mmap_read_lock(mm);
	}

	/*
	 * If the interval [start,end) covers some unmapped address
	 * ranges, just ignore them, but return -ENOMEM at the end.
	 * - different from the way of handling in mlock etc.
	 */
	vma = find_vma_prev(mm, start, &prev);
	if (vma && start > vma->vm_start)
		prev = vma;

	blk_start_plug(&plug);
	for (;;) {
		/* Still start < end. */
		error = -ENOMEM;
		if (!vma)
			goto out;

		/* Here start < (end|vma->vm_end). */
		if (start < vma->vm_start) {
			unmapped_error = -ENOMEM;
			start = vma->vm_start;
			if (start >= end)
				goto out;
		}

		/* Here vma->vm_start <= start < (end|vma->vm_end) */
		tmp = vma->vm_end;
		if (end < tmp)
			tmp = end;

		/* Here vma->vm_start <= start < tmp <= (end|vma->vm_end). */
		error = hermit_evict_vma_range(vma, &prev, start, tmp);
		if (error)
			goto out;
		start = tmp;
		if (prev && start < prev->vm_end)
			start = prev->vm_end;
		error = unmapped_error;
		if (start >= end)
			goto out;
		if (prev)
			vma = prev->vm_next;
		else	/* madvise_remove dropped mmap_lock */
			vma = find_vma(mm, start);
	}
out:
	blk_finish_plug(&plug);
	if (write)
		mmap_write_unlock(mm);
	else
		mmap_read_unlock(mm);

	return error;
}

inline void hermit_dbg(int type, void *buf, unsigned long buf_len)
{
	if (type == HMT_EVICT) {
		int ret = 0;
		unsigned long *params = (unsigned long *)buf;
		unsigned long start = params[0];
		unsigned long len = params[1];

		pr_err("%s:evict_range 0x%lx, %lu\n", __func__, start, len);

		ret = hermit_evict_mm_range(current->mm, start, len);
		if (ret)
			pr_err("%s:evict_range ret %d\n", __func__, ret);
	} else if (type == HMT_SET_STHD_CPUS) {
		pr_err("hermit: set_sthd_cores\n");
		hermit_set_sthd_cores(buf, buf_len);
	} else {
		pr_err("unsupport hermit_dbg syscall type! %d\n", type);
	}
}
