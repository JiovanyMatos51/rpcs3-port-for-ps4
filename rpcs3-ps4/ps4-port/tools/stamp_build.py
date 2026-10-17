"""Writes the build stamp into the native ELF (before create-fself).

The ELF holds one 64-byte array: the marker below, a 32-byte payload (placeholder 0x00..0x1f when linked) and 8 zero
bytes (rpcs3/util/ps4_spu_object_cache.hpp, defined in rpcs3/Emu/Cell/SPULLVMRecompiler.cpp). The payload becomes the
SHA-256 of the file bytes of every PT_LOAD segment, computed with the payload zeroed, so running this again on the same
ELF writes the same value. The persistent SPU object cache keys its packs with it: a build that changes any loaded
byte (code, constant tables, data) never reuses objects of another build. An unstamped ELF keeps that cache off.

Usage: python -I stamp_build.py <rpcs3_ps4.elf>   (prints the 64 hex digits; exit 1 on any problem)
"""

import hashlib
import struct
import sys

MARKER = b"RPCS3PS4-BUILD-STAMP-v1:"
PAYLOAD_SIZE = 32
PT_LOAD = 1


def fail(message):
    print("stamp_build: " + message, file=sys.stderr)
    sys.exit(1)


def load_segments(data):
    if len(data) < 64 or data[:4] != b"\x7fELF":
        fail("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        fail("not a 64-bit little-endian ELF")
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    if phentsize < 56 or phoff + phentsize * phnum > len(data):
        fail("bad program header table")
    segments = []
    for i in range(phnum):
        base = phoff + i * phentsize
        p_type = struct.unpack_from("<I", data, base)[0]
        p_offset = struct.unpack_from("<Q", data, base + 8)[0]
        p_filesz = struct.unpack_from("<Q", data, base + 32)[0]
        if p_type != PT_LOAD or not p_filesz:
            continue
        if p_offset + p_filesz > len(data):
            fail("PT_LOAD segment %u extends past the end of the file" % i)
        segments.append((p_offset, p_filesz))
    if not segments:
        fail("no PT_LOAD segment")
    return sorted(segments)


def main(argv):
    if len(argv) != 2:
        fail("usage: stamp_build.py <elf>")
    path = argv[1]
    with open(path, "rb") as f:
        data = bytearray(f.read())

    segments = load_segments(data)
    found = set()
    for offset, size in segments:
        end = offset + size
        at = data.find(MARKER, offset, end)
        while at != -1:
            if at + len(MARKER) + PAYLOAD_SIZE <= end:
                found.add(at)
            at = data.find(MARKER, at + 1, end)

    if len(found) != 1:
        fail("expected exactly one build stamp marker in the loaded segments, found %u" % len(found))

    payload_at = found.pop() + len(MARKER)
    data[payload_at:payload_at + PAYLOAD_SIZE] = bytes(PAYLOAD_SIZE)

    digest = hashlib.sha256()
    for offset, size in segments:
        digest.update(data[offset:offset + size])
    stamp = digest.digest()

    if stamp == bytes(range(PAYLOAD_SIZE)) or stamp == bytes(PAYLOAD_SIZE):
        fail("digest equals the unstamped placeholder")

    # 32 bytes patched in place (no second copy of the ELF on disk)
    with open(path, "r+b") as f:
        f.seek(payload_at)
        f.write(stamp)
        f.flush()

    with open(path, "rb") as f:
        f.seek(payload_at)
        if f.read(PAYLOAD_SIZE) != stamp:
            fail("the stamp did not read back")

    print(stamp.hex())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
