#include "../../rpcs3/util/ps4_signal_context.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <limits>

int main()
{
    utils::ps4::signal_context context{};
    auto* bytes = reinterpret_cast<unsigned char*>(&context);
    // Independent byte offsets from the native ABI, not offsetof the adapter.
    constexpr std::array<std::size_t, 17> offsets{
        0x78, 0x60, 0x58, 0x80, 0xf8, 0x88, 0x50, 0x48,
        0x68, 0x70, 0x90, 0x98, 0xa0, 0xa8, 0xb0, 0xb8, 0xe0};
    for (unsigned int reg = 0; reg < offsets.size(); ++reg)
    {
        const std::uint64_t value = 0x800000000ull + 0x1000 * reg + 0x123;
        std::memcpy(bytes + offsets[reg], &value, sizeof(value));
    }
    for (unsigned int reg = 0; reg < offsets.size(); ++reg)
    {
        auto* value = context.register_pointer(reg);
        assert(value && *value == 0x800000000ull + 0x1000 * reg + 0x123);
        assert(reinterpret_cast<unsigned char*>(value) == bytes + offsets[reg]);
        *value += 7;
        std::uint64_t observed{};
        std::memcpy(&observed, bytes + offsets[reg], sizeof(observed));
        assert(observed == 0x800000000ull + 0x1000 * reg + 0x12a);
    }
    assert(!context.register_pointer(17));
    assert(!context.register_pointer(std::numeric_limits<unsigned int>::max()));

    const std::uint64_t error = 0x12, address = 0x8'12345678, flags = 0x202;
    const std::uint32_t trap = 14;
    std::memcpy(bytes + 0xd8, &error, sizeof(error));
    std::memcpy(bytes + 0xc8, &address, sizeof(address));
    std::memcpy(bytes + 0xf0, &flags, sizeof(flags));
    std::memcpy(bytes + 0xc0, &trap, sizeof(trap));
    assert(context.machine.error == 0x12 && context.machine.address == address);
    assert(context.machine.rflags == 0x202 && context.machine.trapno == 14);

    // Each XMM register must address the inline FXSAVE bank, not a Linux
    // fpregs pointer read from the kernel frame's integer register area.
    for (unsigned int xmm = 0; xmm < 16; ++xmm)
    {
        auto* slot = reinterpret_cast<unsigned char*>(context.machine.fpstate) + 160 + 16 * xmm;
        assert(slot == bytes + 0x1e0 + 16 * xmm);
        slot[0] = static_cast<unsigned char>(xmm + 1);
        assert(bytes[0x1e0 + 16 * xmm] == xmm + 1);
    }
}
