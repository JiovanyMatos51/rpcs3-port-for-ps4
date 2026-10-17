#pragma once

#include <cstddef>
#include <cstdint>

// PS4 kernel signal ABI, described by shadPS4's kernel/threads/exception.h.
// The OpenOrbis SDK ucontext_t still has Linux's register order and prefix.
// Keep this type local to the fault handler; do not change the SDK libc ABI.
namespace utils::ps4
{
struct machine_context
{
    std::uint64_t onstack, rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp;
    std::uint64_t r10, r11, r12, r13, r14, r15;
    std::uint32_t trapno;
    std::uint16_t fs, gs;
    std::uint64_t address;
    std::uint32_t flags;
    std::uint16_t es, ds;
    std::uint64_t error, rip, cs, rflags, rsp, ss;
    std::uint64_t length, fpformat, ownedfp, lbrfrom, lbrto, aux1, aux2;
    std::uint64_t fpstate[104];
    std::uint64_t fsbase, gsbase, spare[6];
};

struct signal_context
{
    std::uint32_t signal_mask[4];
    std::uint32_t reserved_prefix[12];
    machine_context machine;
    signal_context* link;
    struct { void* base; std::uint64_t size; std::uint32_t flags, padding; } stack;
    std::uint32_t flags, spare[4], reserved_suffix[3];

    std::uint64_t* register_pointer(unsigned index)
    {
        switch (index)
        {
        case 0: return &machine.rax;
        case 1: return &machine.rcx;
        case 2: return &machine.rdx;
        case 3: return &machine.rbx;
        case 4: return &machine.rsp;
        case 5: return &machine.rbp;
        case 6: return &machine.rsi;
        case 7: return &machine.rdi;
        case 8: return &machine.r8;
        case 9: return &machine.r9;
        case 10: return &machine.r10;
        case 11: return &machine.r11;
        case 12: return &machine.r12;
        case 13: return &machine.r13;
        case 14: return &machine.r14;
        case 15: return &machine.r15;
        case 16: return &machine.rip;
        default: return nullptr;
        }
    }
};

static_assert(offsetof(signal_context, machine) == 0x40);
static_assert(offsetof(machine_context, address) == 0x88);
static_assert(offsetof(machine_context, error) == 0x98);
static_assert(offsetof(machine_context, rip) == 0xa0);
static_assert(offsetof(machine_context, rsp) == 0xb8);
static_assert(offsetof(machine_context, fpstate) == 0x100);
static_assert(sizeof(machine_context) == 0x480);
static_assert(sizeof(signal_context) == 0x500);
}
