#pragma once
#include <asm/types.h>
#include <memory>

namespace atlas {

class FileHandle {
  public:
    FileHandle(int fd) : fd_(fd) {}
    ~FileHandle();
    int Fd() { return fd_; }

  private:
    int fd_;
};

class ThreadLocalResource {
  public:
    ThreadLocalResource() : dma_handle_(0), mmap_page_(nullptr) {}
    ~ThreadLocalResource();
    __u64 Handle() { return dma_handle_; }
    void *Page() { return mmap_page_; }
    void Init(std::shared_ptr<FileHandle> fd, void *mmap_start);

  private:
    std::shared_ptr<FileHandle> fd_;
    __u64 dma_handle_;
    void *mmap_page_;
};

class BksContext {
  public:
    BksContext();
    ~BksContext();
    void *Fetch(const void *object, int object_size);
    bool FetchAsync(const void *object, int object_size, int *queue);
    void *Sync(int queue, int object_size);
    bool Read(void *dst, const void *object, int object_size);

  private:
    static thread_local ThreadLocalResource resource_;
    bool IoctlFetch(const void *object, int object_size, int sync, int *queue,
                    int dst_offset);
    std::shared_ptr<FileHandle> fd_;
    void *mmap_pages_;
};

} // namespace atlas