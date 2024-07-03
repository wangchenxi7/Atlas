#include "runtime.h"
#include "bks_ctx.h"
#include "helpers.h"

atlas::BksContext *bks_ctx = nullptr;

void runtime_init() {
    if (!bks_ctx)
        bks_ctx = new atlas::BksContext();
}

void runtime_exit() {
    delete bks_ctx;
    bks_ctx = nullptr;
}

void *runtime_fetch(const void *object, int object_size) {
    BARRIER_ASSERT(bks_ctx != nullptr);
    return bks_ctx->Fetch(object, object_size);
}

bool runtime_read(void *dst, const void *object, int object_size) {
    BARRIER_ASSERT(bks_ctx != nullptr);
    return bks_ctx->Read(dst, object, object_size);
}
