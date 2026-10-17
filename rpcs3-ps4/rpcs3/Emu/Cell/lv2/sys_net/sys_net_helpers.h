#pragma once

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
#ifdef __clang__
#pragma GCC diagnostic pop
#endif
#endif

#include "Emu/Cell/lv2/sys_net.h"

#ifdef RPCS3_PS4
#include <cerrno>

// FreeBSD never restarts poll() after a signal, and any socket call can come back with EINTR when a host signal lands
// on the thread (the port's sampling profiler signals every thread once a minute, ~500 Hz during a burst). The host
// sockets are non-blocking, so a retry is immediate; an EINTR that is still left after the retries is reported by
// convert_error as would-block. Expands to the plain call on other platforms.
#define SYS_NET_EINTR_RETRY(...) ([&]() { auto r_ = (__VA_ARGS__); for (int n_ = 0; r_ == -1 && errno == EINTR && n_ < 8; n_++) { r_ = (__VA_ARGS__); } return r_; }())
#else
#define SYS_NET_EINTR_RETRY(...) (__VA_ARGS__)
#endif

int get_native_error();
sys_net_error convert_error(bool is_blocking, int native_error, bool is_connecting = false);
sys_net_error get_last_error(bool is_blocking, bool is_connecting = false);
sys_net_sockaddr native_addr_to_sys_net_addr(const ::sockaddr_storage& native_addr);
::sockaddr_in sys_net_addr_to_native_addr(const sys_net_sockaddr& sn_addr);
bool is_ip_public_address(const ::sockaddr_in& addr);
u32 network_clear_queue(ppu_thread& ppu);
void clear_ppu_to_awake(ppu_thread& ppu);
be_t<u32> resolve_binding_ip();

#ifdef _WIN32
void windows_poll(std::vector<pollfd>& fds, unsigned long nfds, int timeout, std::vector<bool>& connecting);
#endif
