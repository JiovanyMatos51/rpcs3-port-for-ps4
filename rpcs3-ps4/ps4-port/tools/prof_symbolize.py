#!/usr/bin/env python3
"""Annotate the PROF lines of a console startup.log (0.29 sampling profiler, rpcs3/ps4_profiler.cpp) with function names.

Usage:
    python ps4-port/tools/prof_symbolize.py <startup.log | -> [rpcs3_ps4.elf] [--all]

The ELF must be the exact build that produced the log (the 'PROF burst' header prints the build version; keep a copy
of rpcs3_ps4.elf for every packaged build). "elf+0x..." / "elf~0x..." offsets are relative to __text_start, which is
virtual address 0 of rpcs3_ps4.elf. With --all, the STALL host samples ('STALL RSX thread: host rip=+0x... frames:
+0x...') and fatal.txt-style backtraces ('#N +0x...') in the same file are annotated too.
Host-only: nothing here talks to the console.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
# Default: the llvm-mingw toolchain in a folder next to this checkout.
SYMBOLIZER = os.environ.get('LLVM_SYMBOLIZER',
                            os.path.join(os.path.dirname(REPO), 'RoberixUSB', 'tools', 'llvm-mingw-20240619-ucrt-x86_64',
                                         'bin', 'llvm-symbolizer.exe'))
DEFAULT_ELF = os.path.join(REPO, 'ps4-port', '.build-abi', 'ps4-port', 'rpcs3_ps4.elf')
PROF_ADDR = re.compile(r'elf[+~](0x[0-9a-f]+)')
TEXT_ADDR = re.compile(r'(?<![0-9A-Za-z_])\+(0x[0-9a-f]+)')
MESA_PREFIXES = ('radv_', 'nir_', 'aco::', 'vk_', 'ac_', 'si_', 'util_', 'ralloc', 'rzalloc', '_mesa_', 'glsl_', 'spirv_',
                 'vtn_', 'disk_cache', 'simple_mtx', 'futex_', 'radeon_', 'zink_', 'st_', 'u_', 'pipe_')


def owner(name, path):
    p = path.replace(chr(92), '/').lower()
    if '/mesa' in p or '/src/amd/' in p or '/src/compiler/' in p or '/src/vulkan/' in p or name.startswith(MESA_PREFIXES):
        return 'mesa'
    if name.startswith('llvm::') or '/llvm/' in p:
        return 'llvm'
    if name.startswith('std::'):
        return 'libc++'
    if name.startswith(('orbis', 'ps4_')) or '/orbis-compat/' in p or '/ps4-port/' in p:
        return 'port'
    return 'rpcs3'


def symbolize(elf, addrs):
    """Returns {address: (function, file)} for the given hex strings (one llvm-symbolizer run)."""
    names = {}
    if not addrs:
        return names
    try:
        out = subprocess.run([SYMBOLIZER, '--obj=' + elf, '--functions=linkage', '--demangle', '--no-inlines'],
                             input='\n'.join(addrs) + '\n', capture_output=True, text=True, check=False).stdout
    except OSError as error:
        print('llvm-symbolizer not runnable (%s): set LLVM_SYMBOLIZER' % error, file=sys.stderr)
        return names
    blocks = [b.splitlines() for b in out.strip().split('\n\n')]
    for a, b in zip(addrs, blocks):
        names[a] = (b[0] if b else '??', b[1] if len(b) > 1 else '')
    return names


def main():
    args = [a for a in sys.argv[1:] if a != '--all']
    everything = '--all' in sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    if args[0] == '-':
        text = sys.stdin.read()
    else:
        with open(args[0], encoding='utf-8', errors='replace') as f:
            text = f.read()
    elf = args[1] if len(args) > 1 else DEFAULT_ELF
    if not os.path.isfile(elf):
        print('ELF not found: %s' % elf, file=sys.stderr)
        return 2

    def selected(line):
        if ' PROF ' in line or line.startswith('PROF '):
            return 'prof'
        if everything and ('STALL RSX thread' in line or re.match(r'\s*#\d+ \+0x', line)):
            return 'text'
        return None

    lines = [(l, selected(l)) for l in text.splitlines()]
    lines = [(l, kind) for l, kind in lines if kind]
    addrs = set()
    for l, kind in lines:
        pattern = PROF_ADDR if kind == 'prof' else TEXT_ADDR
        addrs.update(m.group(1) for m in pattern.finditer(l))
    names = symbolize(elf, sorted(addrs, key=lambda a: int(a, 16)))

    def sub(m):
        name, path = names.get(m.group(1), ('??', ''))
        return '%s [%s] %s' % (m.group(0), owner(name, path), name[:120])

    for l, kind in lines:
        print((PROF_ADDR if kind == 'prof' else TEXT_ADDR).sub(sub, l))
    return 0


if __name__ == '__main__':
    sys.exit(main())
