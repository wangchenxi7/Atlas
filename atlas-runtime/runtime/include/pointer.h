#pragma once
#include "helpers.h"
#include <atomic>
#include <bks_types.h>
#include <stdint.h>
#include <stdio.h>
#include <thread>

extern struct bks_psf *global_psf;

namespace atlas {
template <typename T> class AtlasUniquePtr;
template <typename T, typename H> class AtlasUniquePtrWithHeader;

class AtlasPtrMeta {
  private:
    constexpr static uint64_t kWordSize = sizeof(unsigned);
    constexpr static uint64_t kUnusedBits = 1;
    constexpr static uint64_t kRefcntBitPos = kUnusedBits;
    constexpr static uint64_t kRefcntBits = 5;
    constexpr static uint64_t kRefcntMax = (1ul << kRefcntBits) - 1;
    constexpr static uint64_t kRefcntMask = kRefcntMax << kRefcntBitPos;
    constexpr static uint64_t kTospaceBitPos = kRefcntBitPos + kRefcntBits;
    constexpr static uint64_t kTospaceMask = 1ul << kTospaceBitPos;
    constexpr static uint64_t kEvacuationBitPos = kTospaceBitPos + 1;
    constexpr static uint64_t kEvacuationBitMask = 1 << kEvacuationBitPos;
    constexpr static uint64_t kObjectSizeBitPos = kEvacuationBitPos + 1;
    constexpr static uint64_t kObjectSizeBits = 9;
    constexpr static uint64_t kMaxObjectSize = (1 << kObjectSizeBits) - 1;
    constexpr static uint64_t kObjectAddrBits = 47;

    static_assert(kObjectSizeBitPos + kObjectSizeBits + kObjectAddrBits == 64,
                  "The metadata is not 64 bits");

    union meta_t {
        uint64_t u64;
        std::atomic_uint64_t au64;
        struct {
            unsigned long unused : kUnusedBits;
            unsigned long refcnt : kRefcntBits;
            unsigned long tospace : 1;
            unsigned long evacuation : 1;
            unsigned long obj_size : kObjectSizeBits;
            unsigned long obj_addr : kObjectAddrBits;
        } s;
    } metadata_;

    static_assert(sizeof(metadata_) == sizeof(void *),
                  "AtlasPtrMeta size mismatch");
    friend class AtlasGenericPtr;
    template <typename T> friend class AtlasUniquePtr;
    template <typename T, typename H> friend class AtlasUniquePtrWithHeader;

    FORCE_INLINE AtlasPtrMeta() { metadata_.u64 = 0; }

    void init(uint64_t object_addr, unsigned object_size) {

        unsigned words = DIV_UP(object_size, kWordSize);
        if (words > kMaxObjectSize) {
            words = 0;
        }
        meta_t meta = {
            .u64 = 0,
        };
        meta.s.obj_addr = object_addr;
        meta.s.obj_size = words;
        metadata_.u64 = meta.u64;
    }

  public:
    AtlasPtrMeta(bool tospace, uint64_t object_addr, unsigned object_size) {
        init(object_addr, object_size);
        metadata_.s.tospace = tospace;
    }

    FORCE_INLINE bool is_tospace() const { return metadata_.s.tospace; }

    FORCE_INLINE bool try_set_evacuation() {
        do {
            uint64_t old_meta = load();
            if ((old_meta & kTospaceMask) || (old_meta & kRefcntMask) ||
                (old_meta & kEvacuationBitMask)) {
                return false;
            }
            if (metadata_.au64.compare_exchange_strong(
                    old_meta, old_meta | kEvacuationBitMask)) {
                return true;
            };
            cpu_relax();
        } while (true);
    }

    FORCE_INLINE void mark_evacuation() {
        do {
            uint64_t old_meta = load();
            if ((old_meta & kRefcntMask) || (old_meta & kEvacuationBitMask)) {
                cpu_relax();
                continue;
            }
            if (metadata_.au64.compare_exchange_strong(
                    old_meta, old_meta | kEvacuationBitMask)) {
                return;
            };
            cpu_relax();
        } while (true);
    }

    FORCE_INLINE void clear_evacuation() {
        BARRIER_ASSERT(is_evacuation());
        metadata_.s.evacuation = 0;
    }

    void *finish_evacuation(uint64_t new_object_addr);

    FORCE_INLINE bool is_evacuation() const {
        return !!(metadata_.au64.load(std::memory_order_acquire) &
                  kEvacuationBitMask);
    }

    FORCE_INLINE void inc_refcnt() {
        do {
            uint64_t old_meta = load();
            uint64_t refcnt = (old_meta & kRefcntMask) >> kRefcntBitPos;

            if (old_meta & kTospaceMask) {
                return;
            }

            /* the refcnt is full, wait until someone release it */
            if (refcnt == kRefcntMax) {
                std::this_thread::yield();
                cpu_relax();
                continue;
            }

            /* the object is in evacuation */
            if (old_meta & kEvacuationBitMask) {
                cpu_relax();
                continue;
            }

            meta_t new_meta = {
                .u64 = old_meta,
            };
            new_meta.s.refcnt = refcnt + 1;

            if (metadata_.au64.compare_exchange_strong(old_meta,
                                                       new_meta.u64)) {
                return;
            };
            cpu_relax();
        } while (true);
    }

    FORCE_INLINE void dec_refcnt() {
        do {
            uint64_t old_meta = load();
            if (old_meta & kTospaceMask) {
                return;
            }

            uint64_t refcnt = (old_meta & kRefcntMask) >> kRefcntBitPos;

            BARRIER_ASSERT(refcnt > 0);

            meta_t new_meta = {
                .u64 = old_meta,
            };
            new_meta.s.refcnt = refcnt - 1;

            if (metadata_.au64.compare_exchange_strong(old_meta,
                                                       new_meta.u64)) {
                return;
            };
            cpu_relax();
        } while (true);
    }

    uint64_t get_object_addr() const { return metadata_.s.obj_addr; }

    void set_object_addr(uint64_t object_addr) {
        metadata_.s.obj_addr = object_addr;
    }

    uint16_t get_object_size() const {
        return metadata_.s.obj_size * kWordSize;
    };

    bool is_null() const { return metadata_.u64 == 0; };

    void nullify() { metadata_.u64 = 0; };
    FORCE_INLINE uint64_t load() {
        return metadata_.au64.load(std::memory_order_acquire);
    }
    static FORCE_INLINE void *to_ptr(const meta_t &meta) {
        return reinterpret_cast<void *>((unsigned long)meta.s.obj_addr);
    }
    static FORCE_INLINE uint16_t get_size(const meta_t &meta) {
        return meta.s.obj_size * kWordSize;
    }
    void update_metadata(uint64_t object_addr, unsigned object_size);
};

class AtlasGenericPtr {
  protected:
    AtlasPtrMeta meta_;
    void *deref_get_slow_path(uint64_t object_addr);
    void *deref_no_scope_slow_path(uint64_t object_addr);
    void deref_put_slow_path(void *ptr);
    void *deref_raw_slow_path(uint64_t object_addr, int offset);
    void *deref_readonly_slow_path(uint64_t object_addr, void *dst);

    AtlasGenericPtr() {}
    AtlasGenericPtr(uint64_t object_addr, unsigned object_size) {
        meta_.init(object_addr, object_size);
    }
    FORCE_INLINE AtlasPtrMeta &meta() { return meta_; }
    FORCE_INLINE const AtlasPtrMeta &meta() const { return meta_; }
    FORCE_INLINE bool should_paging(const AtlasPtrMeta::meta_t &meta) {
        /* skip to-space objects and cross-page objects for now */
        if (meta.s.obj_size == 0 || meta.s.tospace != 0 ||
            (meta.s.obj_addr & PAGE_MASK) + AtlasPtrMeta::get_size(meta) >
                PAGE_SIZE) {
            return true;
        }

        // out of range
        if (meta.s.obj_addr <=
            BKS_PSF_VA_BASE - BKS_PSF_MAX_MEM_MB * 1024 * 1024) {
            BARRIER_ASSERT(false);
            meta_.metadata_.s.obj_size = 0;
            return true;
        }

        return false;
    }

    FORCE_INLINE void *deref_get() {
        AtlasPtrMeta::meta_t meta = {
            .u64 = meta_.load(),
        };

        if (should_paging(meta)) {
            return (void *)meta_.get_object_addr();
        }

        return deref_get_slow_path(meta.s.obj_addr);
    }

    FORCE_INLINE void deref_put(void *ptr) {
        AtlasPtrMeta::meta_t meta = {
            .u64 = meta_.load(),
        };

        if (meta.s.obj_size == 0 || meta.s.tospace ||
            (meta.s.obj_addr & PAGE_MASK) + AtlasPtrMeta::get_size(meta) >
                PAGE_SIZE) {
            return;
        }

        deref_put_slow_path(ptr);
    };

    FORCE_INLINE void mark_evacuation() { meta_.mark_evacuation(); }

    FORCE_INLINE void clear_evacuation() { meta_.clear_evacuation(); }

  public:
    FORCE_INLINE void nullify() { meta_.nullify(); };
};

extern void atlas_free_object(void *ptr);

template <typename T> class AtlasUniquePtr : public AtlasGenericPtr {
  public:
    AtlasUniquePtr() { nullify(); }
    ~AtlasUniquePtr() {}
    AtlasUniquePtr(uint64_t object_addr, unsigned object_size)
        : AtlasGenericPtr(object_addr, object_size) {}
    AtlasUniquePtr(AtlasUniquePtr &&other) {
        meta_ = other.meta_;
        other.nullify();
    }
    FORCE_INLINE AtlasUniquePtr &operator=(AtlasUniquePtr &&other) {
        meta_.metadata_.u64 = other.meta_.metadata_.u64;
        other.nullify();
        return *this;
    }
    NOT_COPYABLE(AtlasUniquePtr);
    void reset(T *ptr, unsigned size) {
        if (meta_.is_tospace()) {
            atlas_free_object((void *)meta_.get_object_addr());
        }
        meta_.init((uint64_t)ptr, size);
    }
    void reset_t(T *ptr) { reset(ptr, sizeof(T)); }
    FORCE_INLINE T *deref_get() { return (T *)AtlasGenericPtr::deref_get(); }
    FORCE_INLINE void deref_put(T *ptr) { AtlasGenericPtr::deref_put(ptr); }
    FORCE_INLINE unsigned get_size() { return meta_.get_object_size(); }
    FORCE_INLINE bool is_tospace() { return meta_.is_tospace(); }
    FORCE_INLINE void mark_evacuation() { return meta_.mark_evacuation(); }
    FORCE_INLINE void clear_evacuation() { return meta_.clear_evacuation(); }
    FORCE_INLINE void update_metadata(uint64_t object_addr,
                                      unsigned object_size) {
        meta_.update_metadata(object_addr, object_size);
    }
};

} // namespace atlas
