#pragma once

#include <vector>
#include <map>
#include "Utilities/mutex.h"
#include "Emu/Cell/PPUThread.h"

#include "nt_p2p_port.h"

#ifdef RPCS3_PS4
#include "Emu/IdManager.h"
#endif

struct base_network_thread
{
	void add_ppu_to_awake(ppu_thread* ppu);
	void del_ppu_to_awake(ppu_thread* ppu);

	shared_mutex mutex_ppu_to_awake;
	std::vector<ppu_thread*> ppu_to_awake;
	shared_mutex mutex_thread_loop;

	void wake_threads();
};

struct network_thread : base_network_thread
{
#ifdef RPCS3_PS4
	// 0.39: started by the first blocking socket poll: this thread only works while num_polls is non-zero, and its
	// 0 -> 1 step is notified (lv2_socket::poll_queue)
	ps4_lazy_wake_counter<network_thread> num_polls{0};
	static constexpr bool ps4_lazy = true;
	static void ps4_lazy_start();
#else
	atomic_t<u32> num_polls = 0;
#endif

	static constexpr auto thread_name = "Network Thread";

	void operator()();
};

struct p2p_thread : base_network_thread
{
	shared_mutex list_p2p_ports_mutex;
	std::map<u16, nt_p2p_port> list_p2p_ports;
#ifdef RPCS3_PS4
	// 0.39: started by the first P2P port (p2p_thread::create_p2p_port notifies the 0 -> 1 step)
	ps4_lazy_wake_counter<p2p_thread> num_p2p_ports{0};
	static constexpr bool ps4_lazy = true;
	static void ps4_lazy_start();
#else
	atomic_t<u32> num_p2p_ports = 0;
#endif

	static constexpr auto thread_name = "Network P2P Thread";

	p2p_thread();

	void create_p2p_port(u16 p2p_port);

	void bind_sce_np_port();
	void operator()();
};

using network_context = named_thread<network_thread>;
using p2p_context = named_thread<p2p_thread>;

#ifdef RPCS3_PS4
// Not while the object is still being built (g_fxo marks it initialized afterwards)
inline void network_thread::ps4_lazy_start()
{
	if (g_fxo->is_init<network_context>())
	{
		g_fxo->get<network_context>().ps4_start_lazy();
	}
}

inline void p2p_thread::ps4_lazy_start()
{
	if (g_fxo->is_init<p2p_context>())
	{
		g_fxo->get<p2p_context>().ps4_start_lazy();
	}
}
#endif
