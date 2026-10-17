#include "../compat/llvm_ps4_process.hpp"
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <algorithm>

static std::atomic<std::size_t> module_count{300};
static std::atomic<int> kernel_error{0};
static int kernel_function;

extern "C" int sceKernelGetModuleList(std::uint32_t* ids, std::size_t capacity, std::size_t* count)
{
    if (const int error = kernel_error.load()) return error;
    *count = module_count.load();
    for (std::size_t i = 0; i < std::min(capacity, *count); ++i) ids[i] = static_cast<std::uint32_t>(i + 1);
    return 0;
}

extern "C" int sceKernelDlsym(int id, const char* name, void** address)
{
    // Return garbage on failure to verify that only successful native lookups
    // become callable addresses. Module 300 tests enumeration beyond 256.
    *address = reinterpret_cast<void*>(0x1234);
    if (id == 300 && std::strcmp(name, "kernel_function") == 0)
    { *address = &kernel_function; return 0; }
    return -1;
}

int main()
{
    llvm::sys::ps4::process_symbols process;
    std::string error;
    assert(process.refresh(&error) && error.empty());
    assert(process.find("kernel_function") == &kernel_function);
    assert(process.find("missing") == nullptr);
    module_count = 299;
    assert(process.find("kernel_function") == nullptr);
    module_count = 300;
    assert(process.find("kernel_function") == &kernel_function);
    kernel_error = -22;
    assert(!process.refresh(&error) && error.find("-22") != std::string::npos);
    assert(process.find("kernel_function") == nullptr);
    kernel_error = 0;
    module_count = 5000;
    assert(!process.refresh(&error) && error.find("4096") != std::string::npos);
    module_count = 0;
    assert(process.refresh(&error) && error.empty());
    assert(process.find("kernel_function") == nullptr);
    module_count = 300;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&]
    {
        for (int j = 0; j < 250; ++j)
        {
            std::string thread_error;
            assert(process.refresh(&thread_error));
            assert(process.find("kernel_function") == &kernel_function);
        }
    });
    for (auto& worker : workers) worker.join();
    std::cout << "Native LLVM process symbol adapter: module growth/removal, errors, missing symbols and concurrent lookup passed\n";
}
