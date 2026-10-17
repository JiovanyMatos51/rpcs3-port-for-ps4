"""Reject a native ELF whose required Mesa handlers were dropped at link time."""

import argparse
from pathlib import Path
import struct


FENV_DUMMY_SIZE = 3  # libc.a(fenv.lo): "xor eax,eax; ret"


def _uses_mxcsr(code, reg):
    """True if code contains (v)ldmxcsr (reg 2) or (v)stmxcsr (reg 3) with a memory operand."""
    for prefix in (b"\x0f\xae", b"\xc5\xf8\xae"):
        start = code.find(prefix)
        while start >= 0:
            index = start + len(prefix)
            if index < len(code) and code[index] >> 6 != 3 and (code[index] >> 3) & 7 == reg:
                return True
            start = code.find(prefix, start + 1)
    return False


def verify_fenv(data, sections, symbols):
    """Reject libc.a's dummy fenv (cause of the 0.24 black screen).

    The OpenOrbis libc.a(fenv.lo) defines fegetround/fetestexcept/__fesetround/... as 3-byte
    "xor eax,eax; ret" stubs, so spu_thread::cpu_task never got MXCSR.RC = round toward zero and the
    SPU recompilers' CLZ was off by one. ps4-port/runtime/ps4_fenv.cpp must provide the real ones.
    """
    def code(name):
        shndx, address, size = symbols.get(name, (0, 0, 0))
        if not shndx or not address or shndx >= len(sections) or not sections[shndx][2] & 4:
            raise ValueError(f"fenv function {name} is missing from executable code "
                             "(ps4-port/runtime/ps4_fenv.cpp not linked?)")
        if size <= FENV_DUMMY_SIZE:
            raise ValueError(f"{name} is libc.a's dummy fenv stub ({size} bytes): SPU recompilers need a "
                             "functional fenv (ps4-port/runtime/ps4_fenv.cpp in the rpcs3_ps4_frontend OBJECT target)")
        section = sections[shndx]
        offset = section[4] + address - section[3]
        return data[offset:offset + size]

    for name in ("fegetround", "fetestexcept"):
        if not _uses_mxcsr(code(name), 3):
            raise ValueError(f"{name} does not read MXCSR (stmxcsr): not the functional x86-64 fenv")
        print(f"Verified fenv: {name} = 0x{symbols[name][1]:x} ({symbols[name][2]} bytes, reads MXCSR)")
    # __fesetround is only referenced by libc.a(fesetround.lo); ps4_fenv.cpp defines it as well, and
    # its fesetround may have it inlined. Either one must really write MXCSR.
    setround = code("fesetround")
    inner = code("__fesetround") if "__fesetround" in symbols else b""
    if not _uses_mxcsr(setround, 2) and not _uses_mxcsr(inner, 2):
        raise ValueError("fesetround/__fesetround never write MXCSR (ldmxcsr): libc.a's fesetround.lo + dummy "
                         "__fesetround were linked instead of ps4-port/runtime/ps4_fenv.cpp")
    print(f"Verified fenv: fesetround = 0x{symbols['fesetround'][1]:x} ({symbols['fesetround'][2]} bytes, writes MXCSR)")
    # Every other fenv.lo symbol that made it into the ELF must not be a dummy either.
    for name in ("__fesetround", "feclearexcept", "feraiseexcept", "fegetenv", "fesetenv"):
        if name in symbols and symbols[name][0]:
            code(name)
    return 3


def verify(path, opengl=False, llvm=False):
    data = Path(path).read_bytes()
    if data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("Expected little-endian ELF64")
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
    if header[2] != 62:
        raise ValueError("Expected x86-64 ELF")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, header[6] + i * header[11])
                for i in range(header[12])]

    def contents(section):
        return data[section[4]:section[4] + section[5]]

    symbols = {}
    symbol_tables = {}
    for index, section in enumerate(sections):
        if section[1] not in (2, 11):  # SYMTAB / DYNSYM
            continue
        strings = contents(sections[section[6]])
        table = []
        for offset in range(section[4], section[4] + section[5], section[9]):
            name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", data, offset)
            name = strings[name:strings.find(b"\0", name)].decode("utf-8")
            entry = (shndx, value, size)
            table.append(entry)
            if name and (name not in symbols or shndx):
                symbols[name] = entry
        symbol_tables[index] = table

    relocations = {}
    for section in sections:
        if section[1] != 4:  # RELA
            continue
        for offset in range(section[4], section[4] + section[5], section[9]):
            target, info, addend = struct.unpack_from("<QQq", data, offset)
            kind, symbol = info & 0xffffffff, info >> 32
            if kind == 8:  # R_X86_64_RELATIVE, using a zero module base
                relocations[target] = addend
            elif kind == 1:  # R_X86_64_64
                relocations[target] = symbol_tables[section[6]][symbol][1] + addend

    table_section, table_address, table_size = symbols["vk_common_physical_device_entrypoints"]
    if not table_section or not table_size or table_size % 8:
        raise ValueError("Mesa physical-device dispatch table is missing or malformed")
    section = sections[table_section]
    raw_offset = section[4] + table_address - section[3]
    handlers = set()
    for offset in range(0, table_size, 8):
        slot = table_address + offset
        handlers.add(relocations.get(slot, struct.unpack_from("<Q", data, raw_offset + offset)[0]))

    required = (
        "vk_common_GetPhysicalDeviceProperties2",
        "vk_common_GetPhysicalDeviceFeatures2",
        "vk_common_GetPhysicalDeviceQueueFamilyProperties",
        "vk_common_GetPhysicalDeviceMemoryProperties",
    )
    for name in required:
        shndx, address, _ = symbols.get(name, (0, 0, 0))
        if not shndx or not address or not sections[shndx][2] & 4:
            raise ValueError(f"Required Mesa handler absent from executable code: {name}")
        if address not in handlers:
            raise ValueError(f"Mesa dispatch table does not reference its handler: {name}")
        print(f"Verified Mesa dispatch: {name} = 0x{address:x}")
    count = len(required)
    count += verify_fenv(data, sections, symbols)
    if opengl:
        gl_required = ("eglInitialize", "eglCreateContext", "eglMakeCurrent", "eglSwapBuffers",
                       "eglGetProcAddress", "dri2_initialize_orbis", "zink_create_screen",
                       "_mesa_glapi_get_proc_address", "_ZN10GLGSRender14on_init_threadEv")
        for name in gl_required:
            shndx, address, size = symbols.get(name, (0, 0, 0))
            if not shndx or not address or size < 6 or not sections[shndx][2] & 4:
                raise ValueError(f"Required original OpenGL/Mesa implementation missing: {name}")
            print(f"Verified OpenGL code: {name} = 0x{address:x} ({size} bytes)")
        count += len(gl_required)
    if llvm:
        llvm_required = (
            "LLVMInitializeX86TargetInfo", "LLVMInitializeX86Target",
            "LLVMInitializeX86TargetMC", "LLVMInitializeX86AsmPrinter",
            "LLVMInitializeX86AsmParser",
            "_ZN13PPUTranslator9TranslateERK12ppu_function",
            "_ZN19spu_recompiler_base20make_llvm_recompilerEh",
            "_ZN19spu_llvm_recompiler7compileEO11spu_program",
        )
        for name in llvm_required:
            shndx, address, size = symbols.get(name, (0, 0, 0))
            if not shndx or not address or size < 6 or not sections[shndx][2] & 4:
                raise ValueError(f"Required original LLVM compiler implementation missing: {name}")
            print(f"Verified LLVM compiler code: {name} = 0x{address:x} ({size} bytes)")
        count += len(llvm_required)
    return count


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--opengl", action="store_true")
    parser.add_argument("--llvm", action="store_true")
    args = parser.parse_args()
    try:
        count = verify(args.elf, args.opengl, args.llvm)
    except (ValueError, KeyError, IndexError, struct.error) as error:
        parser.exit(1, f"Native dispatch validation FAILED: {error}\n")
    print(f"Native dispatch validation passed ({count} required handlers)")
