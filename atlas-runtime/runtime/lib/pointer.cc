#include "pointer.h"
#include "bks_ctx.h"
#include "card.h"
#include "pointer_shim.h"
#include "tsx.h"
#include <cstring>
#include <unistd.h>

extern atlas::BksContext *bks_ctx;
extern struct bks_card *global_card;
uint32_t enable_card = 0;

namespace atlas {

extern Card global_card_proxy;

static FORCE_INLINE void *paging_in(AtlasPtrMeta &meta) {
    // meta.inc_refcnt();
    return (void *)meta.get_object_addr();
}

FORCE_INLINE void *AtlasPtrMeta::finish_evacuation(uint64_t new_object_addr) {
    meta_t old_meta = {.u64 = load()};
    BARRIER_ASSERT(old_meta.s.evacuation && old_meta.s.refcnt == 0 &&
                   old_meta.s.tospace == 0);
    meta_t new_meta = {.u64 = old_meta.u64};
    new_meta.s.evacuation = 0;
    void *object_ptr = nullptr;
    if (new_object_addr != 0) {
        new_meta.s.refcnt = 0;
        new_meta.s.tospace = 1;
        new_meta.s.obj_addr = new_object_addr;
        object_ptr = (void *)new_object_addr;
    } else {
        new_meta.s.refcnt = 0;
        object_ptr = AtlasPtrMeta::to_ptr(old_meta);
    }

    if (!metadata_.au64.compare_exchange_strong(old_meta.u64, new_meta.u64)) {
        BARRIER_ASSERT(
            false &&
            " someone changed the metadata when the object is evacuating");
    }

    return object_ptr;
}

void AtlasPtrMeta::update_metadata(uint64_t object_addr, unsigned object_size) {
    do {
        uint64_t old = load();
        if (old & kEvacuationBitMask) {
            cpu_relax();
            continue;
        }
        auto skip_check = [](const meta_t &meta) -> bool {
            return meta.s.obj_size == 0 ||
                   ((meta.s.obj_addr & PAGE_MASK) + get_size(meta) > PAGE_SIZE);
        };

        meta_t old_meta{
            .u64 = old,
        };
        meta_t new_meta{
            .u64 = AtlasPtrMeta(0, object_addr, object_size).metadata_.u64,
        };

        /* It should be careful when update object's addr and size to make sure
         * the refcnt is updated correctly in deref_get & deref_put */
        BARRIER_ASSERT(skip_check(old_meta) == skip_check(new_meta));

        old_meta.s.obj_addr = new_meta.s.obj_addr;
        old_meta.s.obj_size = new_meta.s.obj_size;
        if (metadata_.au64.compare_exchange_strong(old, old_meta.u64)) {
            return;
        };
        cpu_relax();
    } while (true);
}

void *AtlasGenericPtr::deref_get_slow_path(uint64_t object_addr) {
    enum {
        kCardAccessThres = 8,
    };
    bool remote = tsx_remote_check((void *)object_addr);
    if (!remote) {
        if (enable_card) {
            global_card_proxy.Access(object_addr, meta_.get_object_size());
        }
        return paging_in(meta_);
    }

    unsigned psf_index =
        (meta_.metadata_.s.obj_addr - BKS_PSF_VA_END) >> BKS_PSF_CHUNK_SHIFT;
    if (!global_psf[psf_index].psf) {
        if (enable_card) {
            global_card_proxy.Access(object_addr, meta_.get_object_size());
        }
        return paging_in(meta_);
    }

    if (enable_card) {
        uint32_t page_access = global_card_proxy.GetPageAccess(object_addr);
        if (page_access >= kCardAccessThres) {
            global_card_proxy.ClearPageAccess(object_addr);
            global_card_proxy.Access(object_addr, meta_.get_object_size());
            return paging_in(meta_);
        }
    }

    bool succ = meta_.try_set_evacuation();
    /* cannot evacuate the object, either because some threads are holding the
     * page refcnt, or the object is being evacuated */
    if (!succ) {
        return paging_in(meta_);
    }

    object_addr = meta_.get_object_addr();

    BARRIER_ASSERT(bks_ctx != nullptr);

    void *new_object_addr =
        bks_ctx->Fetch((void *)object_addr, meta_.get_object_size());
    if (enable_card) {
        if (!new_object_addr) {
            global_card_proxy.Access((uint64_t)object_addr,
                                     meta_.get_object_size());
        } else {
            global_card_proxy.Access((uint64_t)new_object_addr,
                                     meta_.get_object_size());
        }
    }
    return meta_.finish_evacuation((uint64_t)new_object_addr);
}

void AtlasGenericPtr::deref_put_slow_path(void *ptr) {
    // meta_.dec_refcnt();
}

void atlas_free_object(void *ptr) {}

} // namespace atlas

static_assert(sizeof(atlas_unique_ptr) == sizeof(atlas::AtlasUniquePtr<void>));

atlas_unique_ptr atlas_make_unique_ptr(void *object, unsigned object_size) {
    atlas_unique_ptr up;
    new (&up) atlas::AtlasUniquePtr<void>((uintptr_t)object, object_size);
    return up;
}

void *atlas_up_deref_get(atlas_unique_ptr *up) {
    return ((atlas::AtlasUniquePtr<void> *)up)->deref_get();
}

void atlas_up_deref_put(atlas_unique_ptr *up, void *ptr) {
    ((atlas::AtlasUniquePtr<void> *)up)->deref_put(ptr);
}

void atlas_up_reset(atlas_unique_ptr *up, void *ptr, unsigned object_size) {
    ((atlas::AtlasUniquePtr<void> *)up)->reset(ptr, object_size);
}

void atlas_up_release(atlas_unique_ptr *up) {
    ((atlas::AtlasUniquePtr<void> *)up)->~AtlasUniquePtr();
}

void atlas_up_mark_evacuation(atlas_unique_ptr *up) {
    ((atlas::AtlasUniquePtr<void> *)up)->mark_evacuation();
}

void atlas_up_clear_evacuation(atlas_unique_ptr *up) {
    ((atlas::AtlasUniquePtr<void> *)up)->clear_evacuation();
}

void atlas_clean_card() { atlas::global_card_proxy.ClearAll(); }
