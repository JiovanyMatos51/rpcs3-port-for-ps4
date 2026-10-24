#include "stdafx.h"

#include "Emu/Memory/vm.h"
#include "Common/BufferUtils.h"
#include "Core/RSXReservationLock.hpp"
#include "RSXOffload.h"
#include "RSXThread.h"

#include "Utilities/lockless.h"

#include <thread>
#include "util/asm.hpp"

#ifdef RPCS3_PS4
#include <emmintrin.h>

#include "Emu/Cell/timers.hpp"
#include "Emu/System.h"
#include "util/ps4_memory_governor.hpp" // optional_thread_allowed (also includes util/ps4_safe_mode.hpp)
#include "util/ps4_safe_mode.hpp"       // rows rsx_copy_worker, copy_worker_lazy

void ps4_startup_checkpoint(const char* stage);

namespace rsx
{
	// 0.34: false with /data/rpcs3/rsx-plain-copy.flag (read per boot in dma_manager::init).
	static atomic_t<bool> s_ps4_streaming_copy{true};

	bool ps4_rsx_streaming_copy() noexcept
	{
		return s_ps4_streaming_copy.observe();
	}

	// 0.34: vertex uploads go to Mesa's host-visible ring, write-back cacheable memory on the console. A memcpy
	// there reads every destination line before writing it (read-for-ownership) and evicts L2 lines the guest
	// threads of the same module use. Non-temporal stores do neither. SSE2 only (Jaguar). The trailing sfence
	// orders the weakly ordered stores before anything that later hands the data to the GPU.
	void ps4_streaming_copy(void* dst, const void* src, u32 length) noexcept
	{
		auto* d = static_cast<u8*>(dst);
		auto* s = static_cast<const u8*>(src);

		// Short copies never underflow the remaining length
		const u32 head = std::min<u32>(static_cast<u32>((16 - (reinterpret_cast<uptr>(d) & 15)) & 15), length);
		std::memcpy(d, s, head);
		d += head;
		s += head;
		length -= head;

		for (u32 blocks = length / 64; blocks; blocks--, d += 64, s += 64)
		{
			const __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
			const __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16));
			const __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 32));
			const __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 48));
			_mm_stream_si128(reinterpret_cast<__m128i*>(d), v0);
			_mm_stream_si128(reinterpret_cast<__m128i*>(d + 16), v1);
			_mm_stream_si128(reinterpret_cast<__m128i*>(d + 32), v2);
			_mm_stream_si128(reinterpret_cast<__m128i*>(d + 48), v3);
		}

		std::memcpy(d, s, length % 64);
		_mm_sfence();
	}

	// 0.37: true on the RSX copy worker thread
	static thread_local bool t_ps4_copy_worker = false;

	// 0.37: RSX thread: the queued job it is running itself at a barrier (umax = none). A fault inside it is the RSX's
	// own (inline-copy path); syncs nested in that fault's handling never wait for this job.
	static thread_local u64 t_ps4_help_seq = umax;

	// Window counters of the 10 s 'RSX gameplay' field (ps4_copy_worker_text). worker_bytes: copied by the worker
	// thread; inline_bytes: copied on the RSX thread while the worker is on (small copies, full ring, ZCULL-locked
	// sources, jobs it ran at a barrier); helped: jobs the RSX ran at a barrier; sync_wait_us: RSX time waiting for
	// jobs the worker had claimed; ring_full: copies kept inline because the ring was full.
	static struct
	{
		atomic_t<bool> on{false};
		atomic_t<u64> worker_bytes{0}, inline_bytes{0}, helped{0}, sync_wait_us{0}, ring_full{0};
	} s_ps4_copy_stats;

	// 0.37: RSX copy worker. Vertex copies of rsx-copy-worker-min-kib.txt (default 16 KiB) or more go to a fixed ring
	// of 1024 jobs (no allocation per job; a full ring copies inline) and run on this thread while the RSX goes on.
	// Jobs are claimed in order by the worker or, at a barrier (ps4_copy_worker_barrier), by the RSX itself, so a
	// barrier never waits for more than the one copy the worker is running. Other syncs (submit, heap changes) only
	// wait: a fault in a copy the RSX ran there would re-enter the submit. Idle: 20 us spin, then an atomic wait the
	// producer ends only when the worker sleeps. Runs at the RSX priority. Multithreaded RSX stays off. The first
	// fault on a vertex source (memory the texture cache protects) turns the worker off for the session.
	struct dma_manager::ps4_copy_worker
	{
		static constexpr u32 ring_size = 1024;

		struct job
		{
			void* dst;
			const void* src;
			u32 length;
		};

		std::array<job, ring_size> m_jobs{};
		// Sequence + 1 of the newest finished job of each slot: job s is done when m_done[s % ring_size] > s (a slot is
		// reused only after its job finished)
		std::array<atomic_t<u64>, ring_size> m_done{};
		// Producer and consumer words on separate cache lines (padding, no over-aligned allocation)
		atomic_t<u64> m_produced{0};   // jobs pushed (RSX thread only)
		atomic_t<bool> m_accepting{true}; // false after join() or a source fault
		u8 m_pad0[64]{};
		atomic_t<u64> m_claimed{0};    // jobs claimed by the worker or by the RSX in sync()
		atomic_t<u64> m_done_hint{0};  // every job below it is done
		atomic_t<u64> m_running{umax}; // job the worker is copying (its fault range), umax = none
		atomic_t<u32> m_sleeping{0};
		u8 m_pad1[64]{};
		const bool m_raise_priority;
		// 0.39: RSX thread only (the single producer): 0 = stand-by until the first large copy, 1 = running, 2 = refused
		u8 m_ps4_start = 0;
		// 0.40: RSX thread only: large copies left before the stand-by worker asks the thread budget again
		u32 m_ps4_budget_wait = 0;

		explicit ps4_copy_worker(bool raise_priority) noexcept
			: m_raise_priority(raise_priority)
		{
		}

		bool has_work() const noexcept
		{
			return m_claimed.load() < m_produced.load();
		}

		bool try_claim(u64& seq) noexcept
		{
			u64 claimed = m_claimed.load();

			while (claimed < m_produced.load())
			{
				const u64 old = m_claimed.compare_and_swap(claimed, claimed + 1);

				if (old == claimed)
				{
					seq = claimed;
					return true;
				}

				claimed = old;
			}

			return false;
		}

		// The same copy as the inline path (strict rendering mode, which locks the source, keeps the worker off)
		void run(u64 seq) noexcept
		{
			const job& j = m_jobs[seq % ring_size];

			if (s_ps4_streaming_copy.observe())
				ps4_streaming_copy(j.dst, j.src, j.length);
			else
				std::memcpy(j.dst, j.src, j.length);

			m_done[seq % ring_size].release(seq + 1);
		}

		// Every job below `target` done, `except` (a job the calling thread is running itself) aside
		bool all_done_below(u64 target, u64 except = umax) noexcept
		{
			const u64 hint = m_done_hint.load();
			u64 seq = hint;

			while (seq < target && m_done[seq % ring_size].load() > seq)
			{
				seq++;
			}

			if (seq > hint)
			{
				m_done_hint.fetch_op([seq](u64& value)
				{
					if (value < seq)
					{
						value = seq;
					}
				});
			}

			while (seq < target && (seq == except || m_done[seq % ring_size].load() > seq))
			{
				seq++;
			}

			return seq >= target;
		}

		// After a fault on a vertex source: new copies stay inline (queued ones still finish here)
		void stop_accepting(const char* where) noexcept
		{
			if (m_accepting.exchange(false))
			{
				const std::string line = fmt::format("RSX: a vertex source faulted %s (protected memory); copy worker off for this session, "
					"vertex copies stay on the RSX thread", where);
				rsx_log.warning("PS4 %s", line);
				ps4_startup_checkpoint(line.c_str());
			}
		}

		void wake() noexcept
		{
			if (m_sleeping.load() && m_sleeping.exchange(0))
			{
				m_sleeping.notify_one();
			}
		}

		void operator()()
		{
			t_ps4_copy_worker = true;
			thread_ctrl::scoped_priority priority(m_raise_priority ? +1 : 0);

			while (thread_ctrl::state() != thread_state::aborting)
			{
				if (u64 seq; try_claim(seq))
				{
					m_running.release(seq);
					const u32 length = m_jobs[seq % ring_size].length;
					run(seq);
					m_running.release(umax);
					s_ps4_copy_stats.worker_bytes += length;
					continue;
				}

				bool work = false;

				for (const u64 start = get_system_time(); get_system_time() - start < 20;)
				{
					if (has_work())
					{
						work = true;
						break;
					}

					utils::pause();
				}

				if (work)
				{
					continue;
				}

				// Sleep; the producer stores m_produced before it reads m_sleeping, this thread the reverse
				m_sleeping.store(1);

				if (has_work())
				{
					m_sleeping.store(0);
					continue;
				}

				thread_ctrl::wait_on(m_sleeping, 1u, 2000);
				m_sleeping.store(0);
			}
		}
	};

	void ps4_copy_worker_barrier() noexcept
	{
		const auto dma = g_fxo->try_get<dma_manager>();

		if (!dma || !dma->ps4_copy_worker_on())
		{
			return;
		}

		const auto rsx = get_current_renderer();
		const bool on_rsx = rsx && rsx->is_current_thread();

		// false while the worker waits in its fault handler for the RSX: the RSX clears the fault (backend task) and
		// tries again; another thread cannot help it and returns
		while (!dma->ps4_worker_sync(on_rsx) && on_rsx)
		{
			rsx->on_semaphore_acquire_wait();
			utils::pause();
		}
	}

	std::string ps4_copy_worker_text(double frames)
	{
		auto& s = s_ps4_copy_stats;

		if (!s.on.observe() || frames <= 0)
		{
			return {};
		}

		return fmt::format("; copies: worker KiB/frame=%.0f inline KiB/frame=%.0f helped=%llu sync wait us/frame=%.0f ring full=%llu",
			s.worker_bytes.exchange(0) / 1024. / frames, s.inline_bytes.exchange(0) / 1024. / frames, s.helped.exchange(0),
			s.sync_wait_us.exchange(0) / frames, s.ring_full.exchange(0));
	}
}
#endif

namespace rsx
{
	struct dma_manager::offload_thread
	{
		lf_queue<transport_packet> m_work_queue;
		atomic_t<u64> m_enqueued_count = 0;
		atomic_t<u64> m_processed_count = 0;
		transport_packet* m_current_job = nullptr;

		thread_base* current_thread_ = nullptr;

		void operator ()()
		{
			if (!g_cfg.video.multithreaded_rsx)
			{
				// Abort if disabled
				return;
			}

			current_thread_ = thread_ctrl::get_current();
			ensure(current_thread_);

			if (g_cfg.core.thread_scheduler != thread_scheduler_mode::os)
			{
				thread_ctrl::set_thread_affinity_mask(thread_ctrl::get_affinity_mask(thread_class::rsx));
			}

			while (thread_ctrl::state() != thread_state::aborting)
			{
				for (auto&& job : m_work_queue.pop_all())
				{
					m_current_job = &job;

					switch (job.type)
					{
					case raw_copy:
					{
						const u32 vm_addr = vm::try_get_addr(job.src).first;
						rsx::reservation_lock<true, 1> rsx_lock(vm_addr, job.length, g_cfg.video.strict_rendering_mode && vm_addr);
						std::memcpy(job.dst, job.src, job.length);
						break;
					}
					case vector_copy:
					{
						std::memcpy(job.dst, job.opt_storage.data(), job.length);
						break;
					}
					case index_emulate:
					{
						write_index_array_for_non_indexed_non_native_primitive_to_buffer(static_cast<char*>(job.dst), static_cast<rsx::primitive_type>(job.aux_param0), job.length);
						break;
					}
					case callback:
					{
						rsx::get_current_renderer()->renderctl(job.aux_param0, job.src);
						break;
					}
					default: fmt::throw_exception("Unreachable");
					}

					m_processed_count.release(m_processed_count + 1);
				}

				m_current_job = nullptr;

				if (m_enqueued_count.load() == m_processed_count.load())
				{
					m_processed_count.notify_all();
					std::this_thread::yield();
				}
			}

			m_processed_count = -1;
			m_processed_count.notify_all();
		}

		static constexpr auto thread_name = "RSX Offloader"sv;
	};

	// initialization
	void dma_manager::init()
	{
#ifdef RPCS3_PS4
		s_ps4_streaming_copy.release(!fs::is_file("/data/rpcs3/rsx-plain-copy.flag"));
		ps4_startup_checkpoint(s_ps4_streaming_copy.load()
			? "RSX: vertex upload copies of 4 KiB or more use streaming stores (rsx-plain-copy.flag disables)"
			: "RSX: vertex upload copies use memcpy (rsx-plain-copy.flag)");

		// 0.37: RSX copy worker. Only where no guest-visible RSX buffer is read or written on the CPU (no WCB/WDB/RCB/
		// RDB, no strict rendering mode: their sources fault into flushes), on Vulkan (its submit, ring reclaim and
		// access-violation drain sync the worker) and without the upstream offloader.
		if (m_ps4_worker)
		{
			sync();
			m_ps4_worker->m_accepting.release(false);
			*m_ps4_worker = thread_state::aborting;
			m_ps4_worker.reset();
		}

		s_ps4_copy_stats.on.release(false);

		std::string ps4_worker_off;

		// 0.40: row rsx_copy_worker (util/ps4_safe_mode.hpp), read per renderer: off with rsx-copy-worker-off.flag or
		// at safe-mode level 3 (the copies then run inline, as in 0.36)
		if (!utils::ps4::safe_mode::enabled(utils::ps4::safe_mode::feature::rsx_copy_worker))
			ps4_worker_off = fs::is_file("/data/rpcs3/rsx-copy-worker-off.flag") ? "rsx-copy-worker-off.flag" : "safe mode level 3";
		else if (g_cfg.video.renderer != video_renderer::vulkan)
			ps4_worker_off = "Vulkan renderer only";
		else if (g_cfg.video.multithreaded_rsx)
			ps4_worker_off = "Multithreaded RSX is on";
		else if (g_cfg.video.write_color_buffers || g_cfg.video.write_depth_buffer || g_cfg.video.read_color_buffers ||
			g_cfg.video.read_depth_buffer || g_cfg.video.strict_rendering_mode)
			ps4_worker_off = "the title reads or writes RSX buffers on the CPU (WCB/WDB/RCB/RDB or strict rendering mode)";

		m_ps4_worker_min = 16 * 1024;

		if (const fs::file f{"/data/rpcs3/rsx-copy-worker-min-kib.txt"}; f)
		{
			const std::string text = f.size() < 16 ? f.to_string() : std::string();
			char* end = nullptr;
			const unsigned long value = std::strtoul(text.c_str(), &end, 10);

			if (end != text.c_str() && value >= 4 && value <= 4096)
				m_ps4_worker_min = static_cast<u32>(value) * 1024;
			else
				ps4_startup_checkpoint("RSX: rsx-copy-worker-min-kib.txt ignored (expected 4..4096)");
		}

		if (ps4_worker_off.empty())
		{
			// The RSX priority: elevated unless rsx-normal-priority.flag (read again here: init runs before the RSX
			// thread applies its own priority)
			const bool raise = !fs::is_file("/data/rpcs3/rsx-normal-priority.flag");
			auto worker = std::make_shared<named_thread<ps4_copy_worker>>(stx::try_launch, std::string("RSX copy worker"), raise);

			// 0.39: stand-by until the first copy it takes (ps4_worker_push); row copy_worker_lazy
			const bool lazy = utils::ps4::safe_mode::enabled(utils::ps4::safe_mode::feature::copy_worker_lazy);

			if (lazy || worker->ps4_try_start())
			{
				worker->m_ps4_start = lazy ? 0 : 1;
				m_ps4_worker = std::move(worker);
				s_ps4_copy_stats.on.release(true);
				ps4_startup_checkpoint(fmt::format("RSX: vertex copies of %u KiB or more run on 'RSX copy worker' (bounded ring 1024, help-on-sync; %s; rsx-copy-worker-off.flag disables)",
					m_ps4_worker_min / 1024, lazy ? "started at the first such copy" : "started now").c_str());
			}
			else
			{
				ps4_worker_off = "the console refused the thread";
			}
		}

		if (!ps4_worker_off.empty())
		{
			ps4_startup_checkpoint(fmt::format("RSX: copy worker off (%s); vertex copies stay on the RSX thread", ps4_worker_off).c_str());
		}

		// 0.29: with Multithreaded RSX off (the PS4 default) the offloader body returns at once but its thread
		// still held a 2 MiB libkernel stack until Emu stop (unjoined), one of the ~81 stacks live when Demon's
		// Souls hit EAGAIN in 0.28. Do not create it then; every user below accepts a null m_thread. The
		// setting is not dynamic and the fxo object is rebuilt per boot, so a game with MT RSX on still gets it.
		if (!g_cfg.video.multithreaded_rsx)
		{
			m_thread.reset();
			rsx_log.notice("PS4: RSX Offloader thread not created (Multithreaded RSX is off)");
			return;
		}
#endif
		m_thread = std::make_shared<named_thread<offload_thread>>();
	}

	// General transport
	void dma_manager::copy(void *dst, std::vector<u8>& src, u32 length) const
	{
#ifdef RPCS3_PS4
		// 0.29: no offloader thread (see init()) means the inline path.
		if (length <= max_immediate_transfer_size || !g_cfg.video.multithreaded_rsx || !m_thread)
#else
		if (length <= max_immediate_transfer_size || !g_cfg.video.multithreaded_rsx)
#endif
		{
			std::memcpy(dst, src.data(), length);
#ifdef RPCS3_PS4
			ps4_rsx_phase_add(g_ps4_rsx_phase.upload_bytes, length);
#endif
		}
		else
		{
			m_thread->m_enqueued_count++;
			m_thread->m_work_queue.push(dst, src, length);
		}
	}

	void dma_manager::copy(void *dst, void *src, u32 length) const
	{
#ifdef RPCS3_PS4
		// 0.37: large copies run on the RSX copy worker (inline when it cannot take them)
		if (m_ps4_worker && length >= m_ps4_worker_min && ps4_worker_push(dst, src, length))
		{
			ps4_rsx_phase_add(g_ps4_rsx_phase.upload_bytes, length);
			return;
		}

		// 0.29: no offloader thread (see init()) means the inline path.
		if (length <= max_immediate_transfer_size || !g_cfg.video.multithreaded_rsx || !m_thread)
#else
		if (length <= max_immediate_transfer_size || !g_cfg.video.multithreaded_rsx)
#endif
		{
			const u32 vm_addr = vm::try_get_addr(src).first;
			rsx::reservation_lock<true, 1> rsx_lock(vm_addr, length, g_cfg.video.strict_rendering_mode && vm_addr);
#ifdef RPCS3_PS4
			// 0.34: see ps4_streaming_copy (rsx-plain-copy.flag keeps memcpy).
			if (length >= 4096 && s_ps4_streaming_copy.observe())
				ps4_streaming_copy(dst, src, length);
			else
				std::memcpy(dst, src, length);
			ps4_rsx_phase_add(g_ps4_rsx_phase.upload_bytes, length);

			if (m_ps4_worker)
			{
				s_ps4_copy_stats.inline_bytes += length;
			}
#else
			std::memcpy(dst, src, length);
#endif
		}
		else
		{
			m_thread->m_enqueued_count++;
			m_thread->m_work_queue.push(dst, src, length);
		}
	}

	// Vertex utilities
	void dma_manager::emulate_as_indexed(void *dst, rsx::primitive_type primitive, u32 count)
	{
#ifdef RPCS3_PS4
		// 0.29: no offloader thread (see init()) means the inline path.
		if (!g_cfg.video.multithreaded_rsx || !m_thread)
#else
		if (!g_cfg.video.multithreaded_rsx)
#endif
		{
			write_index_array_for_non_indexed_non_native_primitive_to_buffer(
				static_cast<char*>(dst), primitive, count);
		}
		else
		{
			m_thread->m_enqueued_count++;
			m_thread->m_work_queue.push(dst, primitive, count);
		}
	}

	// Backend callback
	void dma_manager::backend_ctrl(u32 request_code, void* args)
	{
		ensure(g_cfg.video.multithreaded_rsx);
#ifdef RPCS3_PS4
		// 0.29: callers queue only with MT RSX on, when init() created the thread; report instead of a null deref.
		ensure(m_thread);
#endif

		m_thread->m_enqueued_count++;
		m_thread->m_work_queue.push(request_code, args);
	}

#ifdef RPCS3_PS4
	// RSX thread only (the single producer)
	bool dma_manager::ps4_worker_push(void* dst, void* src, u32 length) const
	{
		auto& w = *m_ps4_worker;

		if (!w.m_accepting.observe() || !ps4_is_rsx_thread())
		{
			return false;
		}

		// A source on a page ZCULL locks would fault on the worker (see ps4_zcull_range_locked)
		if (const auto [vm_addr, ok] = vm::try_get_addr(src); !ok || ps4_zcull_range_locked(vm_addr, length))
		{
			return false;
		}

		// 0.39: the first copy it takes starts the stand-by worker (a refusal keeps every copy inline)
		if (w.m_ps4_start != 1)
		{
			if (w.m_ps4_start == 2 || Emu.IsStopped())
			{
				return false;
			}

			// 0.40: an optional thread never takes one of the last slots of the console's thread cap, which stay for the
			// game: the copy runs inline, and the budget is asked again 256 large copies later
			if (w.m_ps4_budget_wait)
			{
				w.m_ps4_budget_wait--;
				return false;
			}

			if (!utils::ps4::governor::optional_thread_allowed("RSX copy worker", 1))
			{
				w.m_ps4_budget_wait = 256;
				return false;
			}

			if (!w.ps4_try_start())
			{
				w.m_ps4_start = 2;
				w.m_accepting.release(false);
				ps4_startup_checkpoint("RSX: copy worker refused at its first copy (thread cap); vertex copies stay on the RSX thread");
				return false;
			}

			w.m_ps4_start = 1;
			ps4_startup_checkpoint("PS4 threads: RSX copy worker started on first use");
		}

		constexpr u32 ring_size = ps4_copy_worker::ring_size;
		const u64 seq = w.m_produced.observe();

		// The slot is free once the job pushed ring_size jobs earlier finished
		if (seq >= ring_size && w.m_done[seq % ring_size].load() <= seq - ring_size)
		{
			s_ps4_copy_stats.ring_full++;
			return false;
		}

		w.m_jobs[seq % ring_size] = {dst, src, length};
		w.m_produced.store(seq + 1);
		w.wake();
		return true;
	}

	bool dma_manager::ps4_worker_sync(bool help) const
	{
		auto& w = *m_ps4_worker;
		const u64 target = w.m_produced.load();

		// A sync nested in the handling of the RSX's own fault inside a job it runs never waits for that job
		if (w.all_done_below(target, t_ps4_help_seq)) [[likely]]
		{
			return true;
		}

		if (t_ps4_copy_worker)
		{
			// The worker never waits for its own job
			return true;
		}

		if (const auto rsxthr = get_current_renderer(); rsxthr && rsxthr->is_current_thread())
		{
			if (m_mem_fault_flag)
			{
				// As upstream: the worker sits in its fault handler until the RSX clears the fault, which may be what
				// this RSX code is doing (a submit or flush inside the fault recovery)
				return false;
			}

			if (help && t_ps4_help_seq == umax)
			{
				// Run the jobs nobody claimed yet, in order (the same copies the worker would make)
				for (u64 seq; w.try_claim(seq);)
				{
					const u32 length = w.m_jobs[seq % ps4_copy_worker::ring_size].length;
					t_ps4_help_seq = seq;
					w.run(seq);
					t_ps4_help_seq = umax;
					s_ps4_copy_stats.helped++;
					s_ps4_copy_stats.inline_bytes += length;
				}
			}

			if (!w.all_done_below(target, t_ps4_help_seq))
			{
				// Jobs the worker claimed. Its copies need nothing from the RSX unless one faults: then the RSX clears
				// the fault (backend task, also allowed inside a submit) and the copy goes on.
				const u64 start = get_system_time();
				w.wake();

				while (!w.all_done_below(target, t_ps4_help_seq))
				{
					if (m_mem_fault_flag)
					{
						rsxthr->on_semaphore_acquire_wait();
					}

					utils::pause();
				}

				s_ps4_copy_stats.sync_wait_us += get_system_time() - start;
			}

			return true;
		}

		// Other threads (a PPU or SPU flushing an access violation) wait for the jobs pushed before the call, unless the
		// worker sits in its fault handler: it needs the RSX, which may be parked for this thread's flush
		w.wake();

		while (!w.all_done_below(target))
		{
			if (m_mem_fault_flag)
			{
				return false;
			}

			utils::pause();
		}

		return true;
	}
#endif

	// Synchronization
	bool dma_manager::is_current_thread() const
	{
#ifdef RPCS3_PS4
		// 0.37: the copy worker takes the offloader's fault path (get_fault_range reports its current copy). Called on
		// every access violation: a fault of the RSX inside a queued copy it runs is its own (as for an inline copy),
		// and either kind turns the worker off.
		if (t_ps4_copy_worker)
		{
			return true;
		}

		if (t_ps4_help_seq != umax && m_ps4_worker)
		{
			m_ps4_worker->stop_accepting("while the RSX ran a queued copy");
			return false;
		}

		// 0.29: called on every access violation (VKGSRender::on_access_violation); no offloader, never true.
		if (!m_thread)
		{
			return false;
		}
#endif

		if (auto cpu = thread_ctrl::get_current())
		{
			return m_thread->current_thread_ == cpu;
		}

		return false;
	}

	bool dma_manager::sync() const
	{
#ifdef RPCS3_PS4
		// 0.37: the copy worker's queued copies first (waits only; see ps4_copy_worker_barrier for the RSX's help)
		if (m_ps4_worker && !ps4_worker_sync(false))
		{
			return false;
		}

		// 0.29: sync() is called unconditionally (nv47_sync, data_heap, VKGSRender flush/dtor); without an
		// offloader nothing is ever queued, so it is always in sync.
		if (!m_thread)
		{
			return true;
		}
#endif

		auto& _thr = *m_thread;

		if (_thr.m_enqueued_count.load() <= _thr.m_processed_count.load()) [[likely]]
		{
			// Nothing to do
			return true;
		}

		if (auto rsxthr = get_current_renderer(); rsxthr->is_current_thread())
		{
			if (m_mem_fault_flag)
			{
				// Abort if offloader is in recovery mode
				return false;
			}

			while (_thr.m_enqueued_count.load() > _thr.m_processed_count.load())
			{
				rsxthr->on_semaphore_acquire_wait();
				utils::pause();
			}
		}
		else
		{
			while (_thr.m_enqueued_count.load() > _thr.m_processed_count.load())
				utils::pause();
		}

		return true;
	}

	void dma_manager::join()
	{
#ifdef RPCS3_PS4
		// 0.37: finish the queued copies and stop the copy worker (the object stays: other threads may still sync)
		if (m_ps4_worker)
		{
			sync();
			m_ps4_worker->m_accepting.release(false);
			*m_ps4_worker = thread_state::aborting;
			s_ps4_copy_stats.on.release(false);
		}

		// 0.29: nothing to stop when init() did not create the offloader (or init() never ran).
		if (!m_thread)
		{
			return;
		}
#endif

		sync();
		*m_thread = thread_state::aborting;
	}

	void dma_manager::set_mem_fault_flag()
	{
		ensure(is_current_thread()); // "Access denied"
		m_mem_fault_flag.release(true);
	}

	void dma_manager::clear_mem_fault_flag()
	{
		ensure(is_current_thread()); // "Access denied"
		m_mem_fault_flag.release(false);
	}

	// Fault recovery
	utils::address_range32 dma_manager::get_fault_range(bool writing) const
	{
#ifdef RPCS3_PS4
		// 0.37: a fault of the copy worker: the copy it is running (writing: the ring, reading: the guest source)
		if (t_ps4_copy_worker)
		{
			auto& worker = *ensure(m_ps4_worker.get());
			const u64 seq = worker.m_running.load();
			ensure(seq != umax);
			worker.stop_accepting("on the copy worker");
			const auto& job = worker.m_jobs[seq % ps4_copy_worker::ring_size];
			return utils::address_range32::start_length(vm::get_addr(writing ? job.dst : job.src), job.length);
		}

		// 0.29: only reachable after is_current_thread() returned true, which needs the thread; keep the
		// ensure() report (same as a missing job) instead of a null dereference if that ever changes.
		const auto m_current_job = ensure(m_thread ? m_thread->m_current_job : nullptr);
#else
		const auto m_current_job = ensure(m_thread->m_current_job);
#endif

		void *address = nullptr;
		u32 range = m_current_job->length;

		switch (m_current_job->type)
		{
		case raw_copy:
			address = (writing) ? m_current_job->dst : m_current_job->src;
			break;
		case vector_copy:
			ensure(writing);
			address = m_current_job->dst;
			break;
		case index_emulate:
			ensure(writing);
			address = m_current_job->dst;
			range = get_index_count(static_cast<rsx::primitive_type>(m_current_job->aux_param0), m_current_job->length);
			break;
		default:
			fmt::throw_exception("Unreachable");
		}

		return utils::address_range32::start_length(vm::get_addr(address), range);
	}
}
