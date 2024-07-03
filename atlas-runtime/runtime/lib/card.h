#pragma once
#include "bks_types.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdio.h>
#include <sys/mman.h>
#include <thread>
#include <vector>

namespace atlas {

class Card {
  public:
    static constexpr unsigned kUnit = 1 << BKS_CARD_SHIFT;
    static constexpr uint64_t kCardBytes =
        BKS_PSF_MAX_MEM_MB * 1024UL * 1024UL / kUnit / 8;
    Card() : card_(nullptr) {}
    void Init(struct bks_card *card) {
        card_ = card;
        // mlock(card_, kCardBytes);
    }
    static constexpr uintptr_t kAddrBase =
        (BKS_PSF_VA_BASE - BKS_PSF_MAX_MEM_MB * 1024UL * 1024UL);
    void Access(uintptr_t ptr, uint16_t size) {
        uint64_t addr = ptr;
        uint64_t addr_last_byte = addr + size - 1;
        const uint64_t card_bit_size = sizeof(struct bks_card) * 8;
        static_assert(card_bit_size == 32);
        // Find the page card the object corresponds to
        uint64_t page_idx = (addr - kAddrBase) >> BKS_PAGE_SHIFT;
        uint64_t page_offset = addr % BKS_PAGE_SIZE;
        uint64_t first_card_idx = page_offset >> BKS_CARD_SHIFT;
        uint64_t last_card_idx;
        // TODO: restriction here: we only consider cards on the first page
        if (page_offset + size >= BKS_PAGE_SIZE) {
            last_card_idx = card_bit_size - 1;
        } else {
            last_card_idx = (addr_last_byte % BKS_PAGE_SIZE) >> BKS_CARD_SHIFT;
        }
        uint64_t num_cards = last_card_idx - first_card_idx + 1;
        uint32_t mask = 0xFFFFFFFF >> (card_bit_size - num_cards);
        card_[page_idx].card |= mask << first_card_idx;
    }
    uint32_t GetPageAccess(uintptr_t ptr) {
        uint64_t card_index = (ptr - kAddrBase) >> BKS_PAGE_SHIFT;
        return __builtin_popcount(card_[card_index].card);
    }
    void ClearPageAccess(uintptr_t ptr) {
        uint64_t card_index = (ptr - kAddrBase) >> BKS_PAGE_SHIFT;
        card_[card_index].card = 0;
    }
    void ClearAll() { memset(card_, 0, kCardBytes); }

  private:
    struct bks_card *card_;
};
} // namespace atlas