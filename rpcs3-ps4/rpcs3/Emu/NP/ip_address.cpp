#include "stdafx.h"
#include "Emu/system_config.h"
#include "ip_address.h"
#include "Utilities/StrFmt.h"
#include "Emu/IdManager.h"
#include "util/types.hpp"
#include "Emu/NP/rpcn_config.h"
#include "Emu/Cell/lv2/sys_net/sys_net_helpers.h"
#include <algorithm>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif

#ifdef __ORBIS__
#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <thread>
#include <sys/ioctl.h>
#include <sys/time.h>
#include "util/ps4_net.hpp"

// Frontend object (ps4_launcher.cpp), always linked on PS4
void ps4_startup_checkpoint(const char* stage);
#endif

LOG_CHANNEL(IPv6_log, "IPv6_layer");

namespace np
{
	bool ip_address_translator::is_ipv6(u32 addr_be)
	{
		const u32 addr = std::bit_cast<u32, be_t<u32>>(addr_be);
		return (addr >= translation_range_begin && addr <= translation_range_end);
	}

	u32 ip_address_translator::register_ipv6(const std::array<u8, 16>& ipv6_addr)
	{
		if (ipv6_addr[10] == 0xFF && ipv6_addr[11] == 0xFF &&
			std::all_of(ipv6_addr.begin(), ipv6_addr.begin() + 10, [](u8 val)
				{
					return val == 0;
				}))
		{
			// IPv4 over IPv6
			u32 ip_addr{};
			std::memcpy(&ip_addr, &ipv6_addr[12], sizeof(u32));
			return ip_addr;
		}

		std::lock_guard lock(mutex);

		const auto it = ipv6_to_ipv4.find(ipv6_addr);

		if (it == ipv6_to_ipv4.end())
		{
			ensure(cur_addr != translation_range_end);
			const u32 translation_ip = cur_addr++;
			const u32 translation_ip_be = std::bit_cast<u32, be_t<u32>>(translation_ip);
			ipv6_to_ipv4.emplace(ipv6_addr, translation_ip_be);
			ipv4_to_ipv6.push_back(ipv6_addr);
			return translation_ip_be;
		}

		return it->second;
	}

	sockaddr_in6 ip_address_translator::get_ipv6_sockaddr(u32 addr_be, u16 port_be)
	{
		const u32 addr_native = std::bit_cast<u32, be_t<u32>>(addr_be);
		const auto ipv6_addr = ::at32(ipv4_to_ipv6, addr_native - translation_range_begin);

		sockaddr_in6 sockaddr_ipv6{.sin6_family = AF_INET6, .sin6_port = port_be};
		std::memcpy(&sockaddr_ipv6.sin6_addr, ipv6_addr.data(), ipv6_addr.size());

		return sockaddr_ipv6;
	}

	u32 register_ip(std::string_view ip_bytes)
	{
		if (ip_bytes.size() == 4)
		{
			const u32 ip = static_cast<u32>(static_cast<u8>(ip_bytes[0])) << 24 |
			               static_cast<u32>(static_cast<u8>(ip_bytes[1])) << 16 |
			               static_cast<u32>(static_cast<u8>(ip_bytes[2])) << 8 |
			               static_cast<u32>(static_cast<u8>(ip_bytes[3]));

			u32 result_ip = std::bit_cast<u32, be_t<u32>>(ip);

			return result_ip;
		}
		else if (ip_bytes.size() == 16)
		{
			std::array<u8, 16> ipv6_addr{};
			std::memcpy(ipv6_addr.data(), ip_bytes.data(), 16);

			auto& translator = g_fxo->get<np::ip_address_translator>();
			return translator.register_ipv6(ipv6_addr);
		}
		else
		{
			fmt::throw_exception("Received ip address with size = %d", ip_bytes.size());
		}
	}

	struct ipv6_support_state
	{
		ipv6_support_state() = default;
		ipv6_support_state(const ipv6_support_state&) = delete;
		ipv6_support_state& operator=(const ipv6_support_state&) = delete;

		atomic_t<IPV6_SUPPORT> status = IPV6_SUPPORT::IPV6_UNKNOWN;
	};

	// The conditions for IPv6 to be enabled are, in order:
	// -IPv6 is not disabled in config
	// -Internet config is Connected
	// -PSN config is RPCN
	// -Bind IP is not set
	// -RPCN host has an IPv6
	// -Can connect to ipv6.google.com:413
	bool is_ipv6_supported(std::optional<IPV6_SUPPORT> force_state)
	{
#ifdef RPCS3_PS4
		// 0.31: IPv6 is off on PS4, even when a PC rpcn.yml enables it. The SDK headers give IPV6_V6ONLY the Linux
		// number (26 is IPV6_CHECKSUM on the FreeBSD-derived kernel) and the IPv6 P2P path is untested, while any
		// error there is fatal (nt_p2p_port throws).
		static atomic_t<bool> s_logged = false;

		if (!s_logged.exchange(true))
		{
			IPv6_log.notice("is_ipv6_supported(): disabled cause: PS4: IPv6 is not validated on this console");
		}

		static_cast<void>(force_state);
		return false;
#else
		auto& ipv6_support = g_fxo->get<ipv6_support_state>();

		if (force_state)
			ipv6_support.status = *force_state;

		if (ipv6_support.status != IPV6_SUPPORT::IPV6_UNKNOWN)
			return ipv6_support.status == IPV6_SUPPORT::IPV6_SUPPORTED;

		static shared_mutex mtx;
		std::lock_guard lock(mtx);

		if (ipv6_support.status != IPV6_SUPPORT::IPV6_UNKNOWN)
			return ipv6_support.status == IPV6_SUPPORT::IPV6_SUPPORTED;

		auto notice_and_disable = [&](std::string_view reason) -> bool
		{
			IPv6_log.notice("is_ipv6_supported(): disabled cause: %s", reason);
			ipv6_support.status = IPV6_SUPPORT::IPV6_UNSUPPORTED;
			return false;
		};

		// IPv6 feature is only used by RPCN
		if (!g_cfg_rpcn.get_ipv6_support())
			return notice_and_disable("force-disabled through config");

		if (g_cfg.net.net_active != np_internet_status::enabled || g_cfg.net.psn_status != np_psn_status::psn_rpcn)
			return notice_and_disable("RPCN is disabled");

		if (resolve_binding_ip())
			return notice_and_disable("Bind IP is used");

		addrinfo* addr_info{};
		socket_type socket_ipv6{};
		const addrinfo hints{.ai_family = AF_INET6};

		auto cleanup = [&]()
		{
			if (socket_ipv6)
				close_socket(socket_ipv6);

			if (addr_info)
				freeaddrinfo(addr_info);
		};

		auto error_and_disable = [&](std::string_view message) -> bool
		{
			IPv6_log.error("is_ipv6_supported(): disabled cause: %s", message);
			ipv6_support.status = IPV6_SUPPORT::IPV6_UNSUPPORTED;
			cleanup();
			return false;
		};

		auto get_ipv6 = [](const addrinfo* addr_info) -> const addrinfo*
		{
			const addrinfo* found = addr_info;

			while (found != nullptr)
			{
				if (found->ai_family == AF_INET6)
					break;

				found = found->ai_next;
			}

			return found;
		};

		// Check if RPCN has an IPv6 address
		const auto parsed_host = parse_rpcn_host(g_cfg_rpcn.get_host());

		if (!parsed_host)
			return error_and_disable("failed to parse RPCN host");

		const auto [hostname, _] = *parsed_host;

		if (getaddrinfo(hostname.c_str(), nullptr, &hints, &addr_info))
			return error_and_disable("failed to resolve RPCN host");

		if (!get_ipv6(addr_info))
			return error_and_disable("RPCN host doesn't support IPv6");

		freeaddrinfo(addr_info);
		addr_info = {};

		// We try to connect to ipv6.google.com:8080
		if (getaddrinfo("ipv6.google.com", nullptr, &hints, &addr_info))
			return error_and_disable("failed to resolve ipv6.google.com");

		const auto* google_ipv6_addr = get_ipv6(addr_info);

		if (!google_ipv6_addr)
			return error_and_disable("failed to find IPv6 for ipv6.google.com");

		socket_type socket_or_err = ::socket(AF_INET6, SOCK_STREAM, 0);

#ifdef _WIN32
		if (socket_or_err == INVALID_SOCKET)
#else
		if (socket_or_err == -1)
#endif
			return error_and_disable("Failed to create IPv6 socket!");

		socket_ipv6 = socket_or_err;
		sockaddr_in6 ipv6_addr = *reinterpret_cast<const sockaddr_in6*>(google_ipv6_addr->ai_addr);
		ipv6_addr.sin6_port = std::bit_cast<u16, be_t<u16>>(443);

		if (::connect(socket_ipv6, reinterpret_cast<const sockaddr*>(&ipv6_addr), sizeof(ipv6_addr)) != 0)
			return error_and_disable("Failed to connect to ipv6.google.com");

		cleanup();

		IPv6_log.success("Successfully tested IPv6 support!");
		ipv6_support.status = IPV6_SUPPORT::IPV6_SUPPORTED;
		return true;
#endif
	}

	s32 sendto_possibly_ipv6(socket_type native_socket, const char* data, u32 size, const sockaddr_in* addr, int native_flags)
	{
		if (is_ipv6_supported())
		{
			sockaddr_in6 addr_ipv6{};

			if (np::ip_address_translator::is_ipv6(addr->sin_addr.s_addr))
			{
				auto& translator = g_fxo->get<np::ip_address_translator>();
				addr_ipv6 = translator.get_ipv6_sockaddr(addr->sin_addr.s_addr, addr->sin_port);
			}
			else
			{
				addr_ipv6 = sockaddr_to_sockaddr6(*addr);
			}

			return ::sendto(native_socket, data, size, native_flags, reinterpret_cast<const sockaddr*>(&addr_ipv6), sizeof(sockaddr_in6));
		}

		return ::sendto(native_socket, data, size, native_flags, reinterpret_cast<const sockaddr*>(addr), sizeof(sockaddr_in));
	}

	sockaddr_in6 sockaddr_to_sockaddr6(const sockaddr_in& addr)
	{
		sockaddr_in6 addr_ipv6{.sin6_family = AF_INET6, .sin6_port = addr.sin_port};
		std::array<u8, 16> ipv6{};

		// IPv4 over IPv6
		ipv6[10] = 0xFF;
		ipv6[11] = 0xFF;
		std::memcpy(ipv6.data() + 12, &addr.sin_addr.s_addr, 4);
		std::memcpy(&addr_ipv6.sin6_addr, ipv6.data(), ipv6.size());
		return addr_ipv6;
	}

	sockaddr_in sockaddr6_to_sockaddr(const sockaddr_in6& addr)
	{
		sockaddr_in addr_ipv4{.sin_family = AF_INET, .sin_port = addr.sin6_port};

		std::array<u8, 16> ipv6{};
		std::memcpy(ipv6.data(), &addr.sin6_addr, 16);
		auto& translator = g_fxo->get<np::ip_address_translator>();
		addr_ipv4.sin_addr.s_addr = translator.register_ipv6(ipv6);

		return addr_ipv4;
	}

	void close_socket(socket_type socket)
	{
		if (socket)
		{
#ifdef _WIN32
			::closesocket(socket);
#else
			::close(socket);
#endif
		}
	}

#ifdef __ORBIS__
	namespace
	{
		// FreeBSD's FIONBIO. The SDK headers only carry the Linux number (LINUX_FIONBIO 0x5421), which this kernel
		// does not read as FIONBIO.
		constexpr unsigned long ps4_fionbio = _IOW('f', 126, int);
		static_assert(ps4_fionbio == 0x8004667eUL);

		// Sony's SOL_SOCKET option for non-blocking I/O (libSceNet's SO_NBIO). FreeBSD's own SOL_SOCKET options end
		// below 0x1100, so a kernel without it answers ENOPROTOOPT.
		constexpr int ps4_so_nbio = 0x1200;

		// libSceNet's sceNetSetsockopt: 0 or a negative SCE error code
		using ps4_sce_setsockopt_t = s32 (*)(s32 id, s32 level, s32 optname, const void* optval, u32 optlen);

		// How native sockets get SO_NBIO. Set once by the boot probe, and only after a plain recvfrom on a socket with
		// the option returned at once; until then no socket gets it.
		enum ps4_nbio_route : u32
		{
			ps4_nbio_unproven = 0,
			ps4_nbio_setsockopt, // the kernel's setsockopt
			ps4_nbio_sce,        // sceNetSetsockopt (looked up by name)
		};

		std::atomic<u32> s_ps4_nbio_route{ps4_nbio_unproven};
		std::atomic<ps4_sce_setsockopt_t> s_ps4_sce_setsockopt{nullptr};

		// 0 on success. 'error' receives errno (setsockopt) or the SCE result (sceNetSetsockopt).
		int ps4_apply_so_nbio(int fd, u32 route, ps4_sce_setsockopt_t sce_setsockopt, int& error)
		{
			const int one = 1;

			if (route == ps4_nbio_sce)
			{
				const s32 result = sce_setsockopt ? sce_setsockopt(fd, SOL_SOCKET, ps4_so_nbio, &one, sizeof(one)) : -1;
				error = result;
				return result == 0 ? 0 : -1;
			}

			const int result = ::setsockopt(fd, SOL_SOCKET, ps4_so_nbio, &one, sizeof(one));
			error = result == 0 ? 0 : errno;
			return result == 0 ? 0 : -1;
		}

		struct ps4_nbio_steps
		{
			bool so_nbio_tried = false; // step 0, only once the boot probe proved SO_NBIO
			bool so_nbio_sce = false;
			int so_nbio = -1;
			int so_nbio_error = 0;
			int getfl = -1; // F_GETFL before the change
			int getfl_errno = 0;
			int setfl = -1;
			int setfl_errno = 0;
			int fionbio = -1;
			int fionbio_errno = 0;
			int readback = -1; // F_GETFL after F_SETFL and FIONBIO
			int readback_errno = 0;

			bool fcntl_ok() const noexcept
			{
				return getfl >= 0 && setfl == 0 && readback >= 0 && (readback & O_NONBLOCK) != 0;
			}

			bool so_nbio_ok() const noexcept
			{
				return so_nbio_tried && so_nbio == 0;
			}

			// One of the three routes took effect
			bool non_blocking() const noexcept
			{
				return so_nbio_ok() || fionbio == 0 || fcntl_ok();
			}

			std::string so_nbio_text() const
			{
				if (!so_nbio_tried)
					return {};
				if (so_nbio == 0)
					return ", SO_NBIO ok";
				if (so_nbio_sce)
					return fmt::format(", SO_NBIO (sceNetSetsockopt) 0x%08x", static_cast<u32>(so_nbio_error));
				return fmt::format(", SO_NBIO -1/%d", so_nbio_error);
			}

			std::string fcntl_text() const
			{
				if (getfl < 0)
					return fmt::format("fcntl F_GETFL -1/%d", getfl_errno);
				if (setfl != 0)
					return fmt::format("fcntl F_SETFL %d/%d", setfl, setfl_errno);
				if (readback < 0)
					return fmt::format("fcntl readback -1/%d", readback_errno);
				if (!(readback & O_NONBLOCK))
					return fmt::format("fcntl readback 0x%x without O_NONBLOCK", readback);
				return "fcntl ok";
			}

			std::string fionbio_text() const
			{
				return fionbio == 0 ? std::string("FIONBIO ok") : fmt::format("FIONBIO %d/%d", fionbio, fionbio_errno);
			}
		};

		// Native sockets must never block the calling thread: a guest blocking call sleeps in lv2 and is woken by the
		// network thread. The fcntl result was never checked on this kernel, so FIONBIO sets the same socket state a
		// second way and F_GETFL reads it back. The kernel refuses both (EACCES); SO_NBIO comes first once the boot
		// probe proved it. The first failure is written to startup.log.
		ps4_nbio_steps ps4_make_non_blocking(int fd, bool report_failure)
		{
			ps4_nbio_steps r;

			if (const u32 route = s_ps4_nbio_route.load(std::memory_order_acquire); route != ps4_nbio_unproven)
			{
				r.so_nbio_tried = true;
				r.so_nbio_sce = route == ps4_nbio_sce;
				r.so_nbio = ps4_apply_so_nbio(fd, route, s_ps4_sce_setsockopt.load(std::memory_order_acquire), r.so_nbio_error);
			}

			r.getfl = ::fcntl(fd, F_GETFL, 0);
			if (r.getfl < 0)
				r.getfl_errno = errno;

			r.setfl = ::fcntl(fd, F_SETFL, (r.getfl < 0 ? 0 : r.getfl) | O_NONBLOCK);
			if (r.setfl != 0)
				r.setfl_errno = errno;

			int one = 1;
			r.fionbio = ::ioctl(fd, ps4_fionbio, &one);
			if (r.fionbio != 0)
				r.fionbio_errno = errno;

			r.readback = ::fcntl(fd, F_GETFL, 0);
			if (r.readback < 0)
				r.readback_errno = errno;

			if (report_failure && (!r.fcntl_ok() || r.fionbio != 0))
			{
				static atomic_t<bool> s_reported = false;

				if (!s_reported.exchange(true))
				{
					ps4_startup_checkpoint(fmt::format("network ABI: making socket %d non-blocking: %s, %s%s", fd, r.fcntl_text(), r.fionbio_text(), r.so_nbio_text()).c_str());
				}
			}

			if (report_failure && r.so_nbio_tried && r.so_nbio != 0)
			{
				static atomic_t<bool> s_nbio_reported = false;

				if (!s_nbio_reported.exchange(true))
				{
					ps4_startup_checkpoint(fmt::format("network ABI: socket %d: SO_NBIO failed after the boot probe passed%s", fd, r.so_nbio_text()).c_str());
				}
			}

			return r;
		}

		// Boot probe of SO_NBIO. Runs on its own thread: the main thread only waits for it, with a bound, so a
		// call that blocks despite every precaution cannot stop the boot.
		struct ps4_so_nbio_probe
		{
			int timeo = -1; // SO_RCVTIMEO, the bound of the plain receive
			int timeo_errno = 0;
			int set = -1; // setsockopt(SOL_SOCKET, SO_NBIO)
			int set_errno = 0;
			int sce_state = 0; // sceNetSetsockopt: 0 not needed, 1 called, 2 not exported
			s32 sce_result = 0;
			ps4_sce_setsockopt_t sce_setsockopt = nullptr;
			u32 route = ps4_nbio_unproven;
			bool recv_tried = false;
			s64 recv_result = 0;
			int recv_errno = 0;
			s64 recv_us = 0;
			bool connect_tried = false;
			int connect_result = 0;
			int connect_errno = 0;
			s64 connect_us = 0;
			std::string notes;
			std::atomic<int> stage{0}; // 0 setup, 1 recvfrom, 2 connect, 3 finished
			std::atomic<bool> done{false};

			// A receive with nothing queued failed at once with EWOULDBLOCK: the socket no longer blocks. A blocking
			// one fails with the same errno, but only when SO_RCVTIMEO expires 200 ms later.
			bool recv_proved() const noexcept
			{
				return recv_tried && recv_result < 0 && (recv_errno == EAGAIN || recv_errno == EWOULDBLOCK) && recv_us < 50'000;
			}
		};

		s64 ps4_us_since(std::chrono::steady_clock::time_point start)
		{
			return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		}

		void ps4_run_so_nbio_probe(ps4_so_nbio_probe& p)
		{
			const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

			if (fd < 0)
			{
				p.notes += fmt::format(", UDP socket -1/%d", errno);
				return;
			}

			timeval timeout{};
			timeout.tv_usec = 200'000;
			p.timeo = ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
			p.timeo_errno = p.timeo == 0 ? 0 : errno;

			sockaddr_in local{};
			local.sin_len = sizeof(local);
			local.sin_family = AF_INET;
			local.sin_addr.s_addr = std::bit_cast<u32, be_t<u32>>(0x7f000001u); // 127.0.0.1, any port

			if (::bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
			{
				p.notes += fmt::format(", bind 127.0.0.1:0 -1/%d", errno);
			}

			const int one = 1;
			p.set = ::setsockopt(fd, SOL_SOCKET, ps4_so_nbio, &one, sizeof(one));
			p.set_errno = p.set == 0 ? 0 : errno;

			if (p.set == 0)
			{
				p.route = ps4_nbio_setsockopt;
			}
			else if (const auto sce = reinterpret_cast<ps4_sce_setsockopt_t>(utils::ps4::net::detail::find_export("sceNetSetsockopt")))
			{
				p.sce_state = 1;
				p.sce_result = sce(fd, SOL_SOCKET, ps4_so_nbio, &one, sizeof(one));

				if (p.sce_result == 0)
				{
					p.route = ps4_nbio_sce;
					p.sce_setsockopt = sce;
				}
			}
			else
			{
				p.sce_state = 2;
			}

			// Without its timeout the receive below would be unbounded on a socket that still blocks
			if (p.route != ps4_nbio_unproven && p.timeo == 0)
			{
				p.stage = 1;
				p.recv_tried = true;

				u8 buffer[16]{};
				sockaddr_storage from{};
				socklen_t from_len = sizeof(from);

				const auto start = std::chrono::steady_clock::now();
				p.recv_result = ::recvfrom(fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
				p.recv_errno = p.recv_result < 0 ? errno : 0;
				p.recv_us = ps4_us_since(start);
			}

			::close(fd);

			if (!p.recv_proved())
			{
				return;
			}

			// TCP connect on a fresh socket with the option: -1/EINPROGRESS (or an immediate ECONNREFUSED from the
			// loopback) instead of waiting for the handshake
			p.stage = 2;
			const int tcp = ::socket(AF_INET, SOCK_STREAM, 0);

			if (tcp < 0)
			{
				p.notes += fmt::format(", TCP socket -1/%d", errno);
				return;
			}

			int error = 0;

			if (ps4_apply_so_nbio(tcp, p.route, p.sce_setsockopt, error) == 0)
			{
				sockaddr_in target{};
				target.sin_len = sizeof(target);
				target.sin_family = AF_INET;
				target.sin_port = std::bit_cast<u16, be_t<u16>>(1);
				target.sin_addr.s_addr = std::bit_cast<u32, be_t<u32>>(0x7f000001u);

				p.connect_tried = true;
				const auto start = std::chrono::steady_clock::now();
				p.connect_result = ::connect(tcp, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
				p.connect_errno = p.connect_result == 0 ? 0 : errno;
				p.connect_us = ps4_us_since(start);
			}
			else
			{
				p.notes += fmt::format(", SO_NBIO on the TCP socket failed (%d)", error);
			}

			::close(tcp);
		}

		// Decides the SO_NBIO route for every later native socket and returns the startup.log line.
		std::string ps4_probe_so_nbio()
		{
			const auto probe = std::make_shared<ps4_so_nbio_probe>();
			std::string start_error;

			try
			{
				std::thread([probe]()
				{
					ps4_run_so_nbio_probe(*probe);
					probe->stage = 3;
					probe->done.store(true, std::memory_order_release);
				}).detach();
			}
			catch (const std::exception& e)
			{
				start_error = e.what();
			}

			if (!start_error.empty())
			{
				return fmt::format("network ABI: SO_NBIO 0x1200 probe thread not started (%s) -> SO_NBIO not used", start_error);
			}

			// The receive is bounded by its 200 ms timeout and the connect is to the loopback
			constexpr s64 wait_limit_ms = 1000;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_limit_ms);

			while (!probe->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			if (!probe->done.load(std::memory_order_acquire))
			{
				// The thread keeps its own reference to the probe state and closes its sockets if it ever returns
				const int stage = probe->stage.load();
				return fmt::format("network ABI: SO_NBIO 0x1200 probe still blocked after %d ms (in %s) -> SO_NBIO not used", wait_limit_ms,
					stage == 1 ? "the plain recvfrom" : stage == 2 ? "the TCP connect" : "socket setup");
			}

			const ps4_so_nbio_probe& p = *probe;
			std::string line = fmt::format("network ABI: SO_NBIO 0x1200 rc %d/%d", p.set, p.set_errno);

			if (p.sce_state == 1)
				line += fmt::format(", sceNetSetsockopt 0x%08x", static_cast<u32>(p.sce_result));
			else if (p.sce_state == 2)
				line += ", sceNetSetsockopt not exported";

			line += p.notes;

			if (p.route != ps4_nbio_unproven && p.timeo != 0)
				line += fmt::format("; SO_RCVTIMEO -1/%d, so the plain recvfrom was not tried", p.timeo_errno);

			if (p.recv_tried)
			{
				line += fmt::format(" -> plain recvfrom %d/%d in %d us", p.recv_result, p.recv_errno, p.recv_us);

				if (p.recv_us >= 50'000)
					line += ": BLOCKED 200 ms";
			}

			if (p.connect_tried)
				line += fmt::format("; connect %d/%d in %d us", p.connect_result, p.connect_errno, p.connect_us);

			if (p.recv_proved())
			{
				s_ps4_sce_setsockopt.store(p.sce_setsockopt, std::memory_order_release);
				s_ps4_nbio_route.store(p.route, std::memory_order_release);
				line += p.route == ps4_nbio_sce ? " -> SO_NBIO on for native sockets (through sceNetSetsockopt)" : " -> SO_NBIO on for native sockets";
			}
			else
			{
				line += " -> SO_NBIO not used";
			}

			return line;
		}
	}

	std::string ps4_probe_nonblocking_recvfrom()
	{
		// The value the kernel reads as MSG_DONTWAIT (ps4-port/compat/sys/socket.h replaces the SDK's Linux numbers)
		static_assert(MSG_DONTWAIT == 0x80);

		const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

		if (fd < 0)
		{
			return fmt::format("network ABI: non-blocking recvfrom probe: socket() failed, errno=%d", errno);
		}

		std::string notes;

		// Upper bound for the probe, should the kernel ignore every non-blocking request below
		timeval timeout{};
		timeout.tv_usec = 200'000;

		if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
		{
			notes += fmt::format(", SO_RCVTIMEO -1/%d", errno);
		}

		sockaddr_in local{};
		local.sin_len = sizeof(local);
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = std::bit_cast<u32, be_t<u32>>(0x7f000001u); // 127.0.0.1, any port

		if (::bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
		{
			notes += fmt::format(", bind 127.0.0.1:0 -1/%d", errno);
		}

		// Same steps as set_socket_non_blocking; this line reports them, so no separate failure line
		const ps4_nbio_steps steps = ps4_make_non_blocking(fd, false);

		u8 buffer[16]{};
		sockaddr_storage from{};
		socklen_t from_len = sizeof(from);

		const auto start = std::chrono::steady_clock::now();
		const s64 received = ::recvfrom(fd, buffer, sizeof(buffer), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &from_len);
		const int error = received < 0 ? errno : 0;
		const s64 elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();

		::close(fd);

		std::string line = fmt::format("network ABI: non-blocking recvfrom -> %d/%d in %d us (%s, %s%s)", received, error, elapsed_us, steps.fcntl_text(), steps.fionbio_text(), notes);

		if (elapsed_us >= 100'000)
		{
			line += ": BLOCKED (bounded by the 200 ms receive timeout)";
		}

		ps4_startup_checkpoint(line.c_str());

		// Then SO_NBIO, the route left now that fcntl and FIONBIO are refused; its line goes to the caller
		return ps4_probe_so_nbio();
	}

	bool ps4_set_socket_non_blocking(socket_type socket)
	{
		return socket && ps4_make_non_blocking(socket, true).non_blocking();
	}
#endif

	void set_socket_non_blocking(socket_type socket)
	{
		if (socket)
		{
#ifdef _WIN32
			u_long _true = 1;
			::ioctlsocket(socket, FIONBIO, &_true);
#elif defined(__ORBIS__)
			ps4_set_socket_non_blocking(socket);
#else
			::fcntl(socket, F_SETFL, ::fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
#endif
		}
	}
} // namespace np
