#pragma once

#ifdef __ORBIS__
// OpenOrbis 0.5.4 mixes a BSD sockaddr/sockaddr_in (length, family) with
// a Linux sockaddr_storage (family at offset 0). Native accept/recvfrom/
// getsockname/getpeername all return the BSD layout. Keep the SDK function
// declarations and replace only its incompatible storage structure.
#define sockaddr_storage openorbis_legacy_sockaddr_storage
#include_next <sys/socket.h>
#undef sockaddr_storage

struct sockaddr_storage
{
    unsigned char ss_len;
    sa_family_t ss_family;
    char __ss_pad1[sizeof(unsigned long) - 2];
    unsigned long __ss_align;
    char __ss_pad2[128 - 2 * sizeof(unsigned long)];
};

// The SDK also gives the MSG_* flags their Linux numbers, which this FreeBSD-derived kernel reads differently:
// Linux MSG_DONTWAIT 0x40 is FreeBSD MSG_WAITALL, Linux MSG_WAITALL 0x100 is MSG_EOF (shuts the TCP send side
// down), Linux MSG_NOSIGNAL 0x4000 is MSG_NBIO. Use FreeBSD's values, and drop the Linux-only flags so no caller
// can pass a number the kernel would misread.
#undef MSG_OOB
#undef MSG_PEEK
#undef MSG_DONTROUTE
#undef MSG_EOR
#undef MSG_TRUNC
#undef MSG_CTRUNC
#undef MSG_WAITALL
#undef MSG_DONTWAIT
#undef MSG_NOSIGNAL
#define MSG_OOB       0x00001
#define MSG_PEEK      0x00002
#define MSG_DONTROUTE 0x00004
#define MSG_EOR       0x00008
#define MSG_TRUNC     0x00010
#define MSG_CTRUNC    0x00020
#define MSG_WAITALL   0x00040
#define MSG_DONTWAIT  0x00080
#define MSG_EOF       0x00100
#define MSG_NOSIGNAL  0x20000

#undef MSG_PROXY
#undef MSG_FIN
#undef MSG_SYN
#undef MSG_CONFIRM
#undef MSG_RST
#undef MSG_ERRQUEUE
#undef MSG_MORE
#undef MSG_WAITFORONE
#undef MSG_BATCH
#undef MSG_ZEROCOPY
#undef MSG_FASTOPEN
#undef MSG_CMSG_CLOEXEC

#ifdef __cplusplus
#include <cstddef>
static_assert(sizeof(sa_family_t) == 1);
static_assert(sizeof(sockaddr_storage) == 128);
static_assert(alignof(sockaddr_storage) == 8);
static_assert(offsetof(sockaddr_storage, ss_family) == offsetof(sockaddr, sa_family));
static_assert(offsetof(sockaddr_storage, ss_family) == 1);
static_assert(offsetof(sockaddr_storage, __ss_align) == 8);
#endif
#else
#include_next <sys/socket.h>
#endif
