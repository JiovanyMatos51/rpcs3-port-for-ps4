#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "util/types.hpp"
#include "Utilities/mutex.h"

#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#else
#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#ifdef __clang__
#pragma GCC diagnostic pop
#endif
#endif

#ifdef _WIN32
using socket_type = uptr;
#else
using socket_type = int;
#endif

template <>
struct std::hash<std::array<u8, 16>>
{
	std::size_t operator()(const std::array<u8, 16>& array) const noexcept
	{
		std::size_t hash = 0;
		std::hash<std::uint8_t> hasher;

		for (auto byte : array)
		{
			hash ^= hasher(byte) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
		}

		return hash;
	}
};

namespace np
{
	class ip_address_translator
	{
	public:
		static bool is_ipv6(u32 addr_be);

		u32 register_ipv6(const std::array<u8, 16>& ipv6_addr);
		sockaddr_in6 get_ipv6_sockaddr(u32 addr_be, u16 port_be);

	private:
		static constexpr u32 translation_range_begin = 0x00'00'00'01;
		static constexpr u32 translation_range_end = 0x00'00'FF'FF;

		shared_mutex mutex;
		u32 cur_addr = translation_range_begin;
		std::unordered_map<std::array<u8, 16>, u32> ipv6_to_ipv4;
		std::vector<std::array<u8, 16>> ipv4_to_ipv6;
	};

	u32 register_ip(std::string_view ip_bytes);

	enum class IPV6_SUPPORT : u8
	{
		IPV6_UNKNOWN,
		IPV6_UNSUPPORTED,
		IPV6_SUPPORTED,
	};

	bool is_ipv6_supported(std::optional<IPV6_SUPPORT> force_state = std::nullopt);
	s32 sendto_possibly_ipv6(socket_type native_socket, const char* data, u32 size, const sockaddr_in* addr, int native_flags);
	sockaddr_in6 sockaddr_to_sockaddr6(const sockaddr_in& addr);
	sockaddr_in sockaddr6_to_sockaddr(const sockaddr_in6& addr);

	void close_socket(socket_type socket);
	void set_socket_non_blocking(socket_type socket);

#ifdef __ORBIS__
	// Boot self-test of the console socket ABI: a loopback UDP socket made non-blocking by set_socket_non_blocking,
	// then one recvfrom(MSG_DONTWAIT) with nothing queued (bounded by a 200 ms SO_RCVTIMEO); that line is written
	// to startup.log here, e.g. "network ABI: non-blocking recvfrom -> -1/35 in 12 us (fcntl ok, FIONBIO ok)".
	// Then the SO_NBIO probe (setsockopt, else sceNetSetsockopt; a plain recvfrom bounded by SO_RCVTIMEO; a TCP
	// connect to 127.0.0.1:1), on a helper thread the caller waits for at most 1 s. Native sockets get SO_NBIO
	// only when it passed. Returns that line, e.g.
	// "network ABI: SO_NBIO 0x1200 rc 0/0 -> plain recvfrom -1/35 in 4 us; connect -1/36 in 30 us -> SO_NBIO on ...".
	std::string ps4_probe_nonblocking_recvfrom();

	// set_socket_non_blocking with its outcome: true when SO_NBIO (once the probe proved it), fcntl or FIONBIO
	// took effect.
	bool ps4_set_socket_non_blocking(socket_type socket);
#endif
} // namespace np
