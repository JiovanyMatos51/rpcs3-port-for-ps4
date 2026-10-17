#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
void rpcs3_ps4_qsort_r(void* base, size_t count, size_t size,
    int (*compare)(const void*, const void*, void*), void* context);
#ifdef __cplusplus
}
#endif

// GNU comparator order, as used by zstd with _GNU_SOURCE enabled.
// Keep a distinct symbol: BSD's qsort_r has a different argument order.
#define qsort_r rpcs3_ps4_qsort_r
