#pragma once

#if defined(__ORBIS__) && !defined(SOCK_CLOEXEC)
// The SDK carries the Linux numbers for SOCK_CLOEXEC/SOCK_NONBLOCK (sys/socket.h:94-97); this FreeBSD-based
// kernel would read them as socket type bits. Defining SOCK_CLOEXEC first makes sys/socket.h skip both, so
// curl does not OR them into socket() (cf-socket.c:426, 1212), keeps CURL_USE_SOCK_NONBLOCK undefined and
// skips its fcntl(F_SETFD) fallback (cf-socket.c:479). Must precede netinet/in.h, which includes sys/socket.h.
#define SOCK_CLOEXEC 0
#endif

#include <netinet/in.h>
#include <sys/socket.h>

// The OpenOrbis headers expose IP_BIND_ADDRESS_NO_PORT but omit the Linux
// alias for the IPv4 socket-option level. IPPROTO_IP is the standard name
// for the same level. curl already treats failure of this optional hint as
// nonfatal and proceeds with ordinary bind().
#if defined(__ORBIS__) && !defined(SOL_IP)
#define SOL_IP IPPROTO_IP
#endif

#if defined(__ORBIS__)
// Sony's SO_NBIO, proven by the boot probe ('network ABI: SO_NBIO 0x1200 rc 0/0 ... connect -1/36'). curl's
// HAVE_SETSOCKOPT_SO_NONBLOCK branch (curlx/nonblock.c) uses it; fcntl(F_SETFL) and FIONBIO are refused.
#ifndef SO_NONBLOCK
#define SO_NONBLOCK 0x1200
#endif

#ifdef BUILDING_LIBCURL // libcurl sources only (lib/CMakeLists.txt); feature checks never see this
// curlx_nonblock passes a long (8 bytes); the probe proved an int optval, so hand the kernel exactly that.
// noinline: each object that calls it keeps a local symbol, so a build check can find it in libcurl.a.
static inline __attribute__((noinline)) int rpcs3_ps4_curl_setsockopt(int fd, int level, int name, const void* value, socklen_t length)
{
	if (level == SOL_SOCKET && name == SO_NONBLOCK && value && length == sizeof(long))
	{
		const int flag = *(const long*)value != 0;
		return setsockopt(fd, level, name, &flag, sizeof(flag));
	}
	return setsockopt(fd, level, name, value, length);
}
#define setsockopt rpcs3_ps4_curl_setsockopt
#endif
#endif
