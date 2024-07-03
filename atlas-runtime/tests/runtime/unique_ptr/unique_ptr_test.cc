#include "pointer.h"
#include "runtime.h"
#include <bks_types.h>
#include <chrono>
#include <gtest/gtest.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>

enum {
    kPageSize = 4096,
    kMagic = 0xdeadbeefu,
    kObjectWords = 16,
};
const size_t kTotalSize = 1024ul * 1024 * 1024; // 1GB

struct Object {
    uint64_t val[kObjectWords];
};

TEST(AtlasUniquePtrTest, TestRead) {
    const size_t kNumObjects = kTotalSize / sizeof(Object);
    std::vector<atlas::AtlasUniquePtr<Object>> objects(kNumObjects);
    Object *buffer =
        reinterpret_cast<Object *>(aligned_alloc(kPageSize, kTotalSize));

    memset(buffer, 0, kTotalSize);

    for (size_t i = 0; i < kNumObjects; i++) {
        objects[i].reset_t(buffer + i);
    }

    /* touch the buffer; the buffer[kNumObjects - 1] should be in remote after
     * the touch */
    for (long i = kNumObjects - 1; i >= 0; i--) {
        buffer[i].val[0] = kMagic + i;
    }

    printf("Initialized %lu objects\n", kNumObjects);

    runtime_init();
    /* set the psf to object-in manually */
    memset(global_psf, 0xf, BKS_PSF_MMAP_SIZE);

    Object *object_first = objects[0].deref_get();
    Object *object_last = objects[kNumObjects - 1].deref_get();

    ASSERT_EQ(object_first, buffer)
        << "The first object should not be evacuated";
    ASSERT_NE(object_last, buffer + kNumObjects - 1)
        << "The last object should be evacuated";
    ASSERT_EQ(object_last->val[0], kMagic + kNumObjects - 1);

    objects[0].deref_put(object_first);
    objects[kNumObjects - 1].deref_put(object_last);

    runtime_exit();
    free(buffer);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
