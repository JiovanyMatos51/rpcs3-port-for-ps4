#pragma once

// Host stand-in for the console's <sys/mman.h> plus the libkernel calls orbis_mmap.cpp makes.
// FreeBSD values, so the allocator's own static_assert on the musl call-site tuple holds.
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_PRIVATE 0x0002
#define MAP_ANON    0x1000
#define MAP_FAILED  ((void*)-1)

#define ORBIS_KERNEL_WB_ONION    0
#define ORBIS_KERNEL_PROT_CPU_RW 0x3

extern "C" {
void*   mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);
int     munmap(void* addr, size_t len);
int     madvise(void* addr, size_t len, int advice);
size_t  sceKernelGetDirectMemorySize();
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int32_t type, off_t* phys);
int32_t sceKernelMapDirectMemory(void** va, size_t len, int32_t prot, int32_t flags, off_t phys, size_t align);
int32_t sceKernelMunmap(void* va, size_t len);
int32_t sceKernelReleaseDirectMemory(off_t phys, size_t len);
int32_t sceKernelUsleep(uint32_t us);
int32_t sceKernelAvailableFlexibleMemorySize(size_t* out);
uint64_t sceKernelGetProcessTime();
}
