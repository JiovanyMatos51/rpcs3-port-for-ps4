#pragma once

#ifdef LLVM_AVAILABLE
#include "Utilities/JIT.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"

namespace utils::ps4
{
// Run a small function through RPCS3's actual compiler and executable-memory
// manager before offering LLVM decoders. Exercises the SysV host-call ABI as
// well as generated arithmetic, not merely the presence of a library symbol.
inline u64 verify_llvm_host_call(u64 a, u64 b)
{
    return (a * 17 + b) ^ 0x1933;
}

inline bool verify_llvm(std::string& error)
{
    jit_compiler compiler({{"ps4_llvm_host_call", reinterpret_cast<u64>(&verify_llvm_host_call)}}, "btver2", 1);
    auto& context = compiler.get_context();
    auto module = std::make_unique<llvm::Module>("ps4_llvm_verification", context);
    module->setTargetTriple(llvm::Triple(jit_compiler::triple1()));
    module->setDataLayout(compiler.get_engine().getDataLayout());
    llvm::IRBuilder<> ir(context);
    auto* type = llvm::FunctionType::get(ir.getInt64Ty(), {ir.getInt64Ty(), ir.getInt64Ty()}, false);
    auto* function = llvm::Function::Create(type, llvm::Function::ExternalLinkage, "ps4_llvm_verification", *module);
    auto* helper = llvm::Function::Create(type, llvm::Function::ExternalLinkage, "ps4_llvm_host_call", *module);
    ir.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
    auto* product = ir.CreateMul(function->getArg(0), ir.getInt64(3));
    ir.CreateRet(ir.CreateCall(helper, {product, function->getArg(1)}));
    if (!compiler.try_add(std::move(module), error) || !compiler.try_fin(error)) return false;
    auto compiled = reinterpret_cast<u64(*)(u64, u64)>(compiler.get("ps4_llvm_verification"));
    if (!compiled) { error = "LLVM did not emit its native verification function"; return false; }
    for (const auto& [a, b] : std::array<std::pair<u64, u64>, 4>{{{0, 0}, {13, 7}, {0x123456789abcdef, 0xfedcba9876543210}, {umax, 1}}})
    {
        if (compiled(a, b) != verify_llvm_host_call(a * 3, b))
        { error = "LLVM generated code or its native SysV call returned incorrect data"; return false; }
    }
    return true;
}
}
#endif
