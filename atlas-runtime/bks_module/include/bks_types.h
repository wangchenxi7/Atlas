#pragma once

#define BKS_PSF_MAX_MEM_MB (256UL * 1024)
#define BKS_PSF_CHUNK_MB_SHIFT (1)
#define BKS_PSF_CHUNK_MB (1UL << BKS_PSF_CHUNK_MB_SHIFT)
//#define BKS_PSF_CHUNK_SHIFT (BKS_PSF_CHUNK_MB_SHIFT + 20)
#define BKS_PSF_MMAP_PGOFF 0x1000ULL
/* The start virtual address for mmap (disable ASLR) */
#define BKS_PSF_MAX_VA (0x7ffff7fff000UL)
/* Aligned to BKS_PSF_CHUNK */
#define BKS_PSF_VA_BASE (BKS_PSF_MAX_VA + 4096)
#define BKS_PSF_VA_END (BKS_PSF_VA_BASE - BKS_PSF_MAX_MEM_MB * 1024 * 1024)
#define BKS_CARD_NUM (BKS_PSF_MAX_MEM_MB * 1024 / 4) // Now this is the size of PSF in bytes
#define BKS_PSF_MMAP_SIZE (BKS_CARD_NUM)
#define BKS_CARD_MMAP_PGOFF (BKS_PSF_MMAP_PGOFF * 2 + BKS_CARD_NUM / 4096)
#define BKS_PAGE_SHIFT 12
#define BKS_PSF_CHUNK_SHIFT BKS_PAGE_SHIFT
#define BKS_CARD_SHIFT 7
#define BKS_PAGE_SIZE 0x1000ULL
#define BKS_MAX_DMA_PAGES (1024UL)

struct bks_psf {
    unsigned char psf;
};

struct bks_card {
    uint32_t card;
};

static_assert(sizeof(struct bks_psf) == sizeof(unsigned char),
              "bks_psf size mismatch");
static_assert(sizeof(struct bks_card) == sizeof(uint32_t),
              "bks_card size mismatch");
