#pragma once

#define FORCE_INLINE inline __attribute__((always_inline))

#define NOT_COPYABLE(TypeName)                                                 \
    TypeName(TypeName const &) = delete;                                       \
    TypeName &operator=(TypeName const &) = delete;

#define ALIGN_UP(x, a) (((x) + (a)-1) & ~(unsigned long)((a)-1))

#define DIV_UP(x, a) (((x) + (a)-1) / (a))

FORCE_INLINE void cpu_relax() { asm volatile("pause"); }

#define PAGE_SIZE (4096UL)
#define PAGE_MASK (PAGE_SIZE - 1)

#include <cstdio>
#include <cstdlib>

#define BARRIER_ASSERT(x)                                                      \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "[%s:%d] Assertion failed:", __FILE__, __LINE__);  \
            fputs(#x "\n", stderr);                                            \
            abort();                                                           \
        }                                                                      \
    } while (0)
