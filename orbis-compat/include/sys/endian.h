/* SPDX-License-Identifier: MIT */
#ifndef _ORBIS_SYS_ENDIAN_H
#define _ORBIS_SYS_ENDIAN_H

/*
 * <sys/endian.h> - the BSD spelling, which this target asks for and the SDK does not have.
 *
 * clang with --target=x86_64-pc-freebsd12-elf predefines __FreeBSD__ 12, so portable libraries
 * take their FreeBSD arm and include <sys/endian.h>, which the SDK lacks (it only ships the glibc
 * spelling, <endian.h>). Examples:
 *
 *   * SDL2: orbis-ports/SDL commit d2f6ea6ff adds && !defined(__ORBIS__) to SDL_endian.h, but
 *     __ORBIS__ comes from the kit's toolchain file, so other build systems still fail.
 *   * Mesa: u_endian.h fell through to the FreeBSD arm and wanted <machine/endian.h>.
 *
 * The SDK's <endian.h> already defines BYTE_ORDER, LITTLE_ENDIAN, BIG_ENDIAN, PDP_ENDIAN and the
 * htobeNN/beNNtoh/htoleNN/leNNtoh family, so it is included rather than restated. FreeBSD's extras
 * are added here:
 *
 *   bswap16/32/64   musl's __bswapNN under FreeBSD's names
 *   be16enc/dec     load/store helpers that go through unsigned char, so they are correct for
 *   le16enc/dec     any alignment and aliasing, as FreeBSD documents
 *   (and 32/64)
 */

#include <stdint.h>
#include <endian.h>

/* FreeBSD's spelling of the swaps musl already provides as __bswapNN. */
#define bswap16(x) __bswap16(x)
#define bswap32(x) __bswap32(x)
#define bswap64(x) __bswap64(x)

static __inline uint16_t be16dec(const void *_p)
{
	const unsigned char *_c = (const unsigned char *)_p;
	return (uint16_t)((uint16_t)_c[0] << 8 | (uint16_t)_c[1]);
}

static __inline uint32_t be32dec(const void *_p)
{
	const unsigned char *_c = (const unsigned char *)_p;
	return (uint32_t)_c[0] << 24 | (uint32_t)_c[1] << 16 |
	       (uint32_t)_c[2] << 8  | (uint32_t)_c[3];
}

static __inline uint64_t be64dec(const void *_p)
{
	const unsigned char *_c = (const unsigned char *)_p;
	return (uint64_t)be32dec(_c) << 32 | be32dec(_c + 4);
}

static __inline uint16_t le16dec(const void *_p)
{
	const unsigned char *_c = (const unsigned char *)_p;
	return (uint16_t)((uint16_t)_c[1] << 8 | (uint16_t)_c[0]);
}

static __inline uint32_t le32dec(const void *_p)
{
	const unsigned char *_c = (const unsigned char *)_p;
	return (uint32_t)_c[3] << 24 | (uint32_t)_c[2] << 16 |
	       (uint32_t)_c[1] << 8  | (uint32_t)_c[0];
}

static __inline uint64_t le64dec(const void *_p)
{
	const unsigned char *_c = (const unsigned char *)_p;
	return (uint64_t)le32dec(_c + 4) << 32 | le32dec(_c);
}

static __inline void be16enc(void *_p, uint16_t _v)
{
	unsigned char *_c = (unsigned char *)_p;
	_c[0] = (unsigned char)((_v >> 8) & 0xff);
	_c[1] = (unsigned char)(_v & 0xff);
}

static __inline void be32enc(void *_p, uint32_t _v)
{
	unsigned char *_c = (unsigned char *)_p;
	_c[0] = (unsigned char)((_v >> 24) & 0xff);
	_c[1] = (unsigned char)((_v >> 16) & 0xff);
	_c[2] = (unsigned char)((_v >> 8) & 0xff);
	_c[3] = (unsigned char)(_v & 0xff);
}

static __inline void be64enc(void *_p, uint64_t _v)
{
	unsigned char *_c = (unsigned char *)_p;
	be32enc(_c, (uint32_t)(_v >> 32));
	be32enc(_c + 4, (uint32_t)(_v & 0xffffffffU));
}

static __inline void le16enc(void *_p, uint16_t _v)
{
	unsigned char *_c = (unsigned char *)_p;
	_c[0] = (unsigned char)(_v & 0xff);
	_c[1] = (unsigned char)((_v >> 8) & 0xff);
}

static __inline void le32enc(void *_p, uint32_t _v)
{
	unsigned char *_c = (unsigned char *)_p;
	_c[0] = (unsigned char)(_v & 0xff);
	_c[1] = (unsigned char)((_v >> 8) & 0xff);
	_c[2] = (unsigned char)((_v >> 16) & 0xff);
	_c[3] = (unsigned char)((_v >> 24) & 0xff);
}

static __inline void le64enc(void *_p, uint64_t _v)
{
	unsigned char *_c = (unsigned char *)_p;
	le32enc(_c, (uint32_t)(_v & 0xffffffffU));
	le32enc(_c + 4, (uint32_t)(_v >> 32));
}

#endif /* _ORBIS_SYS_ENDIAN_H */
