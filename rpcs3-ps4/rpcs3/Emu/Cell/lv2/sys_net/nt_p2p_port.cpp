#include "stdafx.h"
#include "Emu/NP/ip_address.h"
#include "nt_p2p_port.h"
#include "lv2_socket_p2ps.h"
#include "sys_net_helpers.h"
#include "Emu/NP/signaling_handler.h"
#include "sys_net_helpers.h"
#include "Emu/NP/vport0.h"
#include "Emu/NP/np_handler.h"
#include "Emu/NP/np_helpers.h"

#ifdef RPCS3_PS4
#include <cerrno>

// Frontend object (ps4_launcher.cpp), always linked.
void ps4_startup_checkpoint(const char* stage);
#endif

LOG_CHANNEL(sys_net);

namespace sys_net_helpers
{
	bool all_reusable(const std::set<s32>& sock_ids)
	{
		for (const s32 sock_id : sock_ids)
		{
			const auto [_, reusable] = idm::check<lv2_socket>(sock_id, [&](lv2_socket& sock) -> bool
				{
					auto [res_reuseaddr, optval_reuseaddr, optlen_reuseaddr] = sock.getsockopt(SYS_NET_SOL_SOCKET, SYS_NET_SO_REUSEADDR, sizeof(s32));
					auto [res_reuseport, optval_reuseport, optlen_reuseport] = sock.getsockopt(SYS_NET_SOL_SOCKET, SYS_NET_SO_REUSEPORT, sizeof(s32));

					const bool reuse_addr = optlen_reuseaddr == 4 && !!optval_reuseaddr._int;
					const bool reuse_port = optlen_reuseport == 4 && !!optval_reuseport._int;

					return (reuse_addr || reuse_port);
				});

			if (!reusable)
			{
				return false;
			}
		}

		return true;
	}
} // namespace sys_net_helpers

nt_p2p_port::nt_p2p_port(u16 port)
	: port(port)
{
	const bool is_ipv6 = np::is_ipv6_supported();

#ifdef RPCS3_PS4
	// None of the failures below may throw. This constructor runs under list_p2p_ports_mutex (a guest P2P bind, or the
	// NP handler binding SCE_NP_PORT), and a thread ended by emergency_exit is not unwound on the PS4: the lock stayed
	// held, and the P2P thread, every later bind and the emulation stop blocked on it. The port is kept without a
	// socket instead: the P2P thread's poll skips fd -1 and the P2P binds/connects report EADDRNOTAVAIL.
	const auto ps4_unusable = [&](const char* what)
	{
		const std::string line = fmt::format("P2P: UDP port %d unusable, %s failed (errno %d)", port, what, get_native_error());
		sys_net.error("%s", line);
		ps4_startup_checkpoint(line.c_str());

		if (p2p_socket != -1)
		{
			np::close_socket(p2p_socket);
			p2p_socket = -1;
		}
	};
#endif

	// Creates and bind P2P Socket
	p2p_socket = ::socket(is_ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
	if (p2p_socket == INVALID_SOCKET)
#else
	if (p2p_socket == -1)
#endif
#ifdef RPCS3_PS4
	{
		ps4_unusable("socket()");
		return;
	}
#else
		fmt::throw_exception("Failed to create DGRAM socket for P2P socket: %s!", get_last_error(true));
#endif

	np::set_socket_non_blocking(p2p_socket);

	u32 optval = 131072; // value obtained from DECR for a SOCK_DGRAM_P2P socket(should maybe be bigger for actual socket?)
	if (setsockopt(p2p_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&optval), sizeof(optval)) != 0)
#ifdef RPCS3_PS4
		sys_net.warning("P2P port %d: setsockopt SO_RCVBUF failed (errno %d), keeping the default receive buffer", port, get_native_error());
#else
		fmt::throw_exception("Error setsockopt SO_RCVBUF on P2P socket: %s", get_last_error(true));
#endif

	optval = 1;
	if (setsockopt(p2p_socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&optval), sizeof(optval)) != 0)
#ifdef RPCS3_PS4
		sys_net.warning("P2P port %d: setsockopt SO_BROADCAST failed (errno %d), broadcasts from this port will fail", port, get_native_error());
#else
		fmt::throw_exception("Error setsockopt SO_BROADCAST on P2P socket: %s", get_last_error(true));
#endif

	int ret_bind = 0;
	const u16 be_port = std::bit_cast<u16, be_t<u16>>(port);
	auto& nph = g_fxo->get<named_thread<np::np_handler>>();

#ifdef RPCS3_PS4
	bound_port = port;
#endif

	if (is_ipv6)
	{
		// Some OS(Windows, maybe more) will only support IPv6 adressing by default and we need IPv4 over IPv6
		optval = 0;
		if (setsockopt(p2p_socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&optval), sizeof(optval)) != 0)
#ifdef RPCS3_PS4
		{
			ps4_unusable("setsockopt(IPV6_V6ONLY)");
			return;
		}
#else
			fmt::throw_exception("Error setsockopt IPV6_V6ONLY on P2P socket: %s", get_last_error(true));
#endif

		::sockaddr_in6 p2p_ipv6_addr{.sin6_family = AF_INET6, .sin6_port = be_port};
		ret_bind = ::bind(p2p_socket, reinterpret_cast<sockaddr*>(&p2p_ipv6_addr), sizeof(p2p_ipv6_addr));
	}
	else
	{
		::sockaddr_in p2p_ipv4_addr{.sin_family = AF_INET, .sin_port = be_port};
		const u32 bind_ip = nph.get_bind_ip();
		p2p_ipv4_addr.sin_addr.s_addr = bind_ip;

		if (ret_bind = ::bind(p2p_socket, reinterpret_cast<const sockaddr*>(&p2p_ipv4_addr), sizeof(p2p_ipv4_addr)); ret_bind == -1 && bind_ip)
		{
			sys_net.error("Failed to bind to %s:%d, falling back to 0.0.0.0:%d", np::ip_to_string(bind_ip), port, port);
			p2p_ipv4_addr.sin_addr.s_addr = 0;
			ret_bind = ::bind(p2p_socket, reinterpret_cast<const sockaddr*>(&p2p_ipv4_addr), sizeof(p2p_ipv4_addr));
		}

#ifdef RPCS3_PS4
		// 0.31: when the console (or another app) already holds the port, use one of port+10000..port+10007
		// (3658 -> 13658..13665) instead of throwing, which is fatal here. The guest keeps seeing 'port' and
		// list_p2p_ports keeps it as the key; peers reach this socket through the address and port RPCN's UDP
		// signaling observes from it, so a different local port still works behind NAT. Never 3659: a guest UDP
		// bind on 3658 is remapped there (lv2_socket_native.cpp).
		if (ret_bind == -1 && errno == EADDRINUSE && port <= 65535 - 10007)
		{
			for (u32 candidate = port + 10000u; candidate <= port + 10007u; candidate++)
			{
				p2p_ipv4_addr.sin_port = std::bit_cast<u16, be_t<u16>>(static_cast<u16>(candidate));

				if (ret_bind = ::bind(p2p_socket, reinterpret_cast<const sockaddr*>(&p2p_ipv4_addr), sizeof(p2p_ipv4_addr)); ret_bind == 0)
				{
					bound_port = static_cast<u16>(candidate);
					const std::string line = fmt::format("P2P: UDP %d busy, bound %d", port, bound_port);
					sys_net.warning("%s", line);
					ps4_startup_checkpoint(line.c_str());
					break;
				}

				if (errno != EADDRINUSE)
				{
					break;
				}
			}
		}
#endif
	}

	if (ret_bind == -1)
#ifdef RPCS3_PS4
	{
		ps4_unusable("bind()");
		return;
	}
#else
		fmt::throw_exception("Failed to bind DGRAM socket to %d for P2P: %s!", port, get_last_error(true));
#endif

#ifdef RPCS3_PS4
	nph.upnp_add_port_mapping(bound_port, "UDP");

	if (bound_port != port)
		sys_net.notice("P2P port %d was bound! (local UDP port %d)", port, bound_port);
	else
		sys_net.notice("P2P port %d was bound!", port);
#else
	nph.upnp_add_port_mapping(port, "UDP");
	sys_net.notice("P2P port %d was bound!", port);
#endif
}

nt_p2p_port::~nt_p2p_port()
{
	np::close_socket(p2p_socket);
}

void nt_p2p_port::dump_packet(p2ps_encapsulated_tcp* tcph)
{
	sys_net.trace("PACKET DUMP:\nsrc_port: %d\ndst_port: %d\nflags: %d\nseq: %d\nack: %d\nlen: %d", tcph->src_port, tcph->dst_port, tcph->flags, tcph->seq, tcph->ack, tcph->length);
}

// Must be used under bound_p2p_vports_mutex lock
u16 nt_p2p_port::get_port()
{
	if (binding_port == 0)
	{
		binding_port = 30000;
	}

	return binding_port++;
}

bool nt_p2p_port::handle_connected(s32 sock_id, p2ps_encapsulated_tcp* tcp_header, u8* data, ::sockaddr_storage* op_addr)
{
	const auto sock = idm::check<lv2_socket>(sock_id, [&](lv2_socket& sock) -> bool
		{
			ensure(sock.get_type() == SYS_NET_SOCK_STREAM_P2P);
			auto& sock_p2ps = reinterpret_cast<lv2_socket_p2ps&>(sock);

			return sock_p2ps.handle_connected(tcp_header, data, op_addr, this);
		});

	if (!sock)
	{
		sys_net.error("[P2PS] Couldn't find the socket!");
		return false;
	}

	if (!sock.ret)
	{
		sys_net.error("[P2PS] handle_connected() failed!");
		return false;
	}

	return true;
}

bool nt_p2p_port::handle_listening(s32 sock_id, p2ps_encapsulated_tcp* tcp_header, u8* data, ::sockaddr_storage* op_addr)
{
	auto sock = idm::get_unlocked<lv2_socket>(sock_id);
	if (!sock)
		return false;

	auto& sock_p2ps = reinterpret_cast<lv2_socket_p2ps&>(*sock);
	return sock_p2ps.handle_listening(tcp_header, data, op_addr);
}

bool nt_p2p_port::recv_data()
{
	::sockaddr_storage native_addr{};
	::socklen_t native_addrlen = sizeof(native_addr);
#ifdef RPCS3_PS4
	// The host socket may still be blocking (the kernel refuses fcntl/FIONBIO): without MSG_DONTWAIT the drain loop
	// blocks on the call after the last queued datagram, holding mutex_thread_loop and list_p2p_ports_mutex.
	constexpr int recv_flags = MSG_DONTWAIT;
#else
	constexpr int recv_flags = 0;
#endif
	const auto recv_res = SYS_NET_EINTR_RETRY(::recvfrom(p2p_socket, reinterpret_cast<char*>(p2p_recv_data.data()), ::size32(p2p_recv_data), recv_flags, reinterpret_cast<struct sockaddr*>(&native_addr), &native_addrlen));

	if (recv_res == -1)
	{
		auto lerr = get_last_error(false);
		if (lerr != SYS_NET_EINPROGRESS && lerr != SYS_NET_EWOULDBLOCK)
			sys_net.error("Error recvfrom on %s P2P socket: %d", np::is_ipv6_supported() ? "IPv6" : "IPv4", lerr);

		return false;
	}

	if (recv_res < static_cast<s32>(sizeof(u16)))
	{
		sys_net.error("Received badly formed packet on P2P port(no vport)!");
		return true;
	}

	u16 dst_vport = reinterpret_cast<le_t<u16>&>(p2p_recv_data[0]);

	if (np::is_ipv6_supported())
	{
		const auto* addr_ipv6 = reinterpret_cast<sockaddr_in6*>(&native_addr);
		const auto addr_ipv4 = np::sockaddr6_to_sockaddr(*addr_ipv6);
		native_addr = {};
		std::memcpy(&native_addr, &addr_ipv4, sizeof(addr_ipv4));
	}

	if (dst_vport == 0)
	{
		if (recv_res < VPORT_0_HEADER_SIZE)
		{
			sys_net.error("Bad vport 0 packet(no subset)!");
			return true;
		}

		const u8 subset      = p2p_recv_data[2];
		const auto data_size = recv_res - VPORT_0_HEADER_SIZE;
		std::vector<u8> vport_0_data(p2p_recv_data.data() + VPORT_0_HEADER_SIZE, p2p_recv_data.data() + VPORT_0_HEADER_SIZE + data_size);

		switch (subset)
		{
		case SUBSET_RPCN:
		{
			std::lock_guard lock(s_rpcn_mutex);
			rpcn_msgs.push_back(std::move(vport_0_data));
			return true;
		}
		case SUBSET_SIGNALING:
		{
			signaling_message msg;
			msg.src_addr = reinterpret_cast<struct sockaddr_in*>(&native_addr)->sin_addr.s_addr;
			msg.src_port = std::bit_cast<u16, be_t<u16>>(reinterpret_cast<struct sockaddr_in*>(&native_addr)->sin_port);
			msg.data     = std::move(vport_0_data);

			{
				std::lock_guard lock(s_sign_mutex);
				sign_msgs.push_back(std::move(msg));
			}

			auto& sigh = g_fxo->get<named_thread<signaling_handler>>();
			sigh.wake_up();
			return true;
		}
		default:
		{
			sys_net.error("Invalid vport 0 subset!");
			return true;
		}
		}
	}

	if (recv_res < VPORT_P2P_HEADER_SIZE)
	{
		return true;
	}

	const u16 src_vport = *reinterpret_cast<le_t<u16>*>(p2p_recv_data.data() + sizeof(u16));
	const u16 vport_flags = *reinterpret_cast<le_t<u16>*>(p2p_recv_data.data() + sizeof(u16) + sizeof(u16));
	std::vector<u8> p2p_data(recv_res - VPORT_P2P_HEADER_SIZE);
	memcpy(p2p_data.data(), p2p_recv_data.data() + VPORT_P2P_HEADER_SIZE, p2p_data.size());

	if (vport_flags & P2P_FLAG_P2P)
	{
		std::lock_guard lock(bound_p2p_vports_mutex);
		if (bound_p2p_vports.contains(dst_vport))
		{
			sys_net_sockaddr_in_p2p p2p_addr{};

			p2p_addr.sin_len    = sizeof(sys_net_sockaddr_in);
			p2p_addr.sin_family = SYS_NET_AF_INET;
			p2p_addr.sin_addr   = std::bit_cast<be_t<u32>, u32>(reinterpret_cast<struct sockaddr_in*>(&native_addr)->sin_addr.s_addr);
			p2p_addr.sin_vport  = src_vport;
			p2p_addr.sin_port   = std::bit_cast<be_t<u16>, u16>(reinterpret_cast<struct sockaddr_in*>(&native_addr)->sin_port);

			auto& bound_sockets = ::at32(bound_p2p_vports, dst_vport);

			for (auto it = bound_sockets.begin(); it != bound_sockets.end();)
			{
				s32 sock_id = *it;
				const auto sock = idm::check<lv2_socket>(sock_id, [&](lv2_socket& sock)
					{
						ensure(sock.get_type() == SYS_NET_SOCK_DGRAM_P2P);
						auto& sock_p2p = reinterpret_cast<lv2_socket_p2p&>(sock);

						sock_p2p.handle_new_data(p2p_addr, p2p_data);
					});

				if (!sock)
				{
					sys_net.error("Socket %d found in bound_p2p_vports didn't exist!", sock_id);
					it = bound_sockets.erase(it);
					if (bound_sockets.empty())
					{
						bound_p2p_vports.erase(dst_vport);
						break;
					}
				}
				else
				{
					it++;
				}
			}

			return true;
		}
	}
	else if (vport_flags & P2P_FLAG_P2PS)
	{
		if (p2p_data.size() < sizeof(p2ps_encapsulated_tcp))
		{
			sys_net.notice("Received P2P packet targeted at unbound vport(likely) or invalid(vport=%d)", dst_vport);
			return true;
		}

		auto* tcp_header = reinterpret_cast<p2ps_encapsulated_tcp*>(p2p_data.data());

		// Validate signature & length
		if (tcp_header->signature != P2PS_U2S_SIG)
		{
			sys_net.notice("Received P2P packet targeted at unbound vport(vport=%d)", dst_vport);
			return true;
		}

		if (tcp_header->length != (p2p_data.size() - sizeof(p2ps_encapsulated_tcp)))
		{
			sys_net.error("Received STREAM-P2P packet tcp length didn't match packet length");
			return true;
		}

		// Sanity check
		if (tcp_header->dst_port != dst_vport)
		{
			sys_net.error("Received STREAM-P2P packet with dst_port != vport");
			return true;
		}

		// Validate checksum
		u16 given_checksum   = tcp_header->checksum;
		tcp_header->checksum = 0;
		if (given_checksum != u2s_tcp_checksum(reinterpret_cast<const le_t<u16>*>(p2p_data.data()), p2p_data.size()))
		{
			sys_net.error("Checksum is invalid, dropping packet!");
			return true;
		}

		// The packet is valid
		dump_packet(tcp_header);

		// Check if it's bound
		const u64 key_connected = (reinterpret_cast<struct sockaddr_in*>(&native_addr)->sin_addr.s_addr) | (static_cast<u64>(tcp_header->src_port) << 48) | (static_cast<u64>(tcp_header->dst_port) << 32);

		{
			std::lock_guard lock(bound_p2p_vports_mutex);
			if (bound_p2p_streams.contains(key_connected))
			{
				const auto sock_id = ::at32(bound_p2p_streams, key_connected);
				sys_net.trace("Received packet for connected STREAM-P2P socket(s=%d)", sock_id);
				handle_connected(sock_id, tcp_header, p2p_data.data() + sizeof(p2ps_encapsulated_tcp), &native_addr);
				return true;
			}

			if (bound_p2ps_vports.contains(tcp_header->dst_port))
			{
				const auto& bound_sockets = ::at32(bound_p2ps_vports, tcp_header->dst_port);
				bool handled = false;

				for (const auto sock_id : bound_sockets)
				{
					sys_net.trace("Received packet for listening STREAM-P2P socket(s=%d)", sock_id);
					handled |= handle_listening(sock_id, tcp_header, p2p_data.data() + sizeof(p2ps_encapsulated_tcp), &native_addr);
				}

				if (handled)
				{
					return true;
				}

				// The vport is only reserved(e.g. by a connect()ed socket), no socket is listening on it, reply with RST as if it was unbound
			}

			if (tcp_header->flags == p2ps_tcp_flags::RST)
			{
				sys_net.trace("[P2PS] Received RST on unbound P2PS");
				return true;
			}

			// The P2PS packet was sent to an unbound vport, send a RST packet
			p2ps_encapsulated_tcp send_hdr;
			send_hdr.src_port = tcp_header->dst_port;
			send_hdr.dst_port = tcp_header->src_port;
			send_hdr.flags = p2ps_tcp_flags::RST;
			auto packet = generate_u2s_packet(send_hdr, nullptr, 0);

#ifdef RPCS3_PS4
			// Single-shot reply under the P2P locks: a full send buffer drops it instead of blocking
			constexpr int rst_flags = MSG_DONTWAIT;
#else
			constexpr int rst_flags = 0;
#endif
			if (SYS_NET_EINTR_RETRY(np::sendto_possibly_ipv6(p2p_socket, reinterpret_cast<char*>(packet.data()), ::size32(packet), reinterpret_cast<const sockaddr_in*>(&native_addr), rst_flags)) == -1)
			{
				sys_net.error("[P2PS] Error sending RST to sender to unbound P2PS: %s", get_last_error(false));
				return true;
			}

			sys_net.trace("[P2PS] Sent RST to sender to unbound P2PS");
			return true;
		}
	}

	sys_net.notice("Received a P2P packet with no bound target(dst_vport = %d)", dst_vport);
	return true;
}
