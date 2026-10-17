#pragma once

#include "util/types.hpp"
#include "Utilities/address_range.h"
#include "gcm_enums.h"

#include <vector>

template <typename T>
class named_thread;

namespace rsx
{
	class dma_manager
	{
		enum op
		{
			raw_copy = 0,
			vector_copy = 1,
			index_emulate = 2,
			callback = 3
		};

		struct transport_packet
		{
			op type{};
			std::vector<u8> opt_storage{};
			void* src{};
			void* dst{};
			u32 length{};
			u32 aux_param0{};
			u32 aux_param1{};

			transport_packet(void *_dst, void *_src, u32 len)
				: type(op::raw_copy), src(_src), dst(_dst), length(len)
			{}

			transport_packet(void *_dst, std::vector<u8>& _src, u32 len)
				: type(op::vector_copy), opt_storage(std::move(_src)), dst(_dst), length(len)
			{}

			transport_packet(void *_dst, rsx::primitive_type prim, u32 len)
				: type(op::index_emulate), dst(_dst), length(len), aux_param0(static_cast<u8>(prim))
			{}

			transport_packet(u32 command, void* args)
				: type(op::callback), src(args), aux_param0(command)
			{}

			transport_packet(const transport_packet&) = delete;
			transport_packet& operator=(const transport_packet&) = delete;
		};

		atomic_t<bool> m_mem_fault_flag = false;

		struct offload_thread;
		std::shared_ptr<named_thread<offload_thread>> m_thread;

		// TODO: Improved benchmarks here; value determined by profiling on a Ryzen CPU, rounded to the nearest 512 bytes
		const u32 max_immediate_transfer_size = 3584;

#ifdef RPCS3_PS4
		// 0.37: RSX copy worker (RSXOffload.cpp): vertex copies of m_ps4_worker_min bytes or more run on a helper
		// thread through a bounded ring; null while off. Set by init() on the RSX thread.
		struct ps4_copy_worker;
		std::shared_ptr<named_thread<ps4_copy_worker>> m_ps4_worker;
		u32 m_ps4_worker_min = 16 * 1024;

		bool ps4_worker_push(void* dst, void* src, u32 length) const;
#endif

	public:
		dma_manager() = default;

#ifdef RPCS3_PS4
		bool ps4_copy_worker_on() const noexcept
		{
			// Pointer compare: an operator on the shared_ptr itself would instantiate named_thread<ps4_copy_worker>
			// (argument-dependent lookup) where the worker type is incomplete
			return m_ps4_worker.get() != nullptr;
		}

		// Waits for the copies queued before the call. `help`: the RSX thread also runs the unclaimed ones itself (only
		// from ps4_copy_worker_barrier: a fault there is handled like an inline copy's). false: the worker sits in its
		// fault handler and needs the RSX (RSX thread at entry, or any other thread).
		bool ps4_worker_sync(bool help) const;
#endif

		// initialization
		void init();

		// General tranport
		void copy(void *dst, std::vector<u8>& src, u32 length) const;
		void copy(void *dst, void *src, u32 length) const;

		// Vertex utilities
		void emulate_as_indexed(void *dst, rsx::primitive_type primitive, u32 count);

		// Renderer callback
		void backend_ctrl(u32 request_code, void* args);

		// Synchronization
		bool is_current_thread() const;
		bool sync() const;
		void join();
		void set_mem_fault_flag();
		void clear_mem_fault_flag();

		// Fault recovery
		utils::address_range32 get_fault_range(bool writing) const;
	};

#ifdef RPCS3_PS4
	// 0.34: copy with non-temporal stores and a trailing sfence (SSE2, any length). For write-back memory the GPU reads
	// after a submit: no read-for-ownership of the destination and no eviction of the guest threads' L2 lines.
	void ps4_streaming_copy(void* dst, const void* src, u32 length) noexcept;
#endif
}
