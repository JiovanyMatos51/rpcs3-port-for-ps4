#pragma once

// 0.31: PS4 network helpers for RPCN (header-only; empty outside the PS4 build).
//
// RPCN and sys_net use the kernel's BSD sockets, which work without any setup. Name resolution is the weak spot:
// the SDK's musl getaddrinfo builds its name-server list from sceNetGetDnsInfo and reads that call's return value
// as the number of servers, so it may end up with no server at all. resolve_ipv4() therefore tries an IP literal
// first, then the system resolver (libSceNet), and getaddrinfo last.
//
// prepare() loads and initialises libSceNet and libSceNetCtl on its first call (never at boot) and writes the results
// to startup.log together with the DNS settings and the socket-option probe below. While no resolver pool exists it
// tries again on a later call, at most once every 5 s, so a failed start is not permanent.
//
// Entry points: the console's loader leaves weak imports from modules other than libkernel NULL (libSceNet and
// libSceNetCtl are loaded at start, yet every weak import from them read NULL, while the strong sceNetGetDnsInfo
// import of the same module was bound), and a later module load does not patch them. prepare() therefore binds each
// entry point from its weak import when the loader bound it, otherwise by name with sceKernelDlsym over the loaded
// modules (libkernel calls this build already imports). An entry point found by neither stays NULL and its caller
// keeps the "not exported" path.
//
// probe_numbering(): the SDK headers carry Linux numbers for the IPPROTO_IP options (IP_TTL 2) and MSG_* flags,
// while the kernel is FreeBSD-derived (IP_TTL 4). On a fresh UDP socket getsockopt(IPPROTO_IP, 4) returns the
// default TTL (64) with FreeBSD numbering, and getsockopt(IPPROTO_IP, 2) returns it with Linux numbering. The
// verdict decides whether sys_net needs to translate those options.
//
// Every system call here is a query, a module load or a socket call; nothing changes system settings.
//
// API (all PS4 only):
//   status prepare();                                           module load + init + binding (retried while there is
//                                                               no resolver pool); results in startup.log
//   int link_state();                                           sceNetCtlGetState, -1 if unavailable (3 = IP obtained)
//   u32 resolve_ipv4(const std::string& host, std::string& how); IPv4 in network order, 0 on failure; 'how' names
//                                                               the layer that answered (or why each one failed)
//   std::string dns_report();                                   sceNetGetDnsInfo + NetCtl IP/DNS strings (logged)
//   std::string probe_numbering();                              IPPROTO_IP numbering verdict (logged)
//   int udp_bind_probe(u16 port);                               0 if INADDR_ANY:port can be bound, else errno

#ifdef RPCS3_PS4

#include "util/types.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#endif
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

// Frontend object (ps4_launcher.cpp), always linked; same declaration as in ps4_memory_governor.hpp.
void ps4_startup_checkpoint(const char* stage);

extern "C"
{
// libSceSysmodule, libSceNet and libSceNetCtl exports (all present in the SDK stubs; libSceNetCtl is linked since
// 0.31). Bound under private names so this header never clashes with the <orbis/*.h> prototypes (sceNetCtlGetState
// has none), and weak so a missing export reads as "not available" instead of failing at load time.
std::uint32_t ps4_net_load_module_internal(std::uint32_t id) __asm__("sceSysmoduleLoadModuleInternal") __attribute__((weak));
std::int32_t ps4_net_init() __asm__("sceNetInit") __attribute__((weak));
std::int32_t ps4_net_pool_create(const char* name, std::int32_t size, std::int32_t flags) __asm__("sceNetPoolCreate") __attribute__((weak));
std::int32_t ps4_net_resolver_create(const char* name, std::int32_t pool, std::int32_t flags) __asm__("sceNetResolverCreate") __attribute__((weak));
// addr: one IPv4 address in network byte order (OrbisNetInAddr). timeout/retry 0 = the library defaults.
std::int32_t ps4_net_resolver_start_ntoa(std::int32_t resolver, const char* host, std::uint32_t* addr, std::int32_t timeout,
	std::int32_t retry, std::int32_t flags) __asm__("sceNetResolverStartNtoa") __attribute__((weak));
std::int32_t ps4_net_resolver_destroy(std::int32_t resolver) __asm__("sceNetResolverDestroy") __attribute__((weak));
// info: primary and secondary IPv4 DNS server (OrbisNetDnsInfo)
std::int32_t ps4_net_get_dns_info(void* info, std::int32_t flags) __asm__("sceNetGetDnsInfo") __attribute__((weak));
std::int32_t ps4_netctl_init() __asm__("sceNetCtlInit") __attribute__((weak));
std::int32_t ps4_netctl_get_state(std::int32_t* state) __asm__("sceNetCtlGetState") __attribute__((weak));
// info: OrbisNetCtlInfo, a union whose largest member is 256 bytes
std::int32_t ps4_netctl_get_info(std::int32_t code, void* info) __asm__("sceNetCtlGetInfo") __attribute__((weak));
// libkernel exports for the lookup by name. Other objects of this build import both strongly already (the profiler
// and the shared-memory code resolve symbols the same way), so this adds no import.
std::int32_t ps4_net_module_list(std::uint32_t* handles, std::size_t size, std::size_t* available) __asm__("sceKernelGetModuleList");
std::int32_t ps4_net_dlsym(std::int32_t handle, const char* symbol, void** address) __asm__("sceKernelDlsym");
}

namespace utils::ps4::net
{
inline constexpr std::uint32_t sysmodule_internal_net = 0x8000001C;    // ORBIS_SYSMODULE_INTERNAL_NET
inline constexpr std::uint32_t sysmodule_internal_netctl = 0x80000009; // ORBIS_SYSMODULE_INTERNAL_NETCTL
inline constexpr int netctl_state_ip_obtained = 3;
inline constexpr int not_exported = -1; // value of a status field whose function the console does not export

// Entry points bound by prepare(); nullptr = not exported (neither the loader nor the lookup by name found it).
struct entry_points
{
	decltype(&ps4_net_init) net_init = nullptr;
	decltype(&ps4_net_pool_create) pool_create = nullptr;
	decltype(&ps4_net_resolver_create) resolver_create = nullptr;
	decltype(&ps4_net_resolver_start_ntoa) resolver_start_ntoa = nullptr;
	decltype(&ps4_net_resolver_destroy) resolver_destroy = nullptr;
	decltype(&ps4_net_get_dns_info) get_dns_info = nullptr;
	decltype(&ps4_netctl_init) netctl_init = nullptr;
	decltype(&ps4_netctl_get_state) netctl_get_state = nullptr;
	decltype(&ps4_netctl_get_info) netctl_get_info = nullptr;
};

inline constexpr unsigned entry_point_count = 9; // members of entry_points

// Raw SCE results of prepare(); negative values are SCE error codes.
struct status
{
	int net_module = not_exported;    // sceSysmoduleLoadModuleInternal(NET)
	int netctl_module = not_exported; // sceSysmoduleLoadModuleInternal(NETCTL)
	int net_init = not_exported;      // sceNetInit (an "already initialised" error is harmless)
	int netctl_init = not_exported;   // sceNetCtlInit
	int pool = not_exported;          // sceNetPoolCreate memory id (>= 0) for the resolver
	unsigned bound = 0;               // entry points bound, of entry_point_count
	unsigned bound_by_name = 0;       // of those, found by sceKernelDlsym because the loader left the import NULL
	entry_points fn{};
};

namespace detail
{
// First loaded module that exports 'name' (sceKernelDlsym by plain name, the lookup the profiler and the
// shared-memory code use on the console), or nullptr.
inline void* find_export(const char* name)
{
	std::uint32_t handles[256]{};
	std::size_t count = 0;

	if (ps4_net_module_list(handles, 256, &count) != 0)
	{
		return nullptr;
	}

	for (std::size_t i = 0; i < count && i < 256; i++)
	{
		void* address = nullptr;

		if (ps4_net_dlsym(static_cast<std::int32_t>(handles[i]), name, &address) == 0 && address)
		{
			return address;
		}
	}

	return nullptr;
}

// Fills an unbound slot from the weak import when the loader bound it, else from the export found by name.
template <typename F>
void bind_export(F& slot, F weak, const char* name, unsigned& by_name)
{
	if (slot)
	{
		return;
	}

	if (weak)
	{
		slot = weak;
	}
	else if (void* address = find_export(name))
	{
		slot = reinterpret_cast<F>(address);
		by_name++;
	}
}

inline std::string ipv4_string(std::uint32_t addr_be)
{
	unsigned char b[4];
	std::memcpy(b, &addr_be, sizeof(b));
	char text[16];
	std::snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
	return text;
}

inline int link_state_now(const entry_points& fn)
{
	if (!fn.netctl_get_state)
	{
		return -1;
	}

	std::int32_t state = -1;
	return fn.netctl_get_state(&state) < 0 ? -1 : state;
}

inline std::string dns_report_now(const entry_points& fn)
{
	std::string out;
	char text[160];

	if (fn.get_dns_info)
	{
		// The SDK struct holds two IPv4 addresses; the buffer is larger in case the system one is too.
		alignas(8) std::uint32_t info[16]{};
		const std::int32_t result = fn.get_dns_info(info, 0);
		std::snprintf(text, sizeof(text), "sceNetGetDnsInfo returned %d (0x%x): %s, %s", result, static_cast<std::uint32_t>(result),
			ipv4_string(info[0]).c_str(), ipv4_string(info[1]).c_str());
		out += text;
	}
	else
	{
		out += "sceNetGetDnsInfo not exported";
	}

	if (!fn.netctl_get_info)
	{
		return out + "; sceNetCtlGetInfo not exported";
	}

	static constexpr struct
	{
		std::int32_t code;
		const char* name;
	} infos[] = {{14, "IP"}, {17, "primary DNS"}, {18, "secondary DNS"}}; // ORBIS_NET_CTL_INFO_*

	for (const auto& entry : infos)
	{
		alignas(8) char info[512]{};
		const std::int32_t result = fn.netctl_get_info(entry.code, info);
		info[sizeof(info) - 1] = '\0';

		if (result < 0)
			std::snprintf(text, sizeof(text), "; netctl %s error 0x%x", entry.name, static_cast<std::uint32_t>(result));
		else
			std::snprintf(text, sizeof(text), "; netctl %s '%.15s'", entry.name, info);

		out += text;
	}

	return out;
}

inline std::string probe_numbering_now()
{
	const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0)
	{
		char text[96];
		std::snprintf(text, sizeof(text), "net probe: UDP socket failed (errno %d); IPPROTO_IP numbering unknown", errno);
		return text;
	}

	int opt4 = -1, opt2 = -1;
	socklen_t len4 = sizeof(opt4), len2 = sizeof(opt2);
	const int r4 = ::getsockopt(fd, IPPROTO_IP, 4, &opt4, &len4);
	const int e4 = r4 ? errno : 0;
	const int r2 = ::getsockopt(fd, IPPROTO_IP, 2, &opt2, &len2);
	const int e2 = r2 ? errno : 0;
	::close(fd);

	const char* verdict = "unknown";

	if (r4 == 0 && opt4 == 64)
		verdict = "FreeBSD (IP_TTL is 4)";
	else if (r2 == 0 && opt2 == 64)
		verdict = "Linux (IP_TTL is 2), as in the SDK headers";

	char text[224];
	std::snprintf(text, sizeof(text), "net probe: getsockopt(IPPROTO_IP, 4) = %d value %d errno %d; getsockopt(IPPROTO_IP, 2) = %d value %d errno %d; numbering: %s",
		r4, opt4, e4, r2, opt2, e2, verdict);
	return text;
}
} // namespace detail

// Loads and initialises libSceNet/libSceNetCtl and binds their entry points (thread-safe), and logs the results, the
// DNS settings and the numbering probe to startup.log. While there is no resolver pool (sceNetResolver needs one), a
// later call repeats the steps that failed, at most once every 5 s. Cheap otherwise. Returns a copy, so a caller
// never reads the state while a retry updates it.
inline status prepare()
{
	static std::mutex mutex;
	static status state{};
	static unsigned attempts = 0;
	static std::chrono::steady_clock::time_point last_attempt{};
	static decltype(&ps4_net_load_module_internal) load_module = nullptr;

	std::lock_guard<std::mutex> lock(mutex);
	const auto now = std::chrono::steady_clock::now();

	if (attempts && (state.pool >= 0 || now - last_attempt < std::chrono::seconds(5)))
	{
		return state;
	}

	attempts++;
	last_attempt = now;
	status& r = state;
	entry_points& fn = r.fn;
	unsigned by_name = r.bound_by_name;

	// The module loads come first: a module that is not in the process yet has nothing to find by name. A load
	// that already succeeded and an entry point already bound are not repeated. The loader is not one of the
	// counted entry points.
	unsigned loader_by_name = 0;
	detail::bind_export(load_module, ps4_net_load_module_internal, "sceSysmoduleLoadModuleInternal", loader_by_name);
	if (load_module && r.net_module < 0)
		r.net_module = static_cast<int>(load_module(sysmodule_internal_net));
	detail::bind_export(fn.net_init, ps4_net_init, "sceNetInit", by_name);
	detail::bind_export(fn.pool_create, ps4_net_pool_create, "sceNetPoolCreate", by_name);
	detail::bind_export(fn.resolver_create, ps4_net_resolver_create, "sceNetResolverCreate", by_name);
	detail::bind_export(fn.resolver_start_ntoa, ps4_net_resolver_start_ntoa, "sceNetResolverStartNtoa", by_name);
	detail::bind_export(fn.resolver_destroy, ps4_net_resolver_destroy, "sceNetResolverDestroy", by_name);
	detail::bind_export(fn.get_dns_info, ps4_net_get_dns_info, "sceNetGetDnsInfo", by_name);
	if (fn.net_init && r.net_init < 0)
		r.net_init = fn.net_init();
	if (fn.pool_create && r.pool < 0)
		r.pool = fn.pool_create("rpcs3_net", 16 * 1024, 0);
	if (load_module && r.netctl_module < 0)
		r.netctl_module = static_cast<int>(load_module(sysmodule_internal_netctl));
	detail::bind_export(fn.netctl_init, ps4_netctl_init, "sceNetCtlInit", by_name);
	detail::bind_export(fn.netctl_get_state, ps4_netctl_get_state, "sceNetCtlGetState", by_name);
	detail::bind_export(fn.netctl_get_info, ps4_netctl_get_info, "sceNetCtlGetInfo", by_name);
	if (fn.netctl_init && r.netctl_init < 0)
		r.netctl_init = fn.netctl_init();

	r.bound_by_name = by_name;
	r.bound = static_cast<unsigned>(!!fn.net_init + !!fn.pool_create + !!fn.resolver_create + !!fn.resolver_start_ntoa +
		!!fn.resolver_destroy + !!fn.get_dns_info + !!fn.netctl_init + !!fn.netctl_get_state + !!fn.netctl_get_info);

	// Retries are logged while they are among the first four attempts, and once more when one brings the pool up,
	// so a console without network does not add a line every 5 s.
	if (attempts <= 4 || r.pool >= 0)
	{
		char retry[32] = "";
		if (attempts > 1)
			std::snprintf(retry, sizeof(retry), "; retry %u", attempts - 1);

		char line[320];
		std::snprintf(line, sizeof(line), "net: libSceNet module 0x%x, sceNetInit 0x%x, resolver pool %d (0x%x); libSceNetCtl module 0x%x, sceNetCtlInit 0x%x; netctl state %d; exports bound %u/%u (%u by dlsym)%s",
			static_cast<std::uint32_t>(r.net_module), static_cast<std::uint32_t>(r.net_init), r.pool, static_cast<std::uint32_t>(r.pool),
			static_cast<std::uint32_t>(r.netctl_module), static_cast<std::uint32_t>(r.netctl_init), detail::link_state_now(fn),
			r.bound, entry_point_count, r.bound_by_name, retry);
		ps4_startup_checkpoint(line);

		if (attempts == 1 || r.pool >= 0)
			ps4_startup_checkpoint(("net: dns: " + detail::dns_report_now(fn)).c_str());
		if (attempts == 1)
			ps4_startup_checkpoint(detail::probe_numbering_now().c_str());
	}

	return r;
}

// sceNetCtlGetState after prepare(); -1 when unavailable. 3 = IP address obtained.
inline int link_state()
{
	return detail::link_state_now(prepare().fn);
}

// DNS settings as the system reports them (sceNetGetDnsInfo and NetCtl strings). Also written to startup.log.
inline std::string dns_report()
{
	const status st = prepare();
	std::string line = "net: dns: " + detail::dns_report_now(st.fn);
	ps4_startup_checkpoint(line.c_str());
	return line;
}

// IPPROTO_IP option numbering of the kernel. Also written to startup.log.
inline std::string probe_numbering()
{
	std::string line = detail::probe_numbering_now();
	ps4_startup_checkpoint(line.c_str());
	return line;
}

// 0 when a UDP socket can be bound to INADDR_ANY:port (and is closed again), otherwise the errno of the failure.
inline int udp_bind_probe(u16 port)
{
	const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0)
	{
		return errno ? errno : -1;
	}

	sockaddr_in addr{}; // INADDR_ANY
	addr.sin_len = sizeof(addr);
	addr.sin_family = AF_INET;
	addr.sin_port = static_cast<u16>(((port & 0xff) << 8) | (port >> 8)); // network byte order
	const int result = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0 ? 0 : (errno ? errno : -1);
	::close(fd);
	return result;
}

// IPv4 address of 'host' in network byte order, 0 when every layer failed. Order: IP literal, the system resolver
// (sceNetResolver, which uses the console's own DNS settings), then musl getaddrinfo. 'how' receives the layer that
// answered, or the reason each layer failed, for the log. Blocks for at most the resolver's default timeout.
inline u32 resolve_ipv4(const std::string& host, std::string& how)
{
	how.clear();

	in_addr literal{};

	if (::inet_pton(AF_INET, host.c_str(), &literal) == 1)
	{
		how = "literal";
		return literal.s_addr;
	}

	char text[96];
	const status st = prepare();

	if (st.pool >= 0 && st.fn.resolver_create && st.fn.resolver_start_ntoa && st.fn.resolver_destroy)
	{
		const std::int32_t resolver = st.fn.resolver_create("rpcs3_resolver", st.pool, 0);

		if (resolver >= 0)
		{
			std::uint32_t addr = 0;
			const std::int32_t result = st.fn.resolver_start_ntoa(resolver, host.c_str(), &addr, 0, 0, 0);
			st.fn.resolver_destroy(resolver);

			if (result >= 0 && addr)
			{
				how = "sceNetResolver";
				return addr;
			}

			std::snprintf(text, sizeof(text), "sceNetResolver failed (0x%x); ", static_cast<std::uint32_t>(result));
		}
		else
		{
			std::snprintf(text, sizeof(text), "sceNetResolverCreate failed (0x%x); ", static_cast<std::uint32_t>(resolver));
		}
	}
	else
	{
		std::snprintf(text, sizeof(text), "sceNetResolver unavailable (pool %d); ", st.pool);
	}

	how = text;

	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* found = nullptr;
	const int error = ::getaddrinfo(host.c_str(), nullptr, &hints, &found);
	u32 result = 0;

	for (const addrinfo* entry = found; error == 0 && entry && !result; entry = entry->ai_next)
	{
		if (entry->ai_family == AF_INET && entry->ai_addr)
		{
			result = reinterpret_cast<const sockaddr_in*>(entry->ai_addr)->sin_addr.s_addr;
		}
	}

	if (found)
	{
		::freeaddrinfo(found);
	}

	if (result)
	{
		how += "getaddrinfo";
		return result;
	}

	std::snprintf(text, sizeof(text), "getaddrinfo failed (%d)", error);
	how += text;
	return 0;
}
} // namespace utils::ps4::net

#endif // RPCS3_PS4
