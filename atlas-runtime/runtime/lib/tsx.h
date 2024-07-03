#pragma once
#include <immintrin.h>
#include <stdint.h>
static inline bool tsx_remote_check(const void *object) {
    bool is_local = false;
    uint32_t status = 0;
    uint8_t x;
retry:
    status = _xbegin();
    if (status == _XBEGIN_STARTED) {
        asm volatile("movb (%1), %0\n\t" : "=r"(x) : "r"(object) : "memory");
        _xend();
        is_local = true;
    } else if (status & _XABORT_RETRY) {
        goto retry;
    } else if (status & _XABORT_CONFLICT) {
        is_local = true;
    }
    return !is_local;
}
