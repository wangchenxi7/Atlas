#include "bks_ctx.h"
#include "assert.h"
#include "card.h"
#include "helpers.h"
#include "tsx.h"
#include <bks_ioctl.h>
#include <bks_types.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

struct bks_psf *global_psf = NULL;
struct bks_card *global_card = NULL;
bool card_profiling_on = false;
namespace atlas {
Card global_card_proxy;
};

namespace atlas {
enum {
    kPageSize = 4096,
};

thread_local ThreadLocalResource BksContext::resource_;

BksContext::BksContext() {
    int fd = open("/dev/bks_dev", O_RDWR);
    BARRIER_ASSERT(fd != -1);
    fd_ = std::make_shared<FileHandle>(fd);
    void *psf = mmap(nullptr, BKS_CARD_NUM, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, BKS_PSF_MMAP_PGOFF * kPageSize);
    BARRIER_ASSERT(psf != MAP_FAILED);
    global_psf = (struct bks_psf *)psf;
    void *card = mmap(nullptr, BKS_CARD_NUM * sizeof(struct bks_card),
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                      BKS_CARD_MMAP_PGOFF * kPageSize);
    BARRIER_ASSERT(card != MAP_FAILED);
    global_card = (struct bks_card *)card;
    mmap_pages_ = mmap(nullptr, BKS_MAX_DMA_PAGES * kPageSize,
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    BARRIER_ASSERT(mmap_pages_ != MAP_FAILED);
    atlas::global_card_proxy.Init(global_card);
}

BksContext::~BksContext() {}

void *BksContext::Fetch(const void *object, int object_size) {
    if (!IoctlFetch(object, object_size, 1, nullptr, 0)) {
        return nullptr;
    }
    void *new_object = malloc(object_size);
    memcpy(new_object, resource_.Page(), object_size);
    return new_object;
}

bool BksContext::FetchAsync(const void *object, int object_size, int *queue) {
    return IoctlFetch(object, object_size, 0, queue, 0);
}

void *BksContext::Sync(int queue, int object_size) {
    struct bks_fetch_sync sync_args;
    sync_args.queue_id = queue;
    int ret = ioctl(fd_->Fd(), BKS_IOCTL_FETCH_SYNC, &sync_args);
    BARRIER_ASSERT(!ret);
    void *new_object = malloc(object_size);
    memcpy(new_object, resource_.Page(), object_size);
    return new_object;
}

bool BksContext::IoctlFetch(const void *object, int object_size, int sync,
                            int *queue, int dst_offset) {
    int page_offset = (uintptr_t)object & (kPageSize - 1);
    if (page_offset + object_size + dst_offset > kPageSize) {
        return false;
    }

    if (resource_.Handle() == 0) {
        resource_.Init(fd_, mmap_pages_);
    }

    struct bks_fetch_object args;
    args.obj_page_addr = (uintptr_t)object;
    args.handle = resource_.Handle();
    args.obj_offset = page_offset;
    args.obj_size = object_size;
    args.sync = sync;
    args.dst_offset = dst_offset;
    if (ioctl(fd_->Fd(), BKS_IOCTL_FETCH_OBJECT, &args)) {
        return false;
    }

    if (!sync) {
        *queue = args.queue_id;
    }
    return true;
}

bool BksContext::Read(void *dst, const void *object, int object_size) {
    if (!IoctlFetch(object, object_size, 1, nullptr, 0)) {
        return false;
    }
    memcpy(dst, resource_.Page(), object_size);
    return true;
}

void ThreadLocalResource::Init(std::shared_ptr<FileHandle> fd,
                               void *mmap_start) {
    struct bks_alloc_dma_page alloc_args;
    int ret = ioctl(fd->Fd(), BKS_IOCTL_ALLOC_DMA_PAGE, &alloc_args);
    BARRIER_ASSERT(!ret);
    dma_handle_ = alloc_args.handle;
    mmap_page_ = (uint8_t *)mmap_start + alloc_args.pg_off * kPageSize;
    fd_ = std::move(fd);
}

ThreadLocalResource::~ThreadLocalResource() {
    if (mmap_page_ != nullptr) {
        struct bks_free_dma_page free_args;
        free_args.handle = dma_handle_;
        int ret = ioctl(fd_->Fd(), BKS_IOCTL_FREE_DMA_PAGE, &free_args);
        BARRIER_ASSERT(!ret);
    }
}

FileHandle::~FileHandle() { close(fd_); }
} // namespace atlas
