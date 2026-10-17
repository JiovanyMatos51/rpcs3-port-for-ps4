#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

extern "C" int sceKernelGetModuleList(std::uint32_t*, std::size_t, std::size_t*);
extern "C" int sceKernelDlsym(int, const char*, void**);

namespace llvm::sys::ps4
{
// The OpenOrbis static libc has no ELF dynamic loader. The current process
// consists of its statically registered symbols plus the kernel's loaded PRX
// modules. This object represents that borrowed process symbol namespace;
// it does not own or unload any of those modules.
class process_symbols
{
    std::vector<std::uint32_t> modules;
    std::mutex mutex;

    bool refresh_locked(std::string* error)
    {
        modules.resize(256);
        for (;;)
        {
            std::size_t available = 0;
            const int result = sceKernelGetModuleList(modules.data(), modules.size(), &available);
            if (result != 0)
            {
                modules.clear();
                if (error) *error = "sceKernelGetModuleList failed: " + std::to_string(result);
                return false;
            }
            if (available <= modules.size())
            {
                modules.resize(available);
                if (error) error->clear();
                return true;
            }
            // A corrupt or continuously growing list must not cause unbounded
            // allocation. Refresh also handles modules loaded after startup.
            if (available > 4096)
            {
                modules.clear();
                if (error) *error = "PS4 process module list exceeds 4096 entries";
                return false;
            }
            modules.resize(available);
        }
    }

public:
    bool refresh(std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return refresh_locked(error);
    }

    void* find(const char* symbol)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!refresh_locked(nullptr)) return nullptr;
        for (const auto module : modules)
        {
            void* address = nullptr;
            if (sceKernelDlsym(static_cast<int>(module), symbol, &address) == 0 && address)
                return address;
        }
        return nullptr;
    }
};
}
