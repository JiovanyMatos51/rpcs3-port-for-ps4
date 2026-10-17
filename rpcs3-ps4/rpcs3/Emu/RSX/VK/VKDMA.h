#pragma once
#include "vkutils/buffer_object.h"

#ifdef RPCS3_PS4
#include <atomic>
#include <functional>

namespace utils::ps4::gpu
{
	struct vram_snapshot;
}
#endif

namespace vk
{
	using dma_mapping_handle = std::pair<u32, vk::buffer*>;

	dma_mapping_handle map_dma(u32 local_address, u32 length);
	void load_dma(u32 local_address, u32 length);
	void flush_dma(u32 local_address, u32 length);
	void unmap_dma(u32 local_address, u32 length);

	void clear_dma_resources();

#ifdef RPCS3_PS4
	// 0.31: video memory telemetry and GPU health hooks of the Vulkan backend (util/ps4_gpu_health.hpp).
	// Bytes held by the host-visible DMA mirror blocks (VKDMA.cpp). Any thread.
	u64 ps4_dma_pool_bytes();
	// 0.38: releases mirror blocks (utils::ps4::vram::plan_dma_release): those without use (map, load, flush) for
	// idle_us, then, while the pool stays above cap, the least recently used ones idle for min_age_us. 'pinned' is asked
	// for each candidate's range without the pool lock held (true: the block stays). Nothing when the pool lock is busy.
	// RSX thread. Returns the bytes released (their buffers go through the garbage collector).
	u64 ps4_dma_release_idle(u64 now_us, u64 idle_us, u64 cap, u64 min_age_us, const std::function<bool(const utils::address_range32&)>& pinned);
	// Blocks released by ps4_dma_release_idle in this process.
	u32 ps4_dma_blocks_released();
	// Per-heap usage and budget from VMA plus the allocator caps (vkutils/memory.cpp). RSX thread.
	void ps4_query_vram_heaps(utils::ps4::gpu::vram_snapshot& out);
	// Marks the device as lost; the first call also writes startup.log and the running game's adapt note
	// (vkutils/shared.cpp). Any thread, allocation-free.
	void ps4_record_device_lost(const char* where) noexcept;
	// Records a severe video memory event (a trim at severity severe or fatal, or an allocation that ran out of
	// video memory): a startup.log line for the first event and then at most one every 30 s, and the running game
	// is marked for the smaller video memory budget on its next boots (once per game session).
	// VKResourceManager.cpp. Any thread, allocation-free.
	void ps4_note_vram_pressure(const char* what) noexcept;

	// 0.32: switches of the readback (DMA) path from /data/rpcs3/dma.flag, one "key=value" per line like vram.flag
	// (VKTextureCache.cpp). Read once when the renderer starts, which also logs the values to startup.log; before
	// that, and for a missing file or key, the defaults apply. Any thread.
	void ps4_load_dma_flags();
	// events=label|vk (label): host-visible DMA events use the gpu_label backend (a CP DMA fill into one long-lived
	// host-visible buffer) instead of VkEvents, which the host has never seen set on this console.
	bool ps4_dma_events_use_labels();
	// isolate=on|off (on): an RSX-side readback is recorded in its own submission after the frame so far.
	bool ps4_dma_isolate();
	// speculate=on|off (off): predicted (speculative) readbacks appended to the frame's command buffer.
	bool ps4_dma_speculate();
	// trace=on|off (on): GPU progress markers between the steps of every readback (see ps4_dma_trace_report).
	bool ps4_dma_trace_enabled();
	// cpu_swap=on|off (off): small byteswapped colour readbacks are copied straight into the DMA mirror and
	// byteswapped by the CPU after the flush (no scratch buffer, no shuffle shader, no buffer copy).
	bool ps4_dma_cpu_swap();
	// Logs which step of which readback the GPU finished last and which one was recorded last (RPCS3.log and the
	// fsync'd startup.log), so a GPU hang names the step it stopped in. No-op while no readback was traced.
	// Any thread.
	void ps4_dma_trace_report(const char* where) noexcept;
	// The same report as an early snapshot while a GPU wait is still running: a GPU fault can get the process killed
	// before the 2 s / 10 s reports. At most a few per session, only once a readback was traced. Any thread.
	void ps4_dma_trace_snapshot(const char* where) noexcept;

	// 0.32: true on a thread that runs VKGSRender::on_access_violation's flush while it holds its flush-queue
	// reference (VKGSRender.cpp). Submissions from such a thread are expected; vk::queue_submit logs other ones.
	// The RSX thread is normally parked meanwhile, but not always: a hard sync can serve the request without parking.
	extern thread_local bool g_ps4_rsx_parked_for_flush;

	// Hang watchdog for an untimed GPU fence wait (vkutils/sync.cpp). Call poll() after every unsuccessful poll with
	// the 0-based poll count, and done() once the wait has finished. When the fence has not signalled for 10 s it
	// reports a suspected hang once (RPCS3.log, startup.log, one toast, gpu::set_suspect_hang) and the caller simply
	// keeps waiting: slow GPU work can still finish, and throwing here would only turn a stall into a fatal.
	class ps4_fence_watchdog
	{
	public:
		void poll(u32 iteration)
		{
			// The first 64 polls are short pauses; after that the clock is read on every 64th poll only, which
			// is tens of milliseconds apart once the wait sleeps between polls.
			if (iteration >= 64 && !(iteration & 63))
			{
				check();
			}
		}

		void done()
		{
			if (m_reported)
			{
				recovered();
			}
		}

	private:
		void check();
		void recovered();

		u64 m_start_us = 0;
		u64 m_last_us = 0;
		bool m_reported = false;
		bool m_trace_snapshot_taken = false; // early readback-trace snapshot of this wait (see check())
	};
#endif

	class dma_block
	{
	protected:
		struct
		{
			dma_block* parent = nullptr;
			u32 block_offset = 0;
		}
		inheritance_info;

		u32 base_address = 0;
		u8* memory_mapping = nullptr;
		std::unique_ptr<buffer> allocated_memory;
#ifdef RPCS3_PS4
		// 0.38: last map/load/flush of this head block (get_system_time), for the age-out. Any thread.
		std::atomic<u64> m_ps4_last_use_us{ 0 };
		void ps4_touch();
#endif

		virtual void allocate(const render_device& dev, usz size);
		virtual void free();
		virtual void* map_range(const utils::address_range32& range);
		virtual void unmap();

	public:

		dma_block() = default;
		virtual ~dma_block();

		virtual void init(const render_device& dev, u32 addr, usz size);
		virtual void init(dma_block* parent, u32 addr, usz size);
		virtual void flush(const utils::address_range32& range);
		virtual void load(const utils::address_range32& range);
		std::pair<u32, buffer*> get(const utils::address_range32& range);

		u32 start() const;
		u32 end() const;
		u32 size() const;

		dma_block* head();
		const dma_block* head() const;
		virtual void set_parent(dma_block* parent);
		virtual void extend(const render_device& dev, usz new_size);
#ifdef RPCS3_PS4
		u64 ps4_last_use() const
		{
			return m_ps4_last_use_us.load(std::memory_order_relaxed);
		}
#endif
	};

	class dma_block_EXT: public dma_block
	{
	private:
		void allocate(const render_device& dev, usz size) override;
		void* map_range(const utils::address_range32& range) override;
		void unmap() override;

	public:
		void flush(const utils::address_range32& range) override;
		void load(const utils::address_range32& range) override;
	};
}
