#pragma once

#include "util/types.hpp"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Host/MM.h"

#include "context_accessors.define.h"

namespace rsx
{
	namespace util
	{
		template <bool FlushDMA, bool FlushPipe>
		static void write_gcm_label(context* ctx, u32 type, u32 address, u32 data)
		{
#ifdef RPCS3_PS4
			// 0.34: every label write (no-op, backend-handled or direct) goes to the STALL label ring.
			ps4_label_ring_note(ps4_label_release, address, data, RSX(ctx)->fifo_ctrl->get_pos());
#endif
			const bool is_flip_sema = (address == (RSX(ctx)->label_addr + 0x10) || address == (RSX(ctx)->device_addr + 0x30));
			if (!is_flip_sema)
			{
				// First, queue the GPU work. If it flushes the queue for us, the following routines will be faster.
				const bool handled = RSX(ctx)->get_backend_config().supports_host_gpu_labels && RSX(ctx)->release_GCM_label(type, address, data);

#ifdef RPCS3_PS4
				// The label page may be one ZCULL keeps without access (see ps4_rsx_label_sudo)
				if (*ps4_label_ptr<RsxSemaphore>(address) == data)
#else
				if (vm::_ref<RsxSemaphore>(address) == data)
#endif
				{
					// It's a no-op to write the same value (although there is a delay in real-hw so it's more accurate to allow GPU label in this case)
					// There is no possible way for the guest to know that the label has been processed so we can skip MM sync here.
					return;
				}

				if constexpr (FlushDMA || FlushPipe)
				{
					if constexpr (FlushDMA)
					{
						// Release op must be acoompanied by MM flush.
						// FlushPipe implicitly does a MM flush but FlushDMA does not. Trigger the flush here
						rsx::mm_flush();

						// If the backend handled the request, this call will basically be a NOP
						g_fxo->get<rsx::dma_manager>().sync();
					}

					if constexpr (FlushPipe)
					{
						// Syncronization point, may be associated with memory changes without actually changing addresses
						RSX(ctx)->m_graphics_state |= rsx::pipeline_state::fragment_program_needs_rehash;

						// Manually flush the pipeline.
						// It is possible to stream report writes using the host GPU, but that generates too much submit traffic.
						RSX(ctx)->sync();
					}
				}

				if (handled)
				{
					// Backend will handle it, nothing to write.
					return;
				}
			}

#ifdef RPCS3_PS4
			// 0.37: the guest never sees the label before earlier draws read their vertex sources (RSX copy worker),
			// and a label in a line SPU code holds a reservation on breaks it and wakes the sleepers, as on HW.
			ps4_copy_worker_barrier();
			*ps4_label_ptr<atomic_t<RsxSemaphore>>(address) = data;
			ps4_label_ring_touched(address, ps4_rsx_touch_reservations(address, sizeof(RsxSemaphore), true));
#else
			vm::write<atomic_t<RsxSemaphore>>(address, data);
#endif
		}
	}
}

#include "context_accessors.undef.h"
