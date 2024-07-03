#ifndef __RSWAP_RDMA_CUSTOM_OPS_H
#define __RSWAP_RDMA_CUSTOM_OPS_H
#include <linux/mm_types.h>
#include <linux/swap_stats.h>
int rswap_frontswap_load_async_early_map(pgoff_t swp_entry_offset,
                                       struct page *page, u64 address,
                                       struct vm_area_struct *vma, pte_t *ptep,
                                       pte_t orig_pte);
int rswap_frontswap_pref_async_early_map(pgoff_t swp_entry_offset,
                                       struct page *page, u64 address,
                                       struct vm_area_struct *vma, pte_t *ptep,
                                       pte_t orig_pte, int cpu);
int rswap_atlas_pref_object(pgoff_t swp_entry_offset,
                                u64 dma_addr, int dma_off,
                                int obj_off, int obj_size, int cpu, bool sync);
int rswap_atlas_page_map_dma(struct page *page, u64 *dma_addr);
int rswap_atlas_page_unmap_dma(u64 dma_addr);
int rswap_atlas_sync(int cpu);
#endif
