#include "stdafx.h"
#include "RSXThread.h"
#ifdef RPCS3_PS4
#include "ps4_launcher.h"
#endif

#include "Capture/rsx_capture.h"
#include "Common/surface_store.h"
#include "Core/RSXReservationLock.hpp"
#include "Core/RSXEngLock.hpp"
#include "Host/MM.h"
#include "Host/RSXDMAWriter.h"
#include "NV47/HW/context.h"
#include "NV47/HW/context_accessors.define.h"
#include "Program/GLSLCommon.h"
#include "rsx_methods.h"

#include "gcm_printing.h"
#include "RSXDisAsm.h"

#include "Emu/System.h"
#include "Emu/system_utils.hpp"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/lv2/sys_time.h"
#include "Emu/Cell/Modules/cellGcmSys.h"
#include "util/serialization_ext.hpp"
#include "Overlays/overlay_perf_metrics.h"
#include "Overlays/overlay_debug_overlay.h"
#include "Overlays/overlay_manager.h"

#include "Utilities/date_time.h"

#include "util/asm.hpp"
#ifdef RPCS3_PS4
#include "Emu/Memory/vm_reservation.h"
#include "ps4_readback_state.h"
#include "util/ps4_backoff.hpp"
#include "util/ps4_cool_mode.hpp"
#include "util/ps4_hr_wait.hpp"
#include "util/ps4_rsx_programs.hpp"
#include "util/ps4_vram_policy.hpp"
#include "util/sysinfo.hpp"
#include <sys/resource.h>

void ps4_fatal_note(const char* prefix, const char* detail) noexcept;
#endif

#include <span>
#include <thread>
#include <unordered_set>

class GSRender;

#define CMD_DEBUG 0

atomic_t<bool> g_user_asked_for_recording = false;
atomic_t<bool> g_user_asked_for_screenshot = false;
atomic_t<bool> g_user_asked_for_frame_capture = false;
atomic_t<bool> g_disable_frame_limit = false;
rsx::frame_trace_data frame_debug;
rsx::frame_capture_data frame_capture;

extern CellGcmOffsetTable offsetTable;
extern thread_local std::string(*g_tls_log_prefix)();
extern atomic_t<u32> g_lv2_preempts_taken;

LOG_CHANNEL(perf_log, "PERF");

template <>
bool serialize<rsx::rsx_state>(utils::serial& ar, rsx::rsx_state& o)
{
	ar(o.transform_program);

	// Work around for old RSX captures.
	// RSX capture and savestates both call this method.
	// We do not want to grab transform constants if it is not savestate capture.
	const bool is_savestate_capture = thread_ctrl::get_current() && thread_ctrl::get_name() == "Emu State Capture Thread";
	if (GET_SERIALIZATION_VERSION(global_version) || is_savestate_capture)
	{
		ar(o.transform_constants);
	}

	return ar(o.registers);
}

template <>
bool serialize<rsx::frame_capture_data>(utils::serial& ar, rsx::frame_capture_data& o)
{
	ar(o.magic, o.version, o.LE_format);

	if (o.magic != rsx::c_fc_magic || o.version != rsx::c_fc_version || o.LE_format != u32{std::endian::little == std::endian::native})
	{
		return false;
	}

	return ar(o.tile_map, o.memory_map, o.memory_data_map, o.display_buffers_map, o.replay_commands, o.reg_state);
}

template <>
bool serialize<rsx::frame_capture_data::memory_block_data>(utils::serial& ar, rsx::frame_capture_data::memory_block_data& o)
{
	return ar(o.data);
}

template <>
bool serialize<rsx::frame_capture_data::replay_command>(utils::serial& ar, rsx::frame_capture_data::replay_command& o)
{
	return ar(o.rsx_command, o.memory_state, o.tile_state, o.display_buffer_state);
}

template <>
bool serialize<rsx::rsx_iomap_table>(utils::serial& ar, rsx::rsx_iomap_table& o)
{
	// We do not need more than that
	ar(std::span(o.ea.data(), 512));

	if (!ar.is_writing())
	{
		// Populate o.io
		for (const atomic_t<u32>& ea_addr : o.ea)
		{
			const u32& addr = ea_addr.raw();

			if (addr != umax)
			{
				o.io[addr >> 20].raw() = static_cast<u32>(&ea_addr - o.ea.data()) << 20;
			}
		}
	}

	return true;
}

namespace rsx
{
	std::function<bool(u32 addr, bool is_writing)> g_access_violation_handler;

#ifdef RPCS3_PS4
	// Orbis priorities are strict (FIFO class). 0.26: the RSX thread is raised one step above the PPU/SPU
	// threads again by default (0.24 behaviour; at the base priority the 0.25 RSX stopped on one FIFO
	// command for minutes while guest threads kept the cores). /data/rpcs3/rsx-normal-priority.flag keeps it
	// at the base priority. In the elevated mode RSX waits sleep instead of spinning/yielding.
	static atomic_t<bool> s_ps4_rsx_priority_elevated{false};

	bool ps4_rsx_priority_elevated() noexcept
	{
		return s_ps4_rsx_priority_elevated.observe();
	}

	// 0.26: native pthread_t of the RSX thread (as u64) for the STALL dump's host stack sample.
	static atomic_t<u64> s_ps4_rsx_native_thread{0};

	u64 ps4_rsx_native_thread() noexcept
	{
		return s_ps4_rsx_native_thread.load();
	}

	// 0.29: RSX telemetry and the two A/B knobs read at RSX start.
	ps4_rsx_counters_t g_ps4_rsx_counters{};
	u32 ps4_frame_skips = 0;
	alignas(64) atomic_t<u32> g_ps4_rsx_last_method{0};
	atomic_t<u32> g_ps4_rsx_last_arg{0};

	static thread_local bool t_ps4_is_rsx_thread = false;
	alignas(64) static atomic_t<u32> s_ps4_rsx_wait_bits{0};
	static atomic_t<u64> s_ps4_rsx_wait_since{0};
	static atomic_t<u64> s_ps4_rsx_heartbeat{0}; // get_system_time() at the FIFO loop top (every 64 iterations)
	static atomic_t<u64> s_ps4_rsx_loops{0};
	static atomic_t<u32> s_ps4_rsx_idle_poll_us{200};
	static atomic_t<bool> s_ps4_rsx_sema_yield{true};
	static atomic_t<u32> s_ps4_rsx_sema_sleep_us{1000};
	static atomic_t<bool> s_ps4_rsx_profiler{true}; // false with /data/rpcs3/rsx-profiler-off.flag
	static atomic_t<u64> s_ps4_rsx_flush_services{0};
	static thread_local u64 t_ps4_rsx_wait_tsc = 0;  // TSC at the start of the outermost wait

	// 0.34: NV406E waits over 1 s (all of them, and the ones written to startup.log).
	static atomic_t<u64> s_ps4_sema_long_waits{0};
	static atomic_t<u32> s_ps4_sema_notes{0};
	static constexpr u32 ps4_sema_notes_max = 32;

	ps4_rsx_phase_t g_ps4_rsx_phase{};

	// 0.34: label ring (see ps4_label_ring_note). Fields are separate atomics; the count is published last.
	// 0.37: touched = 1 when the write broke the line's reservations (ps4_label_ring_touched).
	struct ps4_label_entry
	{
		atomic_t<u64> tsc{0};
		atomic_t<u32> op{0}, addr{0}, value{0}, get{0};
		atomic_t<u32> touched{0}, waiters{0}, woke{0};
	};

	static std::array<ps4_label_entry, 32> s_ps4_label_ring{};
	static atomic_t<u64> s_ps4_label_count{0};

	// Report ring (see ps4_report_ring_note). flags: 1 = queued, 2 = the queue front was not flushed yet,
	// 4 = the write broke the line's reservations (0.37, waiters/woke valid).
	struct ps4_report_entry
	{
		atomic_t<u64> tsc{0}, retired{0}; // retired: TSC when the queued report reached memory (0 = not yet)
		atomic_t<u32> op{0}, type{0}, sink{0}, get{0}, depth{0}, flags{0};
		atomic_t<u32> waiters{0}, woke{0};
	};

	static std::array<ps4_report_entry, 32> s_ps4_report_ring{};
	static atomic_t<u64> s_ps4_report_count{0};

	// Last ZCULL queue summary (ps4_zcull_publish). flags: 1 front query, 2 queries recording, 4/8 local/main reports
	// read by the CPU.
	static struct
	{
		atomic_t<u64> tsc{0}, front_sync_tag{0}, flushed_sync_tag{0};
		atomic_t<u32> pending{0}, claimed{0}, front_sink{0}, front_type{0}, front_draws{0}, active_draws{0}, flags{0};
		atomic_t<s32> critical{0};
	} s_ps4_zcull_snapshot;

	// Deferred ZCULL reports written while the RSX waits; false with /data/rpcs3/rsx-no-report-drain.flag (read at
	// RSX start). The *_noted flags limit each first-of-session checkpoint to one line.
	static atomic_t<bool> s_ps4_report_drain{true};
	static atomic_t<bool> s_ps4_report_hint_noted{false}, s_ps4_report_forced_noted{false};
	static atomic_t<bool> s_ps4_report_idle_noted{false}, s_ps4_report_idle_forced_noted{false};

	// Label accesses of the RSX thread through the sudo view; false with /data/rpcs3/rsx-label-direct.flag (read at RSX start).
	static atomic_t<bool> s_ps4_label_sudo{true};

	// A dead FIFO requests the game stop instead of a thread fatal; true with /data/rpcs3/dead-fifo-stop.flag (read at
	// RSX start).
	static atomic_t<bool> s_ps4_dead_fifo_stop{false};

	bool ps4_is_rsx_thread() noexcept
	{
		return t_ps4_is_rsx_thread;
	}

	bool ps4_rsx_label_sudo() noexcept
	{
		return s_ps4_label_sudo.observe();
	}

	u32 ps4_rsx_idle_poll_us() noexcept
	{
		return s_ps4_rsx_idle_poll_us.observe();
	}

	bool ps4_rsx_sema_yield() noexcept
	{
		return s_ps4_rsx_sema_yield.observe();
	}

	u32 ps4_rsx_sema_sleep_us() noexcept
	{
		const u32 value = s_ps4_rsx_sema_sleep_us.observe();
		// Cool mode: sleep from the 200 us step-down point on.
		return value > 200 && utils::ps4::cool_mode::enabled() ? 200 : value;
	}

	u64 ps4_rsx_flush_services() noexcept
	{
		return s_ps4_rsx_flush_services.observe();
	}

	void ps4_note_surface_tag_miss() noexcept
	{
		g_ps4_rsx_counters.surface_tag_misses++;
	}

	u32 ps4_rsx_wait_enter(ps4_rsx_wait_reason reason) noexcept
	{
		if (!t_ps4_is_rsx_thread)
		{
			return umax;
		}

		// Single writer (the RSX thread): plain release stores, no locked instruction on the wait paths.
		const u32 previous = s_ps4_rsx_wait_bits.observe();

		if (!previous)
		{
			// Age of the outermost wait: that is what a STALL needs.
			s_ps4_rsx_wait_since.release(get_system_time());
			t_ps4_rsx_wait_tsc = utils::get_tsc();
		}

		if (reason == ps4_wait_flush_request)
		{
			s_ps4_rsx_flush_services.release(s_ps4_rsx_flush_services.observe() + 1);
		}

		s_ps4_rsx_wait_bits.release(previous | (1u << reason));
		return previous;
	}

	void ps4_rsx_wait_leave(u32 previous) noexcept
	{
		if (previous != umax)
		{
			if (!previous)
			{
				// Outermost wait ends: phase timers subtract this from their own span.
				ps4_rsx_phase_add(g_ps4_rsx_phase.wait_ticks, utils::get_tsc() - t_ps4_rsx_wait_tsc);
			}

			s_ps4_rsx_wait_bits.release(previous);
		}
	}

	void ps4_label_ring_note(ps4_label_op op, u32 addr, u32 value, u32 get) noexcept
	{
		const u64 n = s_ps4_label_count.observe();
		auto& e = s_ps4_label_ring[n % s_ps4_label_ring.size()];
		e.tsc.release(utils::get_tsc());
		e.op.release(op);
		e.addr.release(addr);
		e.value.release(value);
		e.get.release(get);
		e.touched.release(0);
		s_ps4_label_count.release(n + 1);
	}

	void ps4_label_ring_touched(u32 addr, const ps4_touch_result& result) noexcept
	{
		// The release was noted just before its write (write_gcm_label): the newest entry
		const u64 count = s_ps4_label_count.observe();

		if (!count)
		{
			return;
		}

		auto& e = s_ps4_label_ring[(count - 1) % s_ps4_label_ring.size()];

		if (e.op.observe() == ps4_label_release && e.addr.observe() == addr && !e.touched.observe())
		{
			e.waiters.release(result.waiters);
			e.woke.release(result.woke);
			e.touched.release(1);
		}
	}

	std::string ps4_label_ring_text()
	{
		static constexpr std::array<const char*, ps4_label_op_count> names
		{
			"acquire", "wait", "acquired", "TIMEOUT", "release"
		};

		const u64 count = s_ps4_label_count.load();
		const u64 now = utils::get_tsc();
		const double ticks_per_ms = static_cast<double>(utils::get_tsc_freq()) / 1000.;
		const u64 shown = std::min<u64>(count, s_ps4_label_ring.size());

		std::string text = fmt::format("RSX labels: %llu operation(s) recorded, last %llu newest first", count, shown);

		for (u64 i = 0; i < shown; i++)
		{
			const auto& e = s_ps4_label_ring[(count - 1 - i) % s_ps4_label_ring.size()];
			const u32 op = e.op.load();
			const u32 addr = e.addr.load();
			const u64 tsc = e.tsc.load();

			// Read through the unprotected view: a ZCULL report lock on the 16 KiB label page (protection::no) would
			// fault on this thread, and the STALL dump runs on the Main Thread, which has no fault handler.
			std::string current = "unmapped";
			if (vm::check_addr(addr, vm::page_readable, 4))
			{
				current = fmt::format("0x%x", static_cast<u32>(*vm::get_super_ptr<RsxSemaphore>(addr)));
			}

			fmt::append(text, "\n  %s 0x%08x value=0x%x now=%s get=0x%x %s", op < names.size() ? names[op] : "?", addr, e.value.load(),
				current, e.get.load(), ticks_per_ms > 0 && now >= tsc ? fmt::format("%.1f ms ago", static_cast<double>(now - tsc) / ticks_per_ms) : std::string("age n/a"));

			if (op == ps4_label_release && e.touched.load())
			{
				fmt::append(text, "; waiters=%u woke=%u", e.waiters.load(), e.woke.load());
			}
		}

		// Labels written by SPU or PPU code never show in the ring above (God of War Ascension's MLAA labels 0x51/0x52).
		if (const auto rsx = get_current_renderer(); rsx && rsx->label_addr)
		{
			fmt::append(text, "\nRSX labels 0x48-0x5f now (0x%08x-0x%08x):", rsx->label_addr + 0x48 * 0x10, rsx->label_addr + 0x5f * 0x10);

			for (u32 index = 0x48; index < 0x60; index++)
			{
				const u32 addr = rsx->label_addr + index * 0x10;

				if (vm::check_addr(addr, vm::page_readable, 4))
				{
					fmt::append(text, " %02x=0x%x", index, static_cast<u32>(*vm::get_super_ptr<RsxSemaphore>(addr)));
				}
				else
				{
					fmt::append(text, " %02x=unmapped", index);
				}
			}
		}

		text += '\n';
		text += ps4_report_ring_text();
		return text;
	}

	std::string ps4_fifo_state_text()
	{
		const auto rsx = get_current_renderer();

		if (!rsx)
		{
			return "no RSX context; " + ps4_sys_rsx_ring_text();
		}

		const u32 get = rsx->fifo_ctrl ? rsx->fifo_ctrl->get_pos() : 0;
		const u32 put = rsx->ctrl ? static_cast<u32>(rsx->ctrl->put.load()) : 0;
		const u32 get_ea = rsx->iomap_table.get_addr(get);

		u32 mapped = 0;
		std::string first;

		for (u32 i = 0; i < rsx->iomap_table.ea.size(); i++)
		{
			if (const u32 ea = rsx->iomap_table.ea[i]; ea != umax && mapped++ < 4)
			{
				fmt::append(first, " io 0x%x -> ea 0x%x;", i << 20, ea);
			}
		}

		// The system mode the guest passed to sys_rsx_context_allocate, as stored in the driver info
		std::string mode = "n/a";
		if (rsx->driver_info && vm::check_addr(rsx->driver_info, vm::page_readable, 0x58))
		{
			mode = fmt::format("0x%x", static_cast<u32>(vm::get_super_ptr<RsxDriverInfo>(rsx->driver_info)->systemModeFlags));
		}

		return fmt::format("get=0x%x put=0x%x (IO 0x%x %s), last cmd 0x%x; IO map: %u MiB mapped, IO 0 %s;%s system mode %s; %s",
			get, put, get, get_ea == umax ? "not mapped" : fmt::format("-> ea 0x%x", get_ea), rsx->fifo_ctrl ? rsx->fifo_ctrl->last_cmd() : 0,
			mapped, rsx->iomap_table.get_addr(0) == umax ? "not mapped" : "mapped", first, mode, ps4_sys_rsx_ring_text());
	}

	void ps4_zcull_publish(const reports::ps4_zcull_state& st) noexcept
	{
		auto& s = s_ps4_zcull_snapshot;
		s.pending.release(st.pending);
		s.claimed.release(st.claimed);
		s.front_sink.release(st.front_sink);
		s.front_type.release(st.front_type);
		s.front_draws.release(st.front_draws);
		s.front_sync_tag.release(st.front_sync_tag);
		s.flushed_sync_tag.release(st.flushed_sync_tag);
		s.active_draws.release(st.active_draws);
		s.critical.release(st.critical);
		s.flags.release((st.front_query ? 1u : 0u) | (st.host_queries ? 2u : 0u) | (st.pages_accessed[0] ? 4u : 0u) | (st.pages_accessed[1] ? 8u : 0u));
		s.tsc.release(utils::get_tsc());
	}

	void ps4_report_ring_note(ps4_report_op op, u32 type, u32 sink, u32 get, bool queued, const reports::ps4_zcull_state& st) noexcept
	{
		const u64 n = s_ps4_report_count.observe();
		auto& e = s_ps4_report_ring[n % s_ps4_report_ring.size()];
		e.tsc.release(utils::get_tsc());
		e.retired.release(0);
		e.op.release(op);
		e.type.release(type);
		e.sink.release(sink);
		e.get.release(get);
		e.depth.release(st.pending);
		e.flags.release((queued ? 1u : 0u) | (st.front_unflushed() ? 2u : 0u));
		s_ps4_report_count.release(n + 1);

		ps4_zcull_publish(st);
	}

	void ps4_report_ring_retired(u32 sink) noexcept
	{
		const u64 count = s_ps4_report_count.observe();
		const u64 shown = std::min<u64>(count, s_ps4_report_ring.size());

		// Oldest first: queued reports reach memory in issue order.
		for (u64 i = count - shown; i < count; i++)
		{
			auto& e = s_ps4_report_ring[i % s_ps4_report_ring.size()];

			if (e.sink.observe() == sink && (e.flags.observe() & 1u) && !e.retired.observe() && e.op.observe() == ps4_report_get)
			{
				e.retired.release(utils::get_tsc());
				return;
			}
		}
	}

	void ps4_report_ring_touched(u32 sink, const ps4_touch_result& result) noexcept
	{
		const u64 count = s_ps4_report_count.observe();
		const u64 shown = std::min<u64>(count, s_ps4_report_ring.size());

		// Oldest first, like ps4_report_ring_retired: an immediate report was noted just before its write, a queued
		// one is the oldest entry of that sink not retired yet. Report copies to other addresses match no entry.
		for (u64 i = count - shown; i < count; i++)
		{
			auto& e = s_ps4_report_ring[i % s_ps4_report_ring.size()];
			const u32 flags = e.flags.observe();

			if (e.sink.observe() == sink && e.op.observe() == ps4_report_get && !(flags & 4u) && (!(flags & 1u) || !e.retired.observe()))
			{
				e.waiters.release(result.waiters);
				e.woke.release(result.woke);
				e.flags.release(flags | 4u);
				return;
			}
		}
	}

	// Newest kick per readback state (RSX thread only): at most one line per second each, so a kick during an SPU
	// readback is never hidden behind an ordinary one logged in the same second.
	static void ps4_note_kick(u32 addr, u32 waiters) noexcept
	{
		static thread_local u64 s_last_us[2]{};
		static atomic_t<u32> s_noted{0};

		u32 spu_id = 0;
		u32 elapsed_ms = 0;
		const bool readback = ps4_spu_readback_in_flight(spu_id, elapsed_ms);

		const u64 now = get_system_time();
		u64& last = s_last_us[readback ? 1 : 0];

		if (last && now - last < 1'000'000)
		{
			return;
		}

		last = now;

		const std::string line = fmt::format("RSX kick 0x%x: waiters=%u, SPU readback in flight=%d (SPU%u since %u ms)", addr, waiters, readback ? 1 : 0, spu_id, elapsed_ms);
		rsx_log.warning("PS4 %s", line);

		// startup.log: the first 32 of the process
		if (s_noted.fetch_add(1) < 32)
		{
			ps4_startup_checkpoint(line.c_str());
		}
	}

	ps4_touch_result ps4_rsx_touch_reservations(u32 addr, u32 size, bool kick) noexcept
	{
		ps4_touch_result result{};

		// Only main memory carries SPU/PPU reservations; g_reservations is indexed by addr & 0xff80, so touching RSX
		// local memory would only break unrelated aliasing lines. Transfers above 64 MiB (malformed pitches) are
		// not scanned.
		if (!size || size > 0x4000000 || addr >= constants::local_mem_base)
		{
			return result;
		}

		size = std::min<u32>(size, constants::local_mem_base - addr);

		if (kick)
		{
			// The sleepers are counted before the touch wakes them
			for (u32 line = addr & -128; line < addr + size; line += 128)
			{
				const u64 base = vm::reservation_acquire(line) & -128;
				result.waiters += vm::reservation_notifier_count(line, base) + vm::reservation_notifier_count(line, base - 128);
			}
		}

		const u64 woke = vm::g_ps4_rsx_touch_woke.load();
		vm::ps4_touch_reservations(addr, size);
		result.woke = static_cast<u32>(vm::g_ps4_rsx_touch_woke.load() - woke);

		if (kick && result.waiters)
		{
			ps4_note_kick(addr, result.waiters);
		}

		return result;
	}

	std::string ps4_report_ring_text()
	{
		static constexpr std::array<const char*, ps4_report_op_count> names
		{
			"GET_REPORT", "CLEAR_REPORT"
		};

		const u64 count = s_ps4_report_count.load();
		const u64 now = utils::get_tsc();
		const double ticks_per_ms = static_cast<double>(utils::get_tsc_freq()) / 1000.;
		const u64 shown = std::min<u64>(count, s_ps4_report_ring.size());

		const auto age = [&](u64 tsc)
		{
			return ticks_per_ms > 0 && now >= tsc ? fmt::format("%.1f ms ago", static_cast<double>(now - tsc) / ticks_per_ms) : std::string("age n/a");
		};

		std::string text = fmt::format("RSX reports: %llu operation(s) recorded, last %llu newest first", count, shown);

		const auto& s = s_ps4_zcull_snapshot;
		if (const u64 tsc = s.tsc.load())
		{
			reports::ps4_zcull_state st{};
			st.pending = s.pending.load();
			st.claimed = s.claimed.load();
			st.front_sink = s.front_sink.load();
			st.front_type = s.front_type.load();
			st.front_draws = s.front_draws.load();
			st.front_sync_tag = s.front_sync_tag.load();
			st.flushed_sync_tag = s.flushed_sync_tag.load();
			st.active_draws = s.active_draws.load();
			st.critical = s.critical.load();
			const u32 flags = s.flags.load();
			st.front_query = !!(flags & 1u);
			st.host_queries = !!(flags & 2u);
			st.pages_accessed[0] = !!(flags & 4u);
			st.pages_accessed[1] = !!(flags & 8u);
			fmt::append(text, "\n  %s (as of %s)", st.to_string(), age(tsc));
		}
		else
		{
			text += "\n  ZCULL queue: no report activity yet";
		}

		for (u64 i = 0; i < shown; i++)
		{
			const auto& e = s_ps4_report_ring[(count - 1 - i) % s_ps4_report_ring.size()];
			const u32 op = e.op.load();
			const u32 flags = e.flags.load();
			const u64 tsc = e.tsc.load();

			if (op == ps4_report_get)
			{
				fmt::append(text, "\n  %s type=%u sink=0x%08x get=0x%x %s %s", names[op], e.type.load(), e.sink.load(), e.get.load(),
					(flags & 1u) ? fmt::format("queued (pending %u, %s)", e.depth.load(), (flags & 2u) ? "front not flushed" : "front flushed") : std::string("immediate"),
					age(tsc));

				if (flags & 1u)
				{
					const u64 retired = e.retired.load();
					fmt::append(text, "; %s", retired && retired >= tsc && ticks_per_ms > 0 ?
						fmt::format("retired after %.1f ms", static_cast<double>(retired - tsc) / ticks_per_ms) : std::string("not retired"));
				}

				if (flags & 4u)
				{
					fmt::append(text, "; waiters=%u woke=%u", e.waiters.load(), e.woke.load());
				}
			}
			else
			{
				fmt::append(text, "\n  %s type=%u get=0x%x (pending %u after it) %s", op < names.size() ? names[op] : "?", e.type.load(), e.get.load(),
					e.depth.load(), age(tsc));
			}
		}

		return text;
	}

	void ps4_note_semaphore(u32 addr, u32 expected, u32 value, u32 get, u64 waited_us, u32 spu_compiles, u32 compile_slots, bool timed_out) noexcept
	{
		s_ps4_sema_long_waits++;

		const u32 n = s_ps4_sema_notes.fetch_add(1);
		if (n >= ps4_sema_notes_max)
		{
			return;
		}

		std::string line = fmt::format("RSX: semaphore 0x%x waited %llu ms (expected 0x%x, value 0x%x, get 0x%x, SPU compiles %u, compile slots %u): %s",
			addr, waited_us / 1000, expected, value, get, spu_compiles, compile_slots, timed_out ? "TIMED OUT" : "released");

		if (n + 1 == ps4_sema_notes_max)
		{
			line += "; later waits over 1 s are only counted (STALL RSX state line)";
		}

		ps4_startup_checkpoint(line.c_str());

		if (timed_out)
		{
			ps4_startup_checkpoint(ps4_label_ring_text().c_str());
		}
	}

	std::string ps4_rsx_wait_state() noexcept
	{
		// Reads only atomics: safe from the frontend thread while the RSX runs (or is stuck).
		static constexpr std::array<const char*, ps4_wait_reason_count> names
		{
			"fifo_idle", "semaphore", "flip", "frame_limit", "zcull", "cpu_wait", "pause", "vk_fence", "vk_event", "flush_request"
		};

		const u64 now = get_system_time();
		const u32 bits = s_ps4_rsx_wait_bits.load();
		const u64 since = s_ps4_rsx_wait_since.load();
		const u64 heartbeat = s_ps4_rsx_heartbeat.load();

		std::string waits;
		for (u32 i = 0; i < ps4_wait_reason_count; i++)
		{
			if (bits & (1u << i))
			{
				if (!waits.empty()) waits += '+';
				waits += names[i];
			}
		}

		std::string line = fmt::format("STALL RSX state: heartbeat %s (loop %llu); last method=0x%04x arg=0x%08x; ",
			heartbeat ? fmt::format("%llu ms ago", now > heartbeat ? (now - heartbeat) / 1000 : 0) : std::string("never"),
			s_ps4_rsx_loops.load(), g_ps4_rsx_last_method.load(), g_ps4_rsx_last_arg.load());

		if (waits.empty())
		{
			line += "wait=none (executing or between waits)";
		}
		else
		{
			fmt::append(line, "wait=%s for %llu ms", waits, now > since ? (now - since) / 1000 : 0);
		}

		fmt::append(line, "; priority elevated=%u, sema-yield=%u, idle poll=%u us",
			ps4_rsx_priority_elevated(), s_ps4_rsx_sema_yield.load(), s_ps4_rsx_idle_poll_us.load());
		fmt::append(line, "; semaphore sleep after %u us; waits over 1 s=%llu (%u logged), timeouts=%llu",
			ps4_rsx_sema_sleep_us(), s_ps4_sema_long_waits.load(), std::min(s_ps4_sema_notes.load(), ps4_sema_notes_max),
			g_ps4_rsx_counters.sema_timeouts.load());
		return line;
	}

	// An NV406E acquire stalls only the RSX puller: the 3D back end still writes every report issued before it.
	// RPCS3 defers ZPASS reports until the RSX loop polls them, which never happens while the RSX blocks here, so a
	// label written by SPU work that a report kicks (God of War Ascension: a report into the SPURS workload flag
	// starts the MLAA job that writes the awaited label) is a circular wait. The first service submits the newest
	// claimed query's command buffer (one submit, no GPU wait); polls follow; a forced drain (GPU wait) only in a
	// wait that already lasted 50 ms, so short per-frame waits never hard-sync.
	void thread::ps4_service_reports_while_blocked(u32 sema_addr, u64 waited_us, bool& hinted, bool& forced)
	{
		// forced: nothing more to do in this wait
		if (forced || !zcull_ctrl->has_pending())
		{
			return;
		}

		if (!hinted)
		{
			hinted = true;

			// No report is claimed while the FIFO is blocked here, so this first look decides the whole wait.
			const auto st = zcull_ctrl->ps4_state();
			ps4_zcull_publish(st);

			if (!st.claimed || in_begin_end || !s_ps4_report_drain.observe())
			{
				forced = true;
				return;
			}

			if (zcull_ctrl->ps4_hint(this))
			{
				g_ps4_rsx_counters.report_hint_submits++;
			}

			zcull_ctrl->update(this);
			g_ps4_rsx_counters.report_hints++;

			if (!s_ps4_report_hint_noted.exchange(true))
			{
				ps4_startup_checkpoint(fmt::format("RSX: semaphore 0x%x waited %llu us with %u deferred ZCULL report(s) pending: flushed and polled while waiting "
					"(first of the session; rsx-no-report-drain.flag disables)", sema_addr, waited_us, st.claimed).c_str());
			}

			return;
		}

		if (waited_us >= 50'000)
		{
			forced = true;

			const auto st = zcull_ctrl->ps4_state();
			if (!st.claimed)
			{
				return;
			}

			const u32 retired = zcull_ctrl->ps4_drain(this);
			g_ps4_rsx_counters.report_forced++;
			g_ps4_rsx_counters.report_forced_written += retired;

			if (!s_ps4_report_forced_noted.exchange(true))
			{
				ps4_startup_checkpoint(fmt::format("RSX: semaphore 0x%x still waiting after %llu ms with %u deferred ZCULL report(s) pending: forced drain retired %u "
					"(first of the session); before: %s", sema_addr, waited_us / 1000, st.claimed, retired, st.to_string()).c_str());
			}

			return;
		}

		zcull_ctrl->update(this);
	}

	// Real hardware drains the back end whenever the puller idles. Here nothing submits the open command buffer while
	// the FIFO idles, so a report whose query is recorded in it is never written, and the RSX loop's own polling runs
	// every 64 iterations, ~64 ms apart once each idle iteration sleeps a kernel tick. A jump-to-self whose patcher
	// waits for SPU work kicked by such a report would wait for good. Called from the FIFO idle sleep path.
	void thread::ps4_service_reports_at_idle(u64 idle_us)
	{
		// 0.37: poll the sub-units before every idle sleep (non-blocking), as the PC loop does at its yield cadence;
		// the 2 ms threshold below only gates the submit and the forced drain.
		zcull_ctrl->update(this);

		if (m_host_dma_ctrl)
		{
			m_host_dma_ctrl->update();
		}

		if (idle_us < 2000 || !zcull_ctrl->has_pending())
		{
			return;
		}

		// One episode per FIFO idle start; only the RSX thread calls this.
		static thread_local u64 s_episode = 0;
		static thread_local bool s_hinted = false, s_done = false;

		if (const u64 episode = performance_counters.FIFO_idle_timestamp; episode != s_episode)
		{
			s_episode = episode;
			s_hinted = false;
			s_done = false;
		}

		if (s_done)
		{
			return;
		}

		if (!s_hinted)
		{
			s_hinted = true;

			// Nothing is claimed while the FIFO idles, so this first look decides the episode.
			const auto st = zcull_ctrl->ps4_state();
			ps4_zcull_publish(st);

			if (!st.claimed || in_begin_end || external_interrupt_lock || !s_ps4_report_drain.observe())
			{
				s_done = true;
				return;
			}

			if (zcull_ctrl->ps4_hint(this))
			{
				g_ps4_rsx_counters.report_hint_submits++;
			}

			zcull_ctrl->update(this);
			g_ps4_rsx_counters.report_idle_hints++;

			if (!s_ps4_report_idle_noted.exchange(true))
			{
				ps4_startup_checkpoint(fmt::format("RSX: FIFO idle for %llu us (get=0x%x) with %u deferred ZCULL report(s) pending: flushed and polled while idle "
					"(first of the session; rsx-no-report-drain.flag disables)", idle_us, fifo_ctrl ? fifo_ctrl->get_pos() : 0, st.claimed).c_str());
			}

			return;
		}

		if (idle_us >= 50'000)
		{
			s_done = true;

			const auto st = zcull_ctrl->ps4_state();
			if (!st.claimed || in_begin_end || external_interrupt_lock)
			{
				return;
			}

			const u32 retired = zcull_ctrl->ps4_drain(this);
			g_ps4_rsx_counters.report_idle_forced++;

			if (!s_ps4_report_idle_forced_noted.exchange(true))
			{
				ps4_startup_checkpoint(fmt::format("RSX: FIFO idle for %llu ms (get=0x%x) with %u deferred ZCULL report(s) pending: forced drain retired %u "
					"(first of the session); before: %s", idle_us / 1000, fifo_ctrl ? fifo_ctrl->get_pos() : 0, st.claimed, retired, st.to_string()).c_str());
			}

			return;
		}
	}

	std::string thread::ps4_label_zcull_text(u32 label_addr)
	{
		if (!zcull_ctrl)
		{
			return "label page ZCULL-locked n/a (no ZCULL unit)";
		}

		const auto st = zcull_ctrl->ps4_state();
		ps4_zcull_publish(st);
		return fmt::format("label page ZCULL-locked %s; %s", zcull_ctrl->ps4_page_locked(label_addr) ? "yes" : "no", st.to_string());
	}
#endif

	// TODO: Proper context manager
	static rsx::context s_ctx{ .rsxthr = nullptr, .register_state = &method_registers };

	constexpr u32 fs_export_config_mask =
		RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE |
		RSX_SHADER_CONTROL_ROP_MULTISAMPLED |
		RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING;

	rsx_iomap_table::rsx_iomap_table() noexcept
		: ea(fill_array(-1))
		, io(fill_array(-1))
	{
	}

	u32 get_address(u32 offset, u32 location, u32 size_to_check, std::source_location src_loc)
	{
		const auto render = get_current_renderer();
		std::string_view msg;

		switch (location)
		{
		case CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER:
		case CELL_GCM_LOCATION_LOCAL:
		{
			if (offset < render->local_mem_size && render->local_mem_size - offset >= size_to_check)
			{
				return rsx::constants::local_mem_base + offset;
			}

			msg = "Local RSX offset out of range!"sv;
			break;
		}

		case CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER:
		case CELL_GCM_LOCATION_MAIN:
		{
			if (const u32 ea = render->iomap_table.get_addr(offset); ea != umax)
			{
				if (size_to_check <= 1 || (offset < render->main_mem_size && render->main_mem_size - offset >= size_to_check))
				{
					bool ok = true;

					for (u32 offs_index = 0x100000; offs_index < size_to_check + (offset & 0xfffff); offs_index += 0x100000)
					{
						// This check does not check continuity but rather that it's mapped at all
						if (render->iomap_table.get_addr(offset + offs_index) == umax)
						{
							ok = false;
						}
					}

					if (ok)
					{
						if (!size_to_check || vm::check_addr(ea, 0, size_to_check))
						{
							return ea;
						}
					}
				}
			}

			msg = "RSXIO memory not mapped!"sv;
			break;
		}

		case CELL_GCM_CONTEXT_DMA_REPORT_LOCATION_LOCAL:
		{
			if (offset < sizeof(RsxReports::report) /*&& (offset % 0x10) == 0*/)
			{
				return render->label_addr + ::offset32(&RsxReports::report) + offset;
			}

			msg = "Local RSX REPORT offset out of range!"sv;
			break;
		}

		case CELL_GCM_CONTEXT_DMA_REPORT_LOCATION_MAIN:
		{
			if (const u32 ea = offset < 0x1000000 ? render->iomap_table.get_addr(0x0e000000 + offset) : -1; ea != umax)
			{
				if (!size_to_check || vm::check_addr(ea, 0, size_to_check))
				{
					return ea;
				}
			}

			msg = "RSXIO REPORT memory not mapped!"sv;
			break;
		}

		// They are handled elsewhere for targeted methods, so it's unexpected for them to be passed here
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY0:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY1:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY2:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY3:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY4:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY5:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY6:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY7:
			msg = "CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFYx"sv; break;

		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_0:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_1:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_2:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_3:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_4:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_5:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_6:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_7:
			msg = "CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_x"sv; break;

		case CELL_GCM_CONTEXT_DMA_SEMAPHORE_RW:
		case CELL_GCM_CONTEXT_DMA_SEMAPHORE_R:
		{
			if (offset < sizeof(RsxReports::semaphore) /*&& (offset % 0x10) == 0*/)
			{
				return render->label_addr + offset;
			}

			msg = "DMA SEMAPHORE offset out of range!"sv;
			break;
		}

		case CELL_GCM_CONTEXT_DMA_DEVICE_RW:
		case CELL_GCM_CONTEXT_DMA_DEVICE_R:
		{
			if (offset < 0x100000 /*&& (offset % 0x10) == 0*/)
			{
				return render->device_addr + offset;
			}

			// TODO: What happens here? It could wrap around or access other segments of rsx internal memory etc
			// Or can simply throw access violation error
			msg = "DMA DEVICE offset out of range!"sv;
			break;
		}

		default:
		{
			msg = "Invalid location!"sv;
			break;
		}
		}

		if (size_to_check)
		{
			// Allow failure if specified size
			// This is to allow accurate recovery for failures
			rsx_log.warning("rsx::get_address(offset=0x%x, location=0x%x, size=0x%x): %s%s", offset, location, size_to_check, msg, src_loc);
			return 0;
		}

		fmt::throw_exception("rsx::get_address(offset=0x%x, location=0x%x): %s%s", offset, location, msg, src_loc);
	}

	extern void set_rsx_yield_flag() noexcept
	{
		if (auto rsx = get_current_renderer())
		{
			if (g_cfg.core.allow_rsx_cpu_preempt)
			{
				rsx->state += cpu_flag::yield;
			}
		}
	}

	extern void set_native_ui_flip()
	{
		if (auto rsxthr = rsx::get_current_renderer())
		{
			rsxthr->async_flip_requested |= rsx::thread::flip_request::native_ui;
		}
	}

	static bool evaluate_programmable_blending_state(rsx::context* ctx, u32& fragment_ctrl)
	{
		const bool is_blend_config_dirty = RSX(ctx)->m_graphics_state.test(rsx::blend_config_dirty);
		RSX(ctx)->m_graphics_state.clear(rsx::blend_config_dirty);

		const auto blend_enable_mask = REGS(ctx)->blend_enabled_mask() & REGS(ctx)->surface_color_target_mask();
		const bool programmable_blend_active = !!(fragment_ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING);
		const bool is_blending_active = !!blend_enable_mask && !REGS(ctx)->logic_op_enabled();

		if (!is_blending_active && !programmable_blend_active)
		{
			return false;
		}

		if (!is_blending_active)
		{
			fragment_ctrl &= ~RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING;
			return true;
		}

		if (g_cfg.video.disable_hardware_blending)
		{
			fragment_ctrl |= RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING;
			return !programmable_blend_active;
		}

		// We actually need to handle this, we're still blending
		const auto is_signed_equation = [](rsx::blend_equation equation)
		{
			switch (equation)
			{
			case rsx::blend_equation::add_signed:
			case rsx::blend_equation::reverse_add_signed:
			case rsx::blend_equation::reverse_subtract_signed:
				return true;
			default:
				return false;
			}
		};

		const auto factor_references_alpha = [](rsx::blend_factor factor)
		{
			switch (factor)
			{
			case rsx::blend_factor::src_alpha:
			case rsx::blend_factor::one_minus_src_alpha:
			case rsx::blend_factor::dst_alpha:
			case rsx::blend_factor::one_minus_dst_alpha:
			case rsx::blend_factor::src_alpha_saturate:
				return true;
			default:
				return false;
			}
		};

		const auto shader_writes_alpha = [&]()
		{
			for (u32 i = 0, mask = blend_enable_mask; !!mask; mask >>= 1, i++)
			{
				if (REGS(ctx)->color_mask_a(i))
				{
					return true;
				}
			}
			return false;
		};

		bool need_programmable_blending = false;
		const auto surface_format = REGS(ctx)->surface_color();

		switch (surface_format)
		{
		case surface_color_format::x1r5g5b5_z1r5g5b5:
		case surface_color_format::x1r5g5b5_o1r5g5b5:
		case surface_color_format::x8r8g8b8_z8r8g8b8:
		case surface_color_format::x8r8g8b8_o8r8g8b8:
		case surface_color_format::x8b8g8r8_z8b8g8r8:
		case surface_color_format::x8b8g8r8_o8b8g8r8:
			// Force PB if alpha is read in the RGB factors or A is written without passthrough.
			if (factor_references_alpha(REGS(ctx)->blend_func_sfactor_rgb()) ||
				factor_references_alpha(REGS(ctx)->blend_func_dfactor_rgb()))
			{
				need_programmable_blending = true;
			}
			else
			{
				need_programmable_blending = shader_writes_alpha() && (
					REGS(ctx)->blend_equation_a() == rsx::blend_equation::reverse_subtract ||
					REGS(ctx)->blend_equation_a() == rsx::blend_equation::min ||
					REGS(ctx)->blend_equation_a() == rsx::blend_equation::max ||
					REGS(ctx)->blend_func_sfactor_a() != rsx::blend_factor::one ||
					REGS(ctx)->blend_func_dfactor_a() != rsx::blend_factor::zero);
			}
			break;
		case surface_color_format::b8:
		case surface_color_format::g8b8:
			// These 2 don't accept custom factors anyway
			[[ fallthrough ]];
		default:
			need_programmable_blending = need_programmable_blending ||
				is_signed_equation(REGS(ctx)->blend_equation_rgb()) ||
				is_signed_equation(REGS(ctx)->blend_equation_a());
			break;
		}

		if (need_programmable_blending && is_blend_config_dirty)
		{
			RSX(ctx)->m_graphics_state.set(rsx::fragment_state_dirty);
		}

		if (need_programmable_blending == programmable_blend_active)
		{
			return false;
		}

		if (!need_programmable_blending)
		{
			fragment_ctrl &= ~RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING;
			return true;
		}

		fragment_ctrl |= RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING;
		return true;
	}

	std::pair<u32, u32> interleaved_range_info::calculate_required_range(u32 first, u32 count)
	{
		if (vertex_range.second)
		{
			// Cached result
			return vertex_range;
		}

		if (single_vertex)
		{
			return { 0, 1 };
		}

		const u32 max_index = (first + count) - 1;
		u32 _max_index = 0;
		u32 _min_index = first;

		u32 frequencies[rsx::limits::vertex_count];
		u32 freq_count = rsx::method_registers.current_draw_clause.command == rsx::draw_command::indexed ? 0 : u32{umax};
		u32 max_result_by_division = 0; // Guaranteed maximum

		for (const auto &attrib : locations)
		{
			if (attrib.frequency <= 1) [[likely]]
			{
				freq_count = umax;
				_max_index = max_index;
			}
			else
			{
				if (attrib.modulo)
				{
					if (max_index >= attrib.frequency)
					{
						// Actually uses the modulo operator
						_min_index = 0;
						_max_index = std::max<u32>(_max_index, attrib.frequency - 1);

						if (max_result_by_division < _max_index)
						{
							if (freq_count != umax)
							{
								if (std::find(frequencies, frequencies + freq_count, attrib.frequency) == frequencies + freq_count)
								{
									frequencies[freq_count++] = attrib.frequency;
								}
							}
						}
					}
					else
					{
						// Same as having no modulo
						_max_index = max_index;
						freq_count = umax;
					}
				}
				else
				{
					// Division operator
					_min_index = std::min(_min_index, first / attrib.frequency);
					_max_index = std::max<u32>(_max_index, utils::aligned_div(max_index, attrib.frequency));

					if (freq_count > 0 && freq_count != umax)
					{
						const u32 max = utils::aligned_div(max_index, attrib.frequency);
						max_result_by_division = std::max<u32>(max_result_by_division, max);

						// Discard lower frequencies because it has been proven that there are indices higher than them
						const usz discard_cnt = frequencies + freq_count - std::remove_if(frequencies, frequencies + freq_count, [&max_result_by_division](u32 freq)
						{
							return freq <= max_result_by_division;
						});

						freq_count -= static_cast<u32>(discard_cnt);
					}
				}
			}
		}

		while (freq_count > 0 && freq_count != umax)
		{
			const rsx::index_array_type index_type = rsx::method_registers.current_draw_clause.is_immediate_draw ?
				rsx::index_array_type::u32 :
				rsx::method_registers.index_type();

			const u32 index_size = index_type == rsx::index_array_type::u32 ? 4 : 2;

			const auto render = rsx::get_current_renderer();

			// If we can access a bit a more memory than required - do it
			// The alternative would be re-iterating again over all of them
			if (get_location(real_offset_address) == CELL_GCM_LOCATION_LOCAL)
			{
				if (utils::add_saturate<u32>(real_offset_address - rsx::constants::local_mem_base, (_max_index + 1) * attribute_stride) <= render->local_mem_size)
				{
					break;
				}
			}
			else if (real_offset_address % 0x100000 + (_max_index + 1) * attribute_stride <= 0x100000)//(vm::check_addr(real_offset_address, vm::page_readable, (_max_index + 1) * attribute_stride))
			{
				break;
			}

			_max_index = 0;

			const auto re_evaluate = [&] <typename T> (const std::byte* ptr, T)
			{
				const u64 restart = rsx::method_registers.restart_index_enabled() ? rsx::method_registers.restart_index() : u64{umax};

				for (u32 _index = first; _index < first + count; _index++)
				{
					const auto value = read_from_ptr_unsafe<be_t<T>>(ptr, _index * sizeof(T));

					if (value == restart)
					{
						continue;
					}

					for (u32 freq_it = 0; freq_it < freq_count; freq_it++)
					{
						const auto res = value % frequencies[freq_it];

						if (res > _max_index)
						{
							_max_index = res;
						}
					}
				}
			};

			const auto element_push_buffer = render->draw_processor()->element_push_buffer();
			if (index_size == 4)
			{
				if (!element_push_buffer.empty()) [[unlikely]]
				{
					// Indices provided via immediate mode
					re_evaluate(reinterpret_cast<const std::byte*>(element_push_buffer.data()), u32{});
				}
				else
				{
					const u32 address = (0 - index_size) & get_address(rsx::method_registers.index_array_address(), rsx::method_registers.index_array_location());
					re_evaluate(vm::get_super_ptr<std::byte>(address), u32{});
				}
			}
			else
			{
				if (!element_push_buffer.empty()) [[unlikely]]
				{
					// Indices provided via immediate mode
					re_evaluate(reinterpret_cast<const std::byte*>(element_push_buffer.data()), u16{});
				}
				else
				{
					const u32 address = (0 - index_size) & get_address(rsx::method_registers.index_array_address(), rsx::method_registers.index_array_location());
					re_evaluate(vm::get_super_ptr<std::byte>(address), u16{});
				}
			}

			break;
		}

		ensure(_max_index >= _min_index);
		vertex_range = { _min_index, (_max_index - _min_index) + 1 };
		return vertex_range;
	}

	u32 get_vertex_type_size_on_host(vertex_base_type type, u32 size)
	{
		switch (type)
		{
		case vertex_base_type::s1:
		case vertex_base_type::s32k:
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return sizeof(u16) * size;
			case 3:
				return sizeof(u16) * 4;
			default:
				break;
			}
			fmt::throw_exception("Wrong vector size");
		case vertex_base_type::f: return sizeof(f32) * size;
		case vertex_base_type::sf:
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return sizeof(f16) * size;
			case 3:
				return sizeof(f16) * 4;
			default:
				break;
			}
			fmt::throw_exception("Wrong vector size");
		case vertex_base_type::ub:
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return sizeof(u8) * size;
			case 3:
				return sizeof(u8) * 4;
			default:
				break;
			}
			fmt::throw_exception("Wrong vector size");
		case vertex_base_type::cmp: return 4;
		case vertex_base_type::ub256: ensure(size == 4); return sizeof(u8) * 4;
		default:
			break;
		}
		fmt::throw_exception("Bad vertex data type (%d)!", static_cast<u8>(type));
	}

	void tiled_region::write(const void *src, u32 width, u32 height, u32 pitch)
	{
		if (!tile)
		{
			memcpy(ptr, src, height * pitch);
			return;
		}

		const u32 offset_x = base % tile->pitch;
		const u32 offset_y = base / tile->pitch;

		switch (tile->comp)
		{
		case CELL_GCM_COMPMODE_C32_2X1:
		case CELL_GCM_COMPMODE_DISABLED:
			for (u32 y = 0; y < height; ++y)
			{
				memcpy(ptr + (offset_y + y) * tile->pitch + offset_x, static_cast<const u8*>(src) + pitch * y, pitch);
			}
			break;
			/*
		case CELL_GCM_COMPMODE_C32_2X1:
			for (u32 y = 0; y < height; ++y)
			{
				const u32* src_line = reinterpret_cast<const u32*>(static_cast<const u8*>(src) + pitch * y);
				u32* dst_line = reinterpret_cast<u32*>(ptr + (offset_y + y) * tile->pitch + offset_x);

				for (u32 x = 0; x < width; ++x)
				{
					u32 value = src_line[x];

					dst_line[x * 2 + 0] = value;
					dst_line[x * 2 + 1] = value;
				}
			}
			break;
			*/
		case CELL_GCM_COMPMODE_C32_2X2:
			for (u32 y = 0; y < height; ++y)
			{
				const u32* src_line = reinterpret_cast<const u32*>(static_cast<const u8*>(src) + pitch * y);
				u32* line_0 = reinterpret_cast<u32*>(ptr + (offset_y + y * 2 + 0) * tile->pitch + offset_x);
				u32* line_1 = reinterpret_cast<u32*>(ptr + (offset_y + y * 2 + 1) * tile->pitch + offset_x);

				for (u32 x = 0; x < width; ++x)
				{
					u32 value = src_line[x];

					line_0[x * 2 + 0] = value;
					line_0[x * 2 + 1] = value;
					line_1[x * 2 + 0] = value;
					line_1[x * 2 + 1] = value;
				}
			}
			break;
		default:
			fmt::throw_exception("Bad tile compression type (%d)!", tile->comp);
		}
	}

	void tiled_region::read(void *dst, u32 width, u32 height, u32 pitch)
	{
		if (!tile)
		{
			memcpy(dst, ptr, height * pitch);
			return;
		}

		u32 offset_x = base % tile->pitch;
		u32 offset_y = base / tile->pitch;

		switch (tile->comp)
		{
		case CELL_GCM_COMPMODE_C32_2X1:
		case CELL_GCM_COMPMODE_DISABLED:
			for (u32 y = 0; y < height; ++y)
			{
				memcpy(static_cast<u8*>(dst) + pitch * y, ptr + (offset_y + y) * tile->pitch + offset_x, pitch);
			}
			break;
			/*
		case CELL_GCM_COMPMODE_C32_2X1:
			for (u32 y = 0; y < height; ++y)
			{
				const u32* src_line = reinterpret_cast<const u32*>(ptr + (offset_y + y) * tile->pitch + offset_x);
				u32* dst_line = reinterpret_cast<u32*>(static_cast<u8*>(dst) + pitch * y);

				for (u32 x = 0; x < width; ++x)
				{
					dst_line[x] = src_line[x * 2 + 0];
				}
			}
			break;
			*/
		case CELL_GCM_COMPMODE_C32_2X2:
			for (u32 y = 0; y < height; ++y)
			{
				const u32* src_line = reinterpret_cast<const u32*>(ptr + (offset_y + y * 2 + 0) * tile->pitch + offset_x);
				u32* dst_line = reinterpret_cast<u32*>(static_cast<u8*>(dst) + pitch * y);

				for (u32 x = 0; x < width; ++x)
				{
					dst_line[x] = src_line[x * 2 + 0];
				}
			}
			break;
		default:
			fmt::throw_exception("Bad tile compression type (%d)!", tile->comp);
		}
	}

	thread::~thread()
	{
		g_access_violation_handler = nullptr;
	}

	void thread::save(utils::serial& ar)
	{
		[[maybe_unused]] const s32 version = GET_OR_USE_SERIALIZATION_VERSION(ar.is_writing(), rsx);

		ar(rsx::method_registers);

		for (auto& v : m_draw_processor.m_vertex_push_buffers)
		{
			ar(v.attr, v.size, v.type, v.vertex_count, v.dword_count, v.data);
		}

		ar(m_draw_processor.m_element_push_buffer, fifo_ret_addr, saved_fifo_ret, zcull_surface_active, m_surface_info, m_depth_surface_info, m_framebuffer_layout);
		ar(dma_address, iomap_table, restore_point, tiles, zculls, display_buffers, display_buffers_count, current_display_buffer);
		ar(enable_second_vhandler, requested_vsync);
		ar(device_addr, label_addr, main_mem_size, local_mem_size, rsx_event_port, driver_info);
		ar(in_begin_end);
		ar(display_buffers, display_buffers_count, current_display_buffer);
		ar(unsent_gcm_events, rsx::method_registers.current_draw_clause);

		if (ar.is_writing() || version >= 2)
		{
			ar(vblank_count);

			b8 flip_pending{};

			if (ar.is_writing())
			{
				flip_pending = !!(async_flip_requested & flip_request::emu_requested);
			}

			ar(flip_pending);

			if (flip_pending)
			{
				ar(vblank_at_flip);
				ar(async_flip_buffer);

				if (!ar.is_writing())
				{
					async_flip_requested |= flip_request::emu_requested;
					flip_notification_count = 1;
				}
			}
		}

		if (ar.is_writing())
		{
			if (fifo_ctrl && state & cpu_flag::again)
			{
				ar(fifo_ctrl->get_remaining_args_count() + 1);
				ar(fifo_ctrl->last_cmd());
			}
			else
			{
				ar(u32{0});
			}
		}
		else if (u32 count{ar})
		{
			restore_fifo_count = count;
			ar(restore_fifo_cmd);
		}
	}

	thread::thread(utils::serial* _ar)
		: cpu_thread(0x5555'5555)
	{
		g_access_violation_handler = [this](u32 address, bool is_writing)
		{
			return on_access_violation(address, is_writing);
		};

		m_textures_dirty.fill(true);
		m_vertex_textures_dirty.fill(true);

		m_graphics_state |= pipeline_state::all_dirty;

		g_user_asked_for_frame_capture = false;

		// TODO: Proper context management in the driver
		s_ctx.rsxthr = this;
		m_ctx = &s_ctx;

		m_draw_processor.init(m_ctx);

		if (g_cfg.misc.use_native_interface && (g_cfg.video.renderer == video_renderer::opengl || g_cfg.video.renderer == video_renderer::vulkan))
		{
			m_overlay_manager = g_fxo->init<rsx::overlays::display_manager>(0);

			if (g_cfg.misc.play_music_during_boot)
			{
				if (const std::string audio_path = rpcs3::utils::get_game_content_path(game_content_type::content_sound); !audio_path.empty())
				{
					m_overlay_manager->start_audio(audio_path);
				}
			}
		}

		if (!_ar)
		{
			add_remove_flags({}, cpu_flag::stop); // TODO: Remove workaround
			return;
		}

		add_remove_flags(cpu_flag::suspend, cpu_flag::stop);

		serialized = true;
		save(*_ar);

		if (dma_address)
		{
			ctrl = vm::_ptr<RsxDmaControl>(dma_address);
			rsx_thread_running = true;
		}

		if (g_cfg.savestate.start_paused)
		{
			// Allow to render a whole frame within this emulation session so there won't be missing graphics
			m_pause_after_x_flips = 2;
		}
	}

	avconf::avconf(utils::serial& ar)
	{
		save(ar);
	}

	void avconf::save(utils::serial& ar)
	{
		[[maybe_unused]] const s32 version = GET_OR_USE_SERIALIZATION_VERSION(ar.is_writing(), rsx);

		if (!ar.is_writing() && version < 3)
		{
			// Be compatible with previous bitwise serialization
			ar(std::span<u8>(reinterpret_cast<u8*>(this), ::offset32(&avconf::scan_mode)));
			ar.pos += utils::align<usz>(::offset32(&avconf::scan_mode), alignof(avconf)) - ::offset32(&avconf::scan_mode);
			return;
		}

		ar(stereo_enabled, format, aspect, resolution_id, scanline_pitch, gamma, resolution_x, resolution_y, state, scan_mode);
	}

	void thread::capture_frame(const std::string& name) const
	{
		frame_trace_data::draw_state draw_state{};

		draw_state.programs = get_programs();
		draw_state.name = name;
		frame_debug.draw_calls.emplace_back(std::move(draw_state));
	}

	void thread::begin()
	{
		if (cond_render_ctrl.hw_cond_active)
		{
			if (!cond_render_ctrl.eval_pending())
			{
				// End conditional rendering if still active
				end_conditional_rendering();
			}

			// If hw cond render is enabled and evalutation is still pending, do nothing
		}
		else if (cond_render_ctrl.eval_pending())
		{
			// Evaluate conditional rendering test or enable hw cond render until results are available
			if (backend_config.supports_hw_conditional_render)
			{
				// In this mode, it is possible to skip the cond render while the backend is still processing data.
				// The backend guarantees that any draw calls emitted during this time will NOT generate any ROP writes
				ensure(!cond_render_ctrl.hw_cond_active);

				// Pending evaluation, use hardware test
				begin_conditional_rendering(cond_render_ctrl.eval_sources);
			}
			else
			{
				// NOTE: eval_sources list is reversed with newest query first
				zcull_ctrl->read_barrier(this, cond_render_ctrl.eval_address, cond_render_ctrl.eval_sources.front());
				ensure(!cond_render_ctrl.eval_pending());
			}
		}

		if (!backend_config.supports_normalized_barycentrics)
		{
			// Check for mode change between rasterized polys vs lines and points
			// Luckily this almost never happens in real games
			const auto current_mode = rsx::method_registers.current_draw_clause.classify_mode();
			if (current_mode != m_current_draw_mode)
			{
				m_graphics_state |= (rsx::vertex_program_state_dirty | rsx::fragment_program_state_dirty);
				m_current_draw_mode = current_mode;
			}
		}

		in_begin_end = true;
	}

	void thread::end()
	{
		if (capture_current_frame)
		{
			capture::capture_draw_memory(this);
		}

		in_begin_end = false;
		m_frame_stats.draw_calls++;

		method_registers.current_draw_clause.post_execute_cleanup(m_ctx);

		m_graphics_state |= rsx::pipeline_state::framebuffer_reads_dirty;
		m_eng_interrupt_mask |= rsx::backend_interrupt;
		ROP_sync_timestamp = rsx::get_shared_tag();

		m_draw_processor.clear_push_buffers();

		zcull_ctrl->on_draw();

		if (capture_current_frame)
		{
			const u32 element_count = rsx::method_registers.current_draw_clause.get_elements_count();
			capture_frame(fmt::format("Draw %s %d", rsx::method_registers.current_draw_clause.primitive, element_count));
		}
	}

	void thread::execute_nop_draw()
	{
		method_registers.current_draw_clause.begin();
		do
		{
			method_registers.current_draw_clause.execute_pipeline_dependencies(m_ctx);
		}
		while (method_registers.current_draw_clause.next());
	}

	void thread::cpu_task()
	{
		while (Emu.IsReady())
		{
			Emu.WaitReady();
		}

		do
		{
			on_task();

			state -= cpu_flag::ret;
		}
		while (!is_stopped());

		on_exit();
	}

	void thread::cpu_wait(bs_t<cpu_flag> old)
	{
#ifdef RPCS3_PS4
		// 0.29: STALL dump wait reason; counted in the 10 s 'RSX gameplay' line.
		const ps4_rsx_wait_scope ps4_wait(ps4_wait_cpu_wait);
		g_ps4_rsx_counters.cpu_waits++;
#endif
		if (external_interrupt_lock)
		{
			wait_pause();
		}

		if ((state & (cpu_flag::dbg_global_pause + cpu_flag::exit)) == cpu_flag::dbg_global_pause)
		{
			// Wait 16ms during emulation pause. This reduces cpu load while still giving us the chance to render overlays.
			do_local_task(rsx::FIFO::state::paused);
			thread_ctrl::wait_on(state, old, 16000);
		}
		else
		{
			on_semaphore_acquire_wait();
#ifdef RPCS3_PS4
            if (ps4_rsx_priority_elevated())
            {
                // Elevated RSX: a yield would re-select it ahead of the guest threads.
                std::this_thread::sleep_for(100us);
            }
            else
            {
                // Spin briefly, then yield, then sleep, timed per wait episode.
                // cpu_wait runs only on the RSX thread; thread_local avoids header changes.
                static thread_local u64 s_wait_start = 0, s_wait_end = 0;
                const u64 now = get_system_time();
                if (now - s_wait_end > 1000) s_wait_start = now; // >1 ms outside cpu_wait = new episode
                const u64 waited = now - s_wait_start;
                if (waited < 100) busy_wait(500);
                else if (waited < 2000) std::this_thread::yield();
                else std::this_thread::sleep_for(100us);
                s_wait_end = get_system_time(); // after the wait: our own sleep never ends an episode
            }
#else
			std::this_thread::yield();
#endif
		}
	}

	void thread::post_vblank_event(u64 post_event_time)
	{
		vblank_count++;

		if (isHLE)
		{
			if (auto ptr = vblank_handler)
			{
				intr_thread->cmd_list
				({
					{ ppu_cmd::set_args, 1 }, u64{1},
					{ ppu_cmd::lle_call, ptr },
					{ ppu_cmd::sleep, 0 }
				});

				intr_thread->cmd_notify.store(1);
				intr_thread->cmd_notify.notify_one();
			}
		}
		else
		{
			sys_rsx_context_attribute(0x55555555, 0xFED, 1, get_guest_system_time(post_event_time), 0, 0);
		}
	}

	namespace nv4097
	{
		void set_render_mode(context* rsx, u32, u32 arg);
	}

	void thread::on_task()
	{
		g_tls_log_prefix = []
		{
			const auto rsx = get_current_renderer();
			return fmt::format("RSX [0x%07x]", rsx->ctrl ? +rsx->ctrl->get : 0);
		};

#ifdef RPCS3_PS4
		// 0.29: wait-reason bookkeeping (STALL dump) and the elevated-RSX GPU backoff apply to this thread only.
		t_ps4_is_rsx_thread = true;
#endif

		if (!serialized) method_registers.init();

		rsx::overlays::reset_performance_overlay();
		rsx::overlays::reset_debug_overlay();

		if (!is_initialized)
		{
			g_fxo->get<rsx::dma_manager>().init();
#ifdef RPCS3_PS4
            ps4_startup_checkpoint("RSX: initialize backend on graphics thread");
#endif
			on_init_thread();
#ifdef RPCS3_PS4
            ps4_startup_checkpoint("RSX: backend ready");
#endif

			if (in_begin_end)
			{
				// on_init_thread should have prepared the backend resources
				// Run draw call warmup again if the savestate happened mid-draw
				ensure(serialized);
				begin();
			}
		}

		is_initialized = true;
		is_initialized.notify_all();

		if (!zcull_ctrl)
		{
			// Backend did not provide an implementation, provide NULL object
			zcull_ctrl = std::make_unique<::rsx::reports::ZCULL_control>();
		}

		check_zcull_status(false);
		nv4097::set_render_mode(m_ctx, 0, method_registers.registers[NV4097_SET_RENDER_ENABLE]);

		performance_counters.state = FIFO::state::empty;

		const u64 event_flags = unsent_gcm_events.exchange(0);

		if (Emu.IsStarting())
		{
			Emu.CallFromMainThread([]
			{
				Emu.RunPPU();
			});
		}

		// Wait for startup (TODO)
		while (!rsx_thread_running || Emu.IsPausedOrReady())
		{
			// Execute backend-local tasks first
			do_local_task(performance_counters.state);

			// Update sub-units
			zcull_ctrl->update(this);

			if (is_stopped())
			{
				return;
			}

			thread_ctrl::wait_for(1000);
		}

#ifdef RPCS3_PS4
        ps4_startup_checkpoint("RSX: context ready; initialize FIFO and vblank");
#endif
		performance_counters.state = FIFO::state::running;

		fifo_ctrl = std::make_unique<::rsx::FIFO::FIFO_control>(this);
		fifo_ctrl->set_get(ctrl->get);

		resolution_scaling_config =
		{
			.scale_percent = static_cast<u16>(g_cfg.video.resolution_scale_percent),
			.min_scalable_dimension = static_cast<u16>(g_cfg.video.min_scalable_dimension),
		};

		last_guest_flip_timestamp = get_system_time() - 1000000;

		vblank_count = 0;

		if (restore_fifo_count)
		{
			fifo_ctrl->restore_state(restore_fifo_cmd, restore_fifo_count);
		}

		if (!send_event(0, event_flags, 0))
		{
			return;
		}

		g_fxo->get<vblank_thread>().set_thread(std::shared_ptr<named_thread<std::function<void()>>>(new named_thread<std::function<void()>>("VBlank Thread"sv, [this]() -> void
		{
#ifdef __linux__
			constexpr u32 host_min_quantum = 10;
#else
			constexpr u32 host_min_quantum = 500;
#endif
			u64 start_time = get_system_time();

			u64 vblank_rate = g_cfg.video.vblank_rate;
			u64 vblank_period = 1'000'000 + u64{g_cfg.video.vblank_ntsc.get()} * 1000;

			u64 local_vblank_count = 0;

			// TODO: exit condition
			while (!is_stopped() && !unsent_gcm_events && thread_ctrl::state() != thread_state::aborting)
			{
				// Get current time
				const u64 current = get_system_time();

				// Calculate the time at which we need to send a new VBLANK signal
				const u64 post_event_time = start_time + (local_vblank_count + 1) * vblank_period / vblank_rate;

				// Calculate time remaining to that time (0 if we passed it)
				const u64 wait_for = current >= post_event_time ? 0 : post_event_time - current;

#ifdef __linux__
				const u64 wait_sleep = wait_for;
#else
				// Substract host operating system min sleep quantom to get sleep time
				const u64 wait_sleep = wait_for - u64{wait_for >= host_min_quantum} * host_min_quantum;
#endif

				if (!wait_for)
				{
					{
						local_vblank_count++;

						if (local_vblank_count == vblank_rate)
						{
							// Advance start_time to the moment of the current VBLANK
							// Which is the last VBLANK event in this period
							// This is in order for multiplication by ratio above to use only small numbers
							start_time += vblank_period;
							local_vblank_count = 0;

							// We have a rare chance to update settings without losing precision whenever local_vblank_count is 0
							vblank_rate = g_cfg.video.vblank_rate;
							vblank_period = 1'000'000 + u64{g_cfg.video.vblank_ntsc.get()} * 1000;
						}

						post_vblank_event(post_event_time);
					}
				}
				else if (wait_sleep)
				{
					thread_ctrl::wait_for(wait_sleep);
				}
				else if (wait_for >= host_min_quantum / 3 * 2)
				{
					std::this_thread::yield();
				}

				if (Emu.IsPaused())
				{
					// Save the difference before pause
					start_time = get_system_time() - start_time;

					while (Emu.IsPaused() && !is_stopped())
					{
						thread_ctrl::wait_for(5'000);
					}

					// Restore difference
					start_time = get_system_time() - start_time;
				}
			}
		})));

		struct join_vblank
		{
			~join_vblank() noexcept
			{
				g_fxo->get<vblank_thread>() = thread_state::finished;
			}

		} join_vblank_obj{};

		// Raise priority above other threads
#ifdef RPCS3_PS4
        // 0.26: elevated by default (see ps4_rsx_priority_elevated above); its waits then sleep, so the
        // PPU/SPU threads below it still get the cores. /data/rpcs3/rsx-normal-priority.flag keeps the base
        // priority. scoped_priority(0) is a no-op (ps4_thread_priority.hpp).
        const bool rsx_high_priority = !fs::is_file("/data/rpcs3/rsx-normal-priority.flag");
        s_ps4_rsx_priority_elevated.release(rsx_high_priority);
        ps4_startup_checkpoint(rsx_high_priority ? "RSX: vblank created; apply elevated native priority (default; rsx-normal-priority.flag disables)"
            : "RSX: vblank created; keep base native priority (rsx-normal-priority.flag)");
		thread_ctrl::scoped_priority high_prio(rsx_high_priority ? +1 : 0);
        ps4_startup_checkpoint("RSX: native priority applied");

        // 0.29: Orbis sleeps last a whole kernel tick (~1 ms), so the raised RSX noticed a
        // PUT update that arrived right after the FIFO drained up to ~1 ms late. It now polls (no syscall) for
        // this window first. /data/rpcs3/rsx-idle-poll-us.txt: 0 restores the 0.28 behaviour, at most 2000.
        // 0.37: with the HR timer the idle sleep lasts ~150-250 us instead of a tick, so the default poll is 50 us.
        u32 idle_poll_us = utils::ps4::hr_wait_enabled() ? 50 : 200;
        if (const fs::file f{"/data/rpcs3/rsx-idle-poll-us.txt"}; f)
        {
            // 0.29: an oversized file is reported as ignored too (empty text never parses).
            const std::string text = f.size() < 16 ? f.to_string() : std::string();
            char* end = nullptr;
            const unsigned long value = std::strtoul(text.c_str(), &end, 10);
            if (end != text.c_str() && value <= 2000)
                idle_poll_us = static_cast<u32>(value);
            else
                ps4_startup_checkpoint("RSX: rsx-idle-poll-us.txt ignored (expected 0..2000)");
        }
        s_ps4_rsx_idle_poll_us.release(idle_poll_us);

        // 0.34: NV406E semaphore waits spin 200 us, step down to the guest priority and yield, and after
        // rsx-sema-sleep-us.txt (default 1000 us, 0 = never) sleep one kernel tick per poll: the 0.33 spin held a
        // whole core for every frame-long wait. rsx-sema-spin.flag restores the spin; the 0.29 opt-in
        // rsx-sema-yield.flag is the default now and has no effect.
        const bool sema_spin = fs::is_file("/data/rpcs3/rsx-sema-spin.flag");
        s_ps4_rsx_sema_yield.release(!sema_spin);
        if (fs::is_file("/data/rpcs3/rsx-sema-yield.flag"))
            ps4_startup_checkpoint("RSX: rsx-sema-yield.flag ignored (stepping down is the default since 0.34; rsx-sema-spin.flag restores the spin)");
        u32 sema_sleep_us = 1000;
        if (const fs::file f{"/data/rpcs3/rsx-sema-sleep-us.txt"}; f)
        {
            const std::string text = f.size() < 16 ? f.to_string() : std::string();
            char* end = nullptr;
            const unsigned long value = std::strtoul(text.c_str(), &end, 10);
            if (end != text.c_str() && value <= 20000)
                sema_sleep_us = static_cast<u32>(value);
            else
                ps4_startup_checkpoint("RSX: rsx-sema-sleep-us.txt ignored (expected 0..20000)");
        }
        s_ps4_rsx_sema_sleep_us.release(sema_sleep_us);
        // The startup.log budget of 'RSX: semaphore ... waited' lines is per boot.
        s_ps4_sema_notes.release(0);
        s_ps4_sema_long_waits.release(0);
        // 0.29: per-draw profiler timers feed the 10 s line; rsx-profiler-off.flag measures their cost.
        s_ps4_rsx_profiler.release(!fs::is_file("/data/rpcs3/rsx-profiler-off.flag"));
        // 0.34: the phase timers (FIFO packets, begin) follow the same switch. Both run in 1 frame of 8 (on_frame_end).
        g_ps4_rsx_phase.on.release(s_ps4_rsx_profiler.load());
        g_ps4_rsx_phase.sampled.release(s_ps4_rsx_profiler.load());
        // Deferred ZCULL reports written while the RSX waits on a semaphore or idles (ps4_service_reports_*).
        s_ps4_report_drain.release(!fs::is_file("/data/rpcs3/rsx-no-report-drain.flag"));
        s_ps4_report_hint_noted.release(false);
        s_ps4_report_forced_noted.release(false);
        s_ps4_report_idle_noted.release(false);
        s_ps4_report_idle_forced_noted.release(false);
        // NV406E/NV4097 label reads and writes of this thread: the sudo view never faults on a ZCULL-locked label page.
        s_ps4_label_sudo.release(!fs::is_file("/data/rpcs3/rsx-label-direct.flag"));
        ps4_startup_checkpoint(s_ps4_label_sudo.load() ? "RSX: semaphore labels are read and written through the unprotected view (default; rsx-label-direct.flag restores the guest view)"
            : "RSX: semaphore labels are read and written through the guest view (rsx-label-direct.flag)");
        s_ps4_dead_fifo_stop.release(fs::is_file("/data/rpcs3/dead-fifo-stop.flag"));
        if (s_ps4_dead_fifo_stop.load())
            ps4_startup_checkpoint("RSX: a dead FIFO stops the game and returns to the XMB (dead-fifo-stop.flag)");
        std::string sema_mode = "spins (rsx-sema-spin.flag)";
        if (!sema_spin)
        {
            sema_mode = sema_sleep_us ? fmt::format("steps down to the guest priority after 200 us and sleeps per poll after %u us (%u in cool mode, now %s; "
                "rsx-sema-sleep-us.txt, rsx-sema-spin.flag)", sema_sleep_us, std::min<u32>(sema_sleep_us, 200), utils::ps4::cool_mode::enabled() ? "on" : "off")
                : std::string("steps down to the guest priority after 200 us and never sleeps (0 in rsx-sema-sleep-us.txt)");
        }
        ps4_startup_checkpoint(fmt::format("RSX: idle poll window %u us before the FIFO-idle sleep (rsx-idle-poll-us.txt)%s; "
            "semaphore wait %s; profiler and phase timers %s for the 10 s 'RSX gameplay' line (rsx-profiler-off.flag)",
            idle_poll_us, rsx_high_priority ? "" : " (elevated RSX only, inactive with rsx-normal-priority.flag)",
            sema_mode, s_ps4_rsx_profiler.load() ? "on, sampled in 1 frame of 8" : "off").c_str());
        ps4_startup_checkpoint(fmt::format("RSX: deferred ZCULL reports are %s while the RSX waits on a semaphore or idles (rsx-no-report-drain.flag); "
            "FIFO-idle and semaphore sleeps use %s (no-hr-wait.flag)",
            s_ps4_report_drain.load() ? "flushed and polled" : "NOT serviced", utils::ps4::hr_wait_enabled() ? "the HR timer" : "sceKernelUsleep").c_str());
#else
		thread_ctrl::scoped_priority high_prio(+1);
#endif

		if (g_cfg.core.thread_scheduler != thread_scheduler_mode::os)
		{
			thread_ctrl::set_thread_affinity_mask(thread_ctrl::get_affinity_mask(thread_class::rsx));
		}

		if (auto manager = g_fxo->try_get<rsx::overlays::display_manager>())
		{
#ifdef RPCS3_PS4
            ps4_startup_checkpoint("RSX: release presentation audio");
#endif
			manager->stop_audio();
#ifdef RPCS3_PS4
            ps4_startup_checkpoint("RSX: presentation audio released");
#endif
		}

#ifdef RPCS3_PS4
        s_ps4_rsx_native_thread.release(reinterpret_cast<u64>(pthread_self()));
        ps4_startup_checkpoint("RSX: entering command processing loop");
        u64 next_fifo_report = get_system_time() + 10'000'000;
#endif
		while (!test_stopped())
		{
			// Wait for external pause events
			if (external_interrupt_lock)
			{
				wait_pause();

				if (!rsx_thread_running)
				{
					return;
				}
			}

			// Note a possible rollback address
			if (sync_point_request && !in_begin_end)
			{
				restore_point = ctrl->get;
				saved_fifo_ret = fifo_ret_addr;
				sync_point_request.release(false);
			}

			// Update sub-units every 64 cycles. The local handler is invoked for other functions externally on-demand anyway.
			// This avoids expensive calls to check timestamps which involves reading some values from TLS storage on windows.
			// If something is going on in the backend that requires an update, set the interrupt bit explicitly.
			if ((m_cycles_counter++ & 63) == 0 || m_eng_interrupt_mask)
			{
#ifdef RPCS3_PS4
                const u64 now = get_system_time();
                // 0.29: loop heartbeat for the STALL dump, using the clock read this block already does.
                s_ps4_rsx_heartbeat.release(now);
                s_ps4_rsx_loops.release(m_cycles_counter);
                if (now >= next_fifo_report)
                {
                    ps4_startup_checkpoint(fmt::format("RSX FIFO: get=0x%x put=0x%x state=%u, in_begin_end=%u, vblank=%llu pending_events=0x%llx",
                        ctrl->get.load(), ctrl->put.load(), static_cast<u32>(performance_counters.state), in_begin_end,
                        vblank_count.load(), unsent_gcm_events.load()).c_str());
                    next_fifo_report = now + 10'000'000;
                }
#endif
				// Execute backend-local tasks first
				do_local_task(performance_counters.state);

				// Update other sub-units
				zcull_ctrl->update(this);

				if (m_host_dma_ctrl)
				{
					m_host_dma_ctrl->update();
				}
			}

			// Execute FIFO queue
			run_FIFO();
		}
	}

	void thread::on_exit()
	{
		if (zcull_ctrl)
		{
			zcull_ctrl->sync(this);
		}

#ifdef RPCS3_PS4
		// 0.37: the vertex copies queued on the RSX copy worker finish while their faults still have a handler (and
		// this thread can still clear a fault the worker waits in); nothing is queued after this point
		ps4_copy_worker_barrier();
#endif

		// Deregister violation handler
		g_access_violation_handler = nullptr;

		// Clear any pending flush requests to release threads
		std::this_thread::sleep_for(10ms);
		do_local_task(rsx::FIFO::state::lock_wait);

		g_fxo->get<rsx::dma_manager>().join();
		g_fxo->get<vblank_thread>() = thread_state::finished;
		state += cpu_flag::exit;
	}

	u64 thread::timestamp()
	{
		const u64 freq = sys_time_get_timebase_frequency();

		auto get_time_ns = [freq]()
		{
			const u64 t = get_timebased_time();
			return (t / freq * 1'000'000'000 + t % freq * 1'000'000'000 / freq);
		};

		const u64 t = get_time_ns();
		if (t != timestamp_ctrl)
		{
			timestamp_ctrl = t;
			timestamp_subvalue = 0;
			return t;
		}

		// Check if we passed the limit of what fixed increments is legal for
		// Wait for the next time value reported if we passed the limit
		if ((1'000'000'000 / freq) - timestamp_subvalue <= 2)
		{
			u64 now = get_time_ns();

			for (; t == now; now = get_time_ns())
			{
				utils::pause();
			}

			timestamp_ctrl = now;
			timestamp_subvalue = 0;
			return now;
		}

		timestamp_subvalue += 2;
		return t + timestamp_subvalue;
	}

	void thread::do_local_task(FIFO::state state)
	{
		m_eng_interrupt_mask.clear(rsx::backend_interrupt);

		if (async_flip_requested & flip_request::emu_requested)
		{
			// NOTE: This has to be executed immediately
			// Delaying this operation can cause desync due to the delay in firing the flip event
			handle_emu_flip(async_flip_buffer);
		}

		if (state != FIFO::state::lock_wait)
		{
			if (!in_begin_end && atomic_storage<u32>::load(m_invalidated_memory_range.end) != 0)
			{
				std::lock_guard lock(m_mtx_task);

				if (m_invalidated_memory_range.valid())
				{
					handle_invalidated_memory_range();
				}
			}

			if (m_eng_interrupt_mask & rsx::dma_control_interrupt && !is_stopped())
			{
				if (const u64 get_put = new_get_put.exchange(u64{umax});
					get_put != umax)
				{
					vm::_ptr<atomic_be_t<u64>>(dma_address + ::offset32(&RsxDmaControl::put))->release(get_put);
					fifo_ctrl->set_get(static_cast<u32>(get_put));
					fifo_ctrl->abort();
					fifo_ret_addr = RSX_CALL_STACK_EMPTY;
					last_known_code_start = static_cast<u32>(get_put);
					sync_point_request.release(true);
				}

				m_eng_interrupt_mask.clear(rsx::dma_control_interrupt);
			}
		}

		if (m_eng_interrupt_mask & rsx::pipe_flush_interrupt)
		{
			sync();
		}

		if (is_stopped())
		{
			std::lock_guard lock(m_mtx_task);

			m_invalidated_memory_range = utils::address_range32::start_end(0x2 << 28, constants::local_mem_base + local_mem_size - 1);
			handle_invalidated_memory_range();
		}
	}

	std::array<u32, 4> thread::get_color_surface_addresses() const
	{
		u32 offset_color[] =
		{
			rsx::method_registers.surface_offset(0),
			rsx::method_registers.surface_offset(1),
			rsx::method_registers.surface_offset(2),
			rsx::method_registers.surface_offset(3),
		};
		u32 context_dma_color[] =
		{
			rsx::method_registers.surface_dma(0),
			rsx::method_registers.surface_dma(1),
			rsx::method_registers.surface_dma(2),
			rsx::method_registers.surface_dma(3),
		};
		return
		{
			rsx::get_address(offset_color[0], context_dma_color[0]),
			rsx::get_address(offset_color[1], context_dma_color[1]),
			rsx::get_address(offset_color[2], context_dma_color[2]),
			rsx::get_address(offset_color[3], context_dma_color[3]),
		};
	}

	u32 thread::get_zeta_surface_address() const
	{
		u32 m_context_dma_z = rsx::method_registers.surface_z_dma();
		u32 offset_zeta = rsx::method_registers.surface_z_offset();
		return rsx::get_address(offset_zeta, m_context_dma_z);
	}

	void thread::get_framebuffer_layout(rsx::framebuffer_creation_context context, framebuffer_layout &layout)
	{
		layout = {};

		layout.ignore_change = true;
		layout.width = rsx::method_registers.surface_clip_width();
		layout.height = rsx::method_registers.surface_clip_height();

		m_graphics_state.clear(rsx::rtt_config_contested | rsx::rtt_config_valid);
		m_current_framebuffer_context = context;

		if (layout.width == 0 || layout.height == 0)
		{
			rsx_log.trace("Invalid framebuffer setup, w=%d, h=%d", layout.width, layout.height);
			return;
		}

		//const u16 clip_x = rsx::method_registers.surface_clip_origin_x();
		//const u16 clip_y = rsx::method_registers.surface_clip_origin_y();

		layout.color_addresses = get_color_surface_addresses();
		layout.zeta_address = get_zeta_surface_address();
		layout.zeta_pitch = rsx::method_registers.surface_z_pitch();
		layout.color_pitch =
		{
			rsx::method_registers.surface_pitch(0),
			rsx::method_registers.surface_pitch(1),
			rsx::method_registers.surface_pitch(2),
			rsx::method_registers.surface_pitch(3),
		};

		layout.color_format = rsx::method_registers.surface_color();
		layout.depth_format = rsx::method_registers.surface_depth_fmt();
		layout.target = rsx::method_registers.surface_color_target();

		const auto mrt_buffers = rsx::utility::get_rtt_indexes(layout.target);
		const auto aa_mode = rsx::method_registers.surface_antialias();
		const u32 aa_factor_u = (aa_mode == rsx::surface_antialiasing::center_1_sample) ? 1 : 2;
		const u32 aa_factor_v = (aa_mode == rsx::surface_antialiasing::center_1_sample || aa_mode == rsx::surface_antialiasing::diagonal_centered_2_samples) ? 1 : 2;
		const u8 sample_count = get_format_sample_count(aa_mode);

		const auto depth_texel_size = get_format_block_size_in_bytes(layout.depth_format) * aa_factor_u;
		const auto color_texel_size = get_format_block_size_in_bytes(layout.color_format) * aa_factor_u;
		const bool stencil_test_enabled = is_depth_stencil_format(layout.depth_format) && rsx::method_registers.stencil_test_enabled();
		const bool depth_test_enabled = rsx::method_registers.depth_test_enabled();

		// Check write masks
		layout.zeta_write_enabled = (depth_test_enabled && rsx::method_registers.depth_write_enabled());
		if (!layout.zeta_write_enabled && stencil_test_enabled)
		{
			// Check if stencil data is modified
			auto mask = rsx::method_registers.stencil_mask();
			bool active_write_op = (rsx::method_registers.stencil_op_zpass() != rsx::stencil_op::keep ||
				rsx::method_registers.stencil_op_fail() != rsx::stencil_op::keep ||
				rsx::method_registers.stencil_op_zfail() != rsx::stencil_op::keep);

			if ((!mask || !active_write_op) && rsx::method_registers.two_sided_stencil_test_enabled())
			{
				mask |= rsx::method_registers.back_stencil_mask();
				active_write_op |= (rsx::method_registers.back_stencil_op_zpass() != rsx::stencil_op::keep ||
					rsx::method_registers.back_stencil_op_fail() != rsx::stencil_op::keep ||
					rsx::method_registers.back_stencil_op_zfail() != rsx::stencil_op::keep);
			}

			layout.zeta_write_enabled = (mask && active_write_op);
		}

		// NOTE: surface_target_a is index 1 but is not MRT since only one surface is active
		bool color_write_enabled = false;
		for (uint i = 0; i < mrt_buffers.size(); ++i)
		{
			if (rsx::method_registers.color_write_enabled(i))
			{
				const auto real_index = mrt_buffers[i];
				layout.color_write_enabled[real_index] = true;
				color_write_enabled = true;
			}
		}

		bool depth_buffer_unused = false, color_buffer_unused = false;

		switch (context)
		{
		case rsx::framebuffer_creation_context::context_clear_all:
			break;
		case rsx::framebuffer_creation_context::context_clear_depth:
			color_buffer_unused = true;
			break;
		case rsx::framebuffer_creation_context::context_clear_color:
			depth_buffer_unused = true;
			break;
		case rsx::framebuffer_creation_context::context_draw:
			// NOTE: As with all other hw, depth/stencil writes involve the corresponding depth/stencil test, i.e No test = No write
			// NOTE: Depth test is not really using the memory if its set to always or never
			// TODO: Perform similar checks for stencil test
			if (!stencil_test_enabled)
			{
				if (!depth_test_enabled)
				{
					depth_buffer_unused = true;
				}
				else if (!rsx::method_registers.depth_write_enabled())
				{
					// Depth test is enabled but depth write is disabled
					switch (rsx::method_registers.depth_func())
					{
					default:
						break;
					case rsx::comparison_function::never:
					case rsx::comparison_function::always:
						// No access to depth buffer memory
						depth_buffer_unused = true;
						break;
					}
				}

				if (depth_buffer_unused) [[unlikely]]
				{
					// Check if depth bounds is active. Depth bounds test does NOT need depth test to be enabled to access the Z buffer
					// Bind Z buffer in read mode for bounds check in this case
					if (rsx::method_registers.depth_bounds_test_enabled() &&
						(rsx::method_registers.depth_bounds_min() > 0.f || rsx::method_registers.depth_bounds_max() < 1.f))
					{
						depth_buffer_unused = false;
					}
				}
			}

			color_buffer_unused = !color_write_enabled || layout.target == rsx::surface_target::none;
			if (color_buffer_unused || depth_buffer_unused)
			{
				m_graphics_state.set(rsx::rtt_config_contested);
			}
			break;
		default:
			fmt::throw_exception("Unknown framebuffer context 0x%x", static_cast<u32>(context));
		}

		// Swizzled render does tight packing of bytes
		bool packed_render = false;
		u32 minimum_color_pitch = 64u;
		u32 minimum_zeta_pitch = 64u;

		switch (layout.raster_type = rsx::method_registers.surface_type())
		{
		default:
			rsx_log.error("Unknown raster mode 0x%x", static_cast<u32>(layout.raster_type));
			[[fallthrough]];
		case rsx::surface_raster_type::linear:
			break;
		case rsx::surface_raster_type::swizzle:
			packed_render = true;
			break;
		}

		if (!packed_render)
		{
			// Well, this is a write operation either way (clearing or drawing)
			// We can deduce a minimum pitch for which this operation is guaranteed to require by checking for the lesser of scissor or clip
			const u32 write_limit_x = std::min<u32>(layout.width, rsx::method_registers.scissor_origin_x() + rsx::method_registers.scissor_width());

			minimum_color_pitch = color_texel_size * write_limit_x;
			minimum_zeta_pitch = depth_texel_size * write_limit_x;

			// Check for size fit and attempt to correct incorrect inputs.
			// BLUS30072 is misconfigured here and renders fine on PS3. The width fails to account for AA being active in that engine.
			u16 corrected_width = umax;
			std::vector<u32*> pitch_fixups;

			if (!depth_buffer_unused)
			{
				if (layout.zeta_pitch < minimum_zeta_pitch)
				{
					// Observed in CoD3 where the depth buffer is clearly misconfigured.
					if (layout.zeta_pitch > 64)
					{
						corrected_width = layout.zeta_pitch / depth_texel_size;
						layout.zeta_pitch = depth_texel_size;
						pitch_fixups.push_back(&layout.zeta_pitch);
					}
					else
					{
						rsx_log.warning("Misconfigured surface could not fit a depth buffer. Dropping.");
						layout.zeta_address = 0;
					}
				}
				else if (layout.width * depth_texel_size > layout.zeta_pitch)
				{
					// This is ok, misconfigured raster dimensions, but we're only writing the pitch as determined by the scissor
					corrected_width = layout.zeta_pitch / depth_texel_size;
				}
			}

			if (!color_buffer_unused)
			{
				for (const auto& index : rsx::utility::get_rtt_indexes(layout.target))
				{
					if (layout.color_pitch[index] < minimum_color_pitch)
					{
						if (layout.color_pitch[index] > 64)
						{
							corrected_width = std::min<u16>(corrected_width, layout.color_pitch[index] / color_texel_size);
							layout.color_pitch[index] = color_texel_size;
							pitch_fixups.push_back(&layout.color_pitch[index]);
						}
						else
						{
							rsx_log.warning("Misconfigured surface could not fit color buffer %d. Dropping.", index);
							layout.color_addresses[index] = 0;
						}

						continue;
					}

					if (layout.width * color_texel_size > layout.color_pitch[index])
					{
						// This is ok, misconfigured raster dimensions, but we're only writing the pitch as determined by the scissor
						corrected_width = std::min<u16>(corrected_width, layout.color_pitch[index] / color_texel_size);
					}
				}
			}

			if (corrected_width != umax)
			{
				layout.width = corrected_width;

				for (auto& value : pitch_fixups)
				{
					*value = *value * layout.width;
				}
			}
		}

		if (depth_buffer_unused)
		{
			layout.zeta_address = 0;
		}
		else if (packed_render)
		{
			layout.actual_zeta_pitch = (layout.width * depth_texel_size);
		}
		else
		{
			layout.actual_zeta_pitch = layout.zeta_pitch;
		}

		for (const auto &index : rsx::utility::get_rtt_indexes(layout.target))
		{
			if (color_buffer_unused)
			{
				layout.color_addresses[index] = 0;
				continue;
			}

			if (layout.color_pitch[index] < minimum_color_pitch)
			{
				// Unlike the depth buffer, when given a color target we know it is intended to be rendered to
				rsx_log.warning("Framebuffer setup error: Color target failed pitch check, Pitch=[%d, %d, %d, %d] + %d, target=%d, context=%d",
					layout.color_pitch[0], layout.color_pitch[1], layout.color_pitch[2], layout.color_pitch[3],
					layout.zeta_pitch, static_cast<u32>(layout.target), static_cast<u32>(context));

				// Some games (COD4) are buggy and set incorrect width + AA + pitch combo. Force fit in such scenarios.
				if (layout.color_pitch[index] > 64)
				{
					layout.width = layout.color_pitch[index] / color_texel_size;
				}
				else
				{
					layout.color_addresses[index] = 0;
					continue;
				}
			}

			if (layout.color_addresses[index] == layout.zeta_address)
			{
				rsx_log.warning("Framebuffer at 0x%X has aliasing color/depth targets, color_index=%d, zeta_pitch = %d, color_pitch=%d, context=%d",
					layout.zeta_address, index, layout.zeta_pitch, layout.color_pitch[index], static_cast<u32>(context));

				m_graphics_state.set(rsx::rtt_config_contested);

				if (g_cfg.video.fb_aliasing_bias == framebuffer_aliasing_bias::prefer_color
					&& layout.color_write_enabled[index]
					&& !layout.zeta_write_enabled)
				{
					// Use address for color data
					layout.zeta_address = 0;
				}
				else
				{
					// TODO: Research clearing both depth AND color
					// TODO: If context is creation_draw, deal with possibility of a lost buffer clear
					if (depth_test_enabled || stencil_test_enabled || (!layout.color_write_enabled[index] && layout.zeta_write_enabled))
					{
						// Use address for depth data
						layout.color_addresses[index] = 0;
						continue;
					}
					else
					{
						// Use address for color data
						layout.zeta_address = 0;
					}
				}
			}

			ensure(layout.color_addresses[index]);

			const auto packed_pitch = (layout.width * color_texel_size);
			if (packed_render)
			{
				layout.actual_color_pitch[index] = packed_pitch;
			}
			else
			{
				layout.actual_color_pitch[index] = layout.color_pitch[index];
			}

			m_graphics_state.set(rsx::rtt_config_valid);
		}

		if (!m_graphics_state.test(rsx::rtt_config_valid) && !layout.zeta_address)
		{
			rsx_log.warning("Framebuffer setup failed. Draw calls may have been lost");
			return;
		}

		// At least one attachment exists
		m_graphics_state.set(rsx::rtt_config_valid);

		// Window (raster) offsets
		const auto window_offset_x = rsx::method_registers.window_offset_x();
		const auto window_offset_y = rsx::method_registers.window_offset_y();
		const auto window_clip_width = rsx::method_registers.window_clip_horizontal();
		const auto window_clip_height = rsx::method_registers.window_clip_vertical();

		if (window_offset_x || window_offset_y)
		{
			// Window offset is what affects the raster position!
			// Tested with Turbo: Super stunt squad that only changes the window offset to declare new framebuffers
			// Sampling behavior clearly indicates the addresses are expected to have changed
			if (auto clip_type = rsx::method_registers.window_clip_type())
				rsx_log.error("Unknown window clip type 0x%X", clip_type);

			for (const auto &index : rsx::utility::get_rtt_indexes(layout.target))
			{
				if (layout.color_addresses[index])
				{
					const u32 window_offset_bytes = (layout.actual_color_pitch[index] * window_offset_y) + (color_texel_size * window_offset_x);
					layout.color_addresses[index] += window_offset_bytes;
				}
			}

			if (layout.zeta_address)
			{
				layout.zeta_address += (layout.actual_zeta_pitch * window_offset_y) + (depth_texel_size * window_offset_x);
			}
		}

		if ((window_clip_width && window_clip_width < layout.width) ||
			(window_clip_height && window_clip_height < layout.height))
		{
			rsx_log.error("Unexpected window clip dimensions: window_clip=%dx%d, surface_clip=%dx%d",
				window_clip_width, window_clip_height, layout.width, layout.height);
		}

		layout.aa_mode = aa_mode;
		layout.aa_factors[0] = aa_factor_u;
		layout.aa_factors[1] = aa_factor_v;

		// Log this to frame stats
		if (layout.target != rsx::surface_target::none)
		{
			m_frame_stats.framebuffer_stats.add(layout.width, layout.height, aa_mode);
		}

		// Check if anything has changed
		bool really_changed = false;

		for (u8 i = 0; i < rsx::limits::color_buffers_count; ++i)
		{
			if (m_surface_info[i].address != layout.color_addresses[i])
			{
				really_changed = true;
				break;
			}

			if (layout.color_addresses[i])
			{
				if (m_surface_info[i].width != layout.width ||
					m_surface_info[i].height != layout.height ||
					m_surface_info[i].color_format != layout.color_format ||
					m_surface_info[i].samples != sample_count)
				{
					really_changed = true;
					break;
				}
			}
		}

		if (!really_changed)
		{
			if (layout.zeta_address == m_depth_surface_info.address &&
				layout.depth_format == m_depth_surface_info.depth_format &&
				sample_count == m_depth_surface_info.samples)
			{
				// Same target is reused
				return;
			}
		}

		layout.ignore_change = false;
	}

	void thread::on_framebuffer_options_changed(u32 opt)
	{
		if (m_graphics_state & rsx::rtt_config_dirty)
		{
			// Nothing to do
			return;
		}

		auto set_zeta_write_enabled = [&](bool state)
		{
			if (state == m_framebuffer_layout.zeta_write_enabled)
			{
				return;
			}

			if (m_graphics_state & rsx::zeta_address_is_cyclic)
			{
				m_graphics_state |= rsx::fragment_program_state_dirty;
			}
			m_framebuffer_layout.zeta_write_enabled = state;
		};

		auto evaluate_depth_buffer_state = [&]()
		{
			const bool zeta_write_en = (rsx::method_registers.depth_test_enabled() && rsx::method_registers.depth_write_enabled());
			set_zeta_write_enabled(zeta_write_en);
		};

		auto evaluate_stencil_buffer_state = [&]()
		{
			if (!m_framebuffer_layout.zeta_write_enabled &&
				rsx::method_registers.stencil_test_enabled() &&
				is_depth_stencil_format(m_framebuffer_layout.depth_format))
			{
				// Check if stencil data is modified
				auto mask = rsx::method_registers.stencil_mask();
				bool active_write_op = (rsx::method_registers.stencil_op_zpass() != rsx::stencil_op::keep ||
					rsx::method_registers.stencil_op_fail() != rsx::stencil_op::keep ||
					rsx::method_registers.stencil_op_zfail() != rsx::stencil_op::keep);

				if ((!mask || !active_write_op) && rsx::method_registers.two_sided_stencil_test_enabled())
				{
					mask |= rsx::method_registers.back_stencil_mask();
					active_write_op |= (rsx::method_registers.back_stencil_op_zpass() != rsx::stencil_op::keep ||
						rsx::method_registers.back_stencil_op_fail() != rsx::stencil_op::keep ||
						rsx::method_registers.back_stencil_op_zfail() != rsx::stencil_op::keep);
				}

				set_zeta_write_enabled(mask && active_write_op);
			}
		};

		auto evaluate_color_buffer_state = [&]() -> bool
		{
			const auto mrt_buffers = rsx::utility::get_rtt_indexes(m_framebuffer_layout.target);
			bool any_found = false;

			for (uint i = 0; i < mrt_buffers.size(); ++i)
			{
				if (REGS(m_ctx)->color_write_enabled(i))
				{
					const auto real_index = mrt_buffers[i];
					m_framebuffer_layout.color_write_enabled[real_index] = true;
					any_found = true;
				}
			}

			on_framebuffer_layout_updated();
			return any_found;
		};

		auto evaluate_depth_buffer_contested = [&]()
		{
			if (m_framebuffer_layout.zeta_address) [[likely]]
			{
				// Nothing to do, depth buffer already exists
				return false;
			}

			// Check if depth read/write is enabled
			if (m_framebuffer_layout.zeta_write_enabled ||
				rsx::method_registers.depth_test_enabled())
			{
				return true;
			}

			// Check if stencil read is enabled
			if (is_depth_stencil_format(m_framebuffer_layout.depth_format) &&
				rsx::method_registers.stencil_test_enabled())
			{
				return true;
			}

			return false;
		};

		switch (opt)
		{
		case NV4097_SET_DEPTH_TEST_ENABLE:
		case NV4097_SET_DEPTH_MASK:
		case NV4097_SET_DEPTH_FUNC:
		{
			evaluate_depth_buffer_state();

			if (m_graphics_state.test(rsx::rtt_config_contested) && evaluate_depth_buffer_contested())
			{
				m_graphics_state.set(rsx::rtt_config_dirty);
			}
			break;
		}
		case NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE:
		case NV4097_SET_STENCIL_TEST_ENABLE:
		case NV4097_SET_STENCIL_MASK:
		case NV4097_SET_STENCIL_OP_ZPASS:
		case NV4097_SET_STENCIL_OP_FAIL:
		case NV4097_SET_STENCIL_OP_ZFAIL:
		case NV4097_SET_BACK_STENCIL_MASK:
		case NV4097_SET_BACK_STENCIL_OP_ZPASS:
		case NV4097_SET_BACK_STENCIL_OP_FAIL:
		case NV4097_SET_BACK_STENCIL_OP_ZFAIL:
		{
			// Stencil takes a back seat to depth buffer stuff
			evaluate_depth_buffer_state();

			if (!m_framebuffer_layout.zeta_write_enabled)
			{
				evaluate_stencil_buffer_state();
			}

			if (m_graphics_state.test(rsx::rtt_config_contested) && evaluate_depth_buffer_contested())
			{
				m_graphics_state.set(rsx::rtt_config_dirty);
			}
			break;
		}
		case NV4097_SET_COLOR_MASK:
		case NV4097_SET_COLOR_MASK_MRT:
		{
			if (!m_graphics_state.test(rsx::rtt_config_contested)) [[likely]]
			{
				// Update write masks and continue
				evaluate_color_buffer_state();
			}
			else
			{
				bool old_state = false;
				for (const auto& enabled : m_framebuffer_layout.color_write_enabled)
				{
					if (old_state = enabled; old_state) break;
				}

				const auto new_state = evaluate_color_buffer_state();
				if (!old_state && new_state)
				{
					// Color buffers now in use
					m_graphics_state.set(rsx::rtt_config_dirty);
				}
			}
			break;
		}
		default:
			rsx_log.fatal("Unhandled framebuffer option changed 0x%x", opt);
			break;
		}
	}

	void thread::on_framebuffer_layout_updated()
	{
		if (m_graphics_state.test(rsx::fragment_program_state_dirty))
		{
			return;
		}

		const auto target = REGS(m_ctx)->surface_color_target();
		if (rsx::utility::get_mrt_buffers_count(target) == current_fragment_program.mrt_buffers_count)
		{
			return;
		}

		m_graphics_state |= rsx::fragment_program_state_dirty;
	}

	bool thread::get_scissor(areau& region, bool clip_viewport)
	{
		if (!m_graphics_state.test(rsx::pipeline_state::scissor_config_state_dirty))
		{
			if (clip_viewport == m_graphics_state.test(rsx::pipeline_state::scissor_setup_clipped))
			{
				// Nothing to do
				return false;
			}
		}

		m_graphics_state.clear(rsx::pipeline_state::scissor_config_state_dirty | rsx::pipeline_state::scissor_setup_clipped);

		u16 x1, x2, y1, y2;

		u16 scissor_x = rsx::method_registers.scissor_origin_x();
		u16 scissor_w = rsx::method_registers.scissor_width();
		u16 scissor_y = rsx::method_registers.scissor_origin_y();
		u16 scissor_h = rsx::method_registers.scissor_height();

		if (clip_viewport)
		{
			u16 raster_x = rsx::method_registers.viewport_origin_x();
			u16 raster_w = rsx::method_registers.viewport_width();
			u16 raster_y = rsx::method_registers.viewport_origin_y();
			u16 raster_h = rsx::method_registers.viewport_height();

			// Get the minimum area between these two
			x1 = std::max(scissor_x, raster_x);
			y1 = std::max(scissor_y, raster_y);
			x2 = std::min(scissor_x + scissor_w, raster_x + raster_w);
			y2 = std::min(scissor_y + scissor_h, raster_y + raster_h);

			m_graphics_state |= rsx::pipeline_state::scissor_setup_clipped;
		}
		else
		{
			x1 = scissor_x;
			x2 = scissor_x + scissor_w;
			y1 = scissor_y;
			y2 = scissor_y + scissor_h;
		}

		if (x2 <= x1 ||
			y2 <= y1 ||
			x1 >= rsx::method_registers.window_clip_horizontal() ||
			y1 >= rsx::method_registers.window_clip_vertical())
		{
			m_graphics_state |= rsx::pipeline_state::scissor_setup_invalid;
			m_graphics_state.clear(rsx::rtt_config_valid);
			return false;
		}

		if (m_graphics_state & rsx::pipeline_state::scissor_setup_invalid)
		{
			m_graphics_state.clear(rsx::pipeline_state::scissor_setup_invalid);
			m_graphics_state.set(rsx::rtt_config_valid);
		}

		std::tie(region.x1, region.y1) = rsx::apply_resolution_scale<false>(resolution_scaling_config, x1, y1, m_framebuffer_layout.width, m_framebuffer_layout.height);
		std::tie(region.x2, region.y2) = rsx::apply_resolution_scale<true>(resolution_scaling_config, x2, y2, m_framebuffer_layout.width, m_framebuffer_layout.height);

		return true;
	}

	rsx::flags32_t thread::get_fragment_program_export_config()
	{
		u32 expected_ctrl = 0;

		// Programmable blending
		if (backend_config.supports_programmable_blending)
		{
			expected_ctrl = current_fragment_program.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING;

			if (m_graphics_state.test(rsx::pipeline_config_dirty))
			{
				evaluate_programmable_blending_state(m_ctx, expected_ctrl);
			}
		}

		// Resolve MSAA here if needed
		if (!!(expected_ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING) &&
			REGS(m_ctx)->surface_antialias() != rsx::surface_antialiasing::center_1_sample &&
			backend_config.supports_hw_msaa)
		{
			expected_ctrl |= RSX_SHADER_CONTROL_ROP_MULTISAMPLED;
		}

		// Depth compare
		if (!g_cfg.video.emulate_depth_compare) [[ likely ]]
		{
			return expected_ctrl;
		}

		if (REGS(m_ctx)->current_draw_clause.classify_mode() != primitive_class::polygon)
		{
			return expected_ctrl;
		}

		if (m_framebuffer_layout.zeta_address &&
			REGS(m_ctx)->depth_test_enabled() &&
			REGS(m_ctx)->depth_func() == rsx::comparison_function::equal)
		{
			expected_ctrl |= RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE;
		}

		// Resolve MSAA here if needed
		if ((expected_ctrl & (RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE | RSX_SHADER_CONTROL_ROP_MULTISAMPLED)) == RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE &&
			REGS(m_ctx)->surface_antialias() != rsx::surface_antialiasing::center_1_sample &&
			backend_config.supports_hw_msaa)
		{
			expected_ctrl |= RSX_SHADER_CONTROL_ROP_MULTISAMPLED;
		}

		return expected_ctrl;
	}

	void thread::prefetch_fragment_program()
	{
		if (!m_graphics_state.test(rsx::pipeline_state::fragment_program_ucode_dirty))
		{
			return;
		}

		m_graphics_state.clear(rsx::pipeline_state::fragment_program_ucode_dirty);

		// Request for update of fragment constants if the program block is invalidated
		m_graphics_state |= rsx::pipeline_state::fragment_constants_dirty;

		const auto [program_offset, program_location] = method_registers.shader_program_address();
		const auto prev_textures_reference_mask = current_fp_metadata.referenced_textures_mask;

		auto data_ptr = vm::base(rsx::get_address(program_offset, program_location));
		current_fp_metadata = program_hash_util::fragment_program_utils::analyse_fragment_program(data_ptr);

		current_fragment_program.data = (static_cast<u8*>(data_ptr) + current_fp_metadata.program_start_offset);
		current_fragment_program.offset = program_offset + current_fp_metadata.program_start_offset;
		current_fragment_program.ucode_length = current_fp_metadata.program_ucode_length;
		current_fragment_program.total_length = current_fp_metadata.program_ucode_length + current_fp_metadata.program_start_offset;
		current_fragment_program.texture_state.import(current_fp_texture_state, current_fp_metadata.referenced_textures_mask);
		current_fragment_program.valid = true;

		if (!m_graphics_state.test(rsx::pipeline_state::fragment_program_state_dirty))
		{
			// Verify current texture state is valid
			for (u32 textures_ref = current_fp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
			{
				if (!(textures_ref & 1)) continue;

				if (m_textures_dirty[i])
				{
					m_graphics_state |= rsx::pipeline_state::fragment_program_state_dirty;
					break;
				}
			}
		}

		if (!m_graphics_state.test(rsx::pipeline_state::fragment_program_state_dirty) &&
			(prev_textures_reference_mask != current_fp_metadata.referenced_textures_mask))
		{
			// If different textures are used, upload their coefficients.
			// The texture parameters transfer routine is optimized and only writes data for textures consumed by the ucode.
			m_graphics_state |= rsx::pipeline_state::fragment_texture_state_dirty;
		}
	}

	void thread::prefetch_vertex_program()
	{
		if (!m_graphics_state.test(rsx::pipeline_state::vertex_program_ucode_dirty))
		{
			return;
		}

		m_graphics_state.clear(rsx::pipeline_state::vertex_program_ucode_dirty);

		// Reload transform constants unconditionally for now
		m_graphics_state |= rsx::pipeline_state::transform_constants_dirty;

		const u32 transform_program_start = rsx::method_registers.transform_program_start();
		current_vertex_program.data.reserve(512 * 4);
		current_vertex_program.jump_table.clear();

		current_vp_metadata = program_hash_util::vertex_program_utils::analyse_vertex_program
		(
			method_registers.transform_program.data(),  // Input raw block
			transform_program_start,                    // Address of entry point
			current_vertex_program                      // [out] Program object
		);

		current_vertex_program.texture_state.import(current_vp_texture_state, current_vp_metadata.referenced_textures_mask);

		if (!m_graphics_state.test(rsx::pipeline_state::vertex_program_state_dirty))
		{
			// Verify current texture state is valid
			for (u32 textures_ref = current_vp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
			{
				if (!(textures_ref & 1)) continue;

				if (m_vertex_textures_dirty[i])
				{
					m_graphics_state |= rsx::pipeline_state::vertex_program_state_dirty;
					break;
				}
			}
		}
	}

	void thread::analyse_current_rsx_pipeline()
	{
		m_program_cache_hint.invalidate(m_graphics_state.load());

		if (u32 export_ctrl = get_fragment_program_export_config();
			(current_fragment_program.ctrl & fs_export_config_mask) != export_ctrl)
		{
			// Update control bits for immediate consumers
			current_fragment_program.ctrl &= ~fs_export_config_mask;
			current_fragment_program.ctrl |= export_ctrl;

			// Signal backend to reload pipeline
			m_graphics_state.set(rsx::pipeline_state::fragment_program_state_dirty);
		}

		prefetch_vertex_program();
		prefetch_fragment_program();
	}

	void thread::get_current_vertex_program(const std::array<std::unique_ptr<rsx::sampled_image_descriptor_base>, rsx::limits::vertex_textures_count>& sampler_descriptors)
	{
		if (m_graphics_state.test(rsx::pipeline_state::xform_instancing_state_dirty))
		{
			current_vertex_program.ctrl &= ~RSX_SHADER_CONTROL_INSTANCED_CONSTANTS;
			if (rsx::method_registers.current_draw_clause.is_trivial_instanced_draw)
			{
				current_vertex_program.ctrl |= RSX_SHADER_CONTROL_INSTANCED_CONSTANTS;
			}

			m_graphics_state.clear(rsx::pipeline_state::xform_instancing_state_dirty);

			// Emit invalidate here in case ucode is actually clean
			m_program_cache_hint.invalidate_vertex_program(current_vertex_program);
		}

		if (!m_graphics_state.test(rsx::pipeline_state::vertex_program_dirty))
		{
			return;
		}

		ensure(!m_graphics_state.test(rsx::pipeline_state::vertex_program_ucode_dirty));
		current_vertex_program.output_mask = rsx::method_registers.vertex_attrib_output_mask();

		current_vertex_program.ctrl &= ~RSX_SHADER_CONTROL_FLAT_SHADING;
		if (rsx::method_registers.shade_mode() == rsx::shading_mode::flat &&
			backend_config.supports_last_provoking_vertex)
		{
			current_vertex_program.ctrl |= RSX_SHADER_CONTROL_FLAT_SHADING;
		}

		for (u32 textures_ref = current_vp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
		{
			if (!(textures_ref & 1)) continue;

			const auto &tex = rsx::method_registers.vertex_textures[i];
			if (tex.enabled() && (current_vp_metadata.referenced_textures_mask & (1 << i)))
			{
				current_vp_texture_state.clear(i);
				current_vp_texture_state.set_dimension(sampler_descriptors[i]->image_type, i);

				if (backend_config.supports_hw_msaa &&
					sampler_descriptors[i]->samples > 1)
				{
					current_vp_texture_state.multisampled_textures |= (1 << i);
				}
			}
		}

		current_vertex_program.texture_state.import(current_vp_texture_state, current_vp_metadata.referenced_textures_mask);

		m_program_cache_hint.invalidate_vertex_program(current_vertex_program);
	}

	void thread::get_current_fragment_program(const std::array<std::unique_ptr<rsx::sampled_image_descriptor_base>, rsx::limits::fragment_textures_count>& sampler_descriptors)
	{
		if (!m_graphics_state.test(rsx::pipeline_state::fragment_program_dirty))
		{
			return;
		}

		ensure(!m_graphics_state.test(rsx::pipeline_state::fragment_program_ucode_dirty));

		m_graphics_state.clear(rsx::pipeline_state::fragment_program_dirty);

		current_fragment_program.ctrl &= fs_export_config_mask;
		current_fragment_program.ctrl |= REGS(m_ctx)->shader_control() & (CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS | CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT | RSX_SHADER_CONTROL_USES_KIL);
		current_fragment_program.texcoord_control_mask = REGS(m_ctx)->texcoord_control_mask();
		current_fragment_program.two_sided_lighting = REGS(m_ctx)->two_side_light_en();
		current_fragment_program.mrt_buffers_count = rsx::utility::get_mrt_buffers_count(REGS(m_ctx)->surface_color_target());

		if (REGS(m_ctx)->shade_mode() == rsx::shading_mode::flat &&
			backend_config.supports_last_provoking_vertex)
		{
			current_fragment_program.ctrl |= RSX_SHADER_CONTROL_FLAT_SHADING;
		}

		if (REGS(m_ctx)->current_draw_clause.classify_mode() == primitive_class::polygon)
		{
			if (!backend_config.supports_normalized_barycentrics)
			{
				current_fragment_program.ctrl |= RSX_SHADER_CONTROL_ATTRIBUTE_INTERPOLATION;
			}

			if (REGS(m_ctx)->polygon_stipple_enabled())
			{
				current_fragment_program.ctrl |= RSX_SHADER_CONTROL_POLYGON_STIPPLE;
			}
		}
		else if (REGS(m_ctx)->point_sprite_enabled() &&
			REGS(m_ctx)->current_draw_clause.primitive == primitive_type::points)
		{
			// Set high word of the control mask to store point sprite control
			current_fragment_program.texcoord_control_mask |= u32(REGS(m_ctx)->point_sprite_control_mask()) << 16;
		}

		if (REGS(m_ctx)->alpha_test_enabled())
		{
			current_fragment_program.ctrl |= RSX_SHADER_CONTROL_ALPHA_TEST;
		}

		if (REGS(m_ctx)->msaa_alpha_to_coverage_enabled())
		{
			const bool is_multiple_samples = REGS(m_ctx)->surface_antialias() != rsx::surface_antialiasing::center_1_sample;
			if (!backend_config.supports_hw_a2c || (!is_multiple_samples && !backend_config.supports_hw_a2c_1spp))
			{
				// Emulation required
				current_fragment_program.ctrl |= RSX_SHADER_CONTROL_ALPHA_TO_COVERAGE;
			}
		}

		// Check if framebuffer is actually an XRGB format and not a WZYX format
		switch (REGS(m_ctx)->surface_color())
		{
		case rsx::surface_color_format::w16z16y16x16:
		case rsx::surface_color_format::w32z32y32x32:
		case rsx::surface_color_format::x32:
			// These behave very differently from "normal" formats.
			break;
		default:
			// Integer framebuffer formats. These can support sRGB output as well as some special rules for output quantization.
			current_fragment_program.ctrl |= RSX_SHADER_CONTROL_8BIT_FRAMEBUFFER;
			if (!(current_fragment_program.ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) && // Cannot output sRGB from 32-bit registers
				REGS(m_ctx)->framebuffer_srgb_enabled())
			{
				current_fragment_program.ctrl |= RSX_SHADER_CONTROL_SRGB_FRAMEBUFFER;
			}
			if (REGS(m_ctx)->surface_is_swizzle_remapped())
			{
				current_fragment_program.ctrl |= RSX_SHADER_CONTROL_ROP_OUTPUT_REMAP;
			}
			break;
		}

		const bool zeta_was_cyclic = m_graphics_state & rsx::zeta_address_is_cyclic;
		m_graphics_state.clear(rsx::zeta_address_is_cyclic);

		for (u32 textures_ref = current_fp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
		{
			if (!(textures_ref & 1)) continue;

			auto &tex = REGS(m_ctx)->fragment_textures[i];
			current_fp_texture_state.clear(i);

			if (!tex.enabled() || sampler_descriptors[i]->format_class == RSX_FORMAT_CLASS_UNDEFINED)
			{
				continue;
			}

			std::memcpy(
				current_fragment_program.texture_params[i].scale,
				sampler_descriptors[i]->texcoord_xform.scale,
				sizeof(sampler_descriptors[i]->texcoord_xform.scale) * 2); // Copy scale and bias together

			current_fragment_program.texture_params[i].remap = tex.remap();

			m_graphics_state |= rsx::pipeline_state::fragment_texture_state_dirty;

			u32 texture_control = 0;
			current_fp_texture_state.set_dimension(sampler_descriptors[i]->image_type, i);

			if (sampler_descriptors[i]->texcoord_xform.clamp)
			{
				std::memcpy(
					current_fragment_program.texture_params[i].clamp_min,
					sampler_descriptors[i]->texcoord_xform.clamp_min,
					sizeof(sampler_descriptors[i]->texcoord_xform.clamp_min) * 2); // Copy clamp_min and clamp_max together

				texture_control |= (1 << rsx::texture_control_bits::CLAMP_TEXCOORDS_BIT);
			}

			if (tex.alpha_kill_enabled())
			{
				//alphakill can be ignored unless a valid comparison function is set
				texture_control |= (1 << texture_control_bits::ALPHAKILL);
				current_fragment_program.ctrl |= RSX_SHADER_CONTROL_TEXTURE_ALPHA_KILL;
			}

			//const u32 texaddr = rsx::get_address(tex.offset(), tex.location());
			const u32 raw_format = tex.format();
			const u32 format = raw_format & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);

			if (raw_format & CELL_GCM_TEXTURE_UN)
			{
				if (tex.min_filter() == rsx::texture_minify_filter::nearest ||
					tex.mag_filter() == rsx::texture_magnify_filter::nearest)
				{
					// Subpixel offset so that (X + bias) * scale will round correctly.
					// This is done to work around fdiv precision issues in some GPUs (NVIDIA)
					// We apply the simplification where (x + bias) * z = xz + zbias here.
					constexpr auto subpixel_bias = 0.01f;
					current_fragment_program.texture_params[i].bias[0] += (subpixel_bias * current_fragment_program.texture_params[i].scale[0]);
					current_fragment_program.texture_params[i].bias[1] += (subpixel_bias * current_fragment_program.texture_params[i].scale[1]);
					current_fragment_program.texture_params[i].bias[2] += (subpixel_bias * current_fragment_program.texture_params[i].scale[2]);
				}
			}

			if (backend_config.supports_hw_msaa && sampler_descriptors[i]->samples > 1)
			{
				current_fp_texture_state.multisampled_textures |= (1 << i);
				texture_control |= (static_cast<u32>(tex.zfunc()) << texture_control_bits::DEPTH_COMPARE_OP);
				texture_control |= (static_cast<u32>(tex.mag_filter() != rsx::texture_magnify_filter::nearest) << texture_control_bits::FILTERED_MAG);
				texture_control |= (static_cast<u32>(tex.min_filter() != rsx::texture_minify_filter::nearest) << texture_control_bits::FILTERED_MIN);
				texture_control |= (((tex.format() & CELL_GCM_TEXTURE_UN) >> 6) << texture_control_bits::UNNORMALIZED_COORDS);

				if (rsx::is_texcoord_wrapping_mode(tex.wrap_s()))
				{
					texture_control |= (1 << texture_control_bits::WRAP_S);
				}

				if (rsx::is_texcoord_wrapping_mode(tex.wrap_t()))
				{
					texture_control |= (1 << texture_control_bits::WRAP_T);
				}

				if (rsx::is_texcoord_wrapping_mode(tex.wrap_r()))
				{
					texture_control |= (1 << texture_control_bits::WRAP_R);
				}
			}

			if (sampler_descriptors[i]->format_class != RSX_FORMAT_CLASS_COLOR)
			{
				switch (sampler_descriptors[i]->format_class)
				{
				case RSX_FORMAT_CLASS_DEPTH16_FLOAT:
				case RSX_FORMAT_CLASS_DEPTH24_FLOAT_X8_PACK32:
					texture_control |= (1 << texture_control_bits::DEPTH_FLOAT);
					break;
				default:
					break;
				}

				switch (format)
				{
				case CELL_GCM_TEXTURE_A8R8G8B8:
				case CELL_GCM_TEXTURE_D8R8G8B8:
				{
					// Emulate bitcast in shader
					current_fp_texture_state.redirected_textures |= (1 << i);
					const auto float_en = (sampler_descriptors[i]->format_class == RSX_FORMAT_CLASS_DEPTH24_FLOAT_X8_PACK32)? 1 : 0;
					texture_control |= (float_en << texture_control_bits::DEPTH_FLOAT);
					break;
				}
				case CELL_GCM_TEXTURE_X16:
				{
					// A simple way to quickly read DEPTH16 data without shadow comparison
					break;
				}
				case CELL_GCM_TEXTURE_DEPTH16:
				case CELL_GCM_TEXTURE_DEPTH24_D8:
				case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
				case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
				{
					// Natively supported Z formats with shadow comparison feature
					const auto compare_mode = tex.zfunc();
					if (!tex.alpha_kill_enabled() &&
						compare_mode < rsx::comparison_function::always &&
						compare_mode > rsx::comparison_function::never)
					{
						current_fp_texture_state.shadow_textures |= (1 << i);
					}
					break;
				}
				default:
					rsx_log.error("Depth texture bound to pipeline with unexpected format 0x%X", format);
				}

				if (sampler_descriptors[i]->is_cyclic_reference &&
					m_framebuffer_layout.zeta_address != 0 &&
					!g_cfg.video.strict_rendering_mode &&
					g_cfg.video.shader_precision != gpu_preset_level::low)
				{
					m_graphics_state |= rsx::zeta_address_is_cyclic;

					if (!(current_fragment_program.ctrl & (CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT | RSX_SHADER_CONTROL_META_USES_DISCARD | RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE)) &&
						m_framebuffer_layout.zeta_write_enabled)
					{
						current_fragment_program.ctrl |= RSX_SHADER_CONTROL_DISABLE_EARLY_Z;
					}
				}
			}
			else if (!backend_config.supports_hw_renormalization /* &&
				tex.min_filter() == rsx::texture_minify_filter::nearest &&
				tex.mag_filter() == rsx::texture_magnify_filter::nearest*/)
			{
				// FIXME: This check should only apply to point-sampled textures. However, it severely regresses some games (id tech 5).
				// This is because even when filtering is active, the error from the PS3 texture expansion still applies.
				// A proper fix is to expand these formats into BGRA8 when high texture precision is required. That requires different GUI settings and inflation shaders, so it will be handled separately.

				switch (format)
				{
				case CELL_GCM_TEXTURE_A1R5G5B5:
				case CELL_GCM_TEXTURE_A4R4G4B4:
				case CELL_GCM_TEXTURE_D1R5G5B5:
				case CELL_GCM_TEXTURE_R5G5B5A1:
				case CELL_GCM_TEXTURE_R5G6B5:
				case CELL_GCM_TEXTURE_R6G5B5:
					texture_control |= (1 << texture_control_bits::RENORMALIZE);
					current_fragment_program.ctrl |= RSX_SHADER_CONTROL_TEXTURE_FORMAT_CONVERT;
					break;
				default:
					break;
				}
			}

			if (const auto& format_ex = sampler_descriptors[i]->format_ex; format_ex.features != 0)
			{
				texture_control |= format_ex.texel_remap_control;
				texture_control |= format_ex.features << texture_control_bits::FORMAT_FEATURES_OFFSET;

				if (format_ex.texel_remap_control)
				{
					current_fragment_program.ctrl |= RSX_SHADER_CONTROL_TEXTURE_FORMAT_CONVERT;
				}

				if (current_fp_metadata.bx2_texture_reads_mask)
				{
					current_fragment_program.ctrl |= RSX_SHADER_CONTROL_TEXTURE_FORMAT_CONVERT;

					const u32 remap_hi = tex.decoded_remap().shuffle_mask_bits(0xFu);
					current_fragment_program.texture_params[i].remap &= ~(0xFu << 16u);
					current_fragment_program.texture_params[i].remap |= (remap_hi << 16u);
				}
			}

			current_fragment_program.texture_params[i].control = texture_control;
		}

		// Update texture configuration
		current_fragment_program.texture_state.import(current_fp_texture_state, current_fp_metadata.referenced_textures_mask);

		//Sanity checks
		if (current_fragment_program.ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT)
		{
			//Check that the depth stage is not disabled
			if (!REGS(m_ctx)->depth_test_enabled())
			{
				rsx_log.trace("FS exports depth component but depth test is disabled (INVALID_OPERATION)");
			}
		}

		m_program_cache_hint.invalidate_fragment_program(current_fragment_program);

		if (zeta_was_cyclic && zeta_was_cyclic != m_graphics_state.test(rsx::zeta_address_is_cyclic))
		{
			// Forced "fall-out" barrier. This is a special case for Z buffers because they can be cyclic without writes.
			// That condition can cause early-Z in a later call to introduce data hazard in previous cyclic draws.
			m_graphics_state |= rsx::zeta_address_cyclic_barrier;
		}
	}

	bool thread::invalidate_fragment_program(u32 dst_dma, u32 dst_offset, u32 size)
	{
		if (!current_fragment_program.total_length)
		{
			// No shader loaded
			return false;
		}

		const auto [shader_offset, shader_dma] = rsx::method_registers.shader_program_address();
		if ((dst_dma & CELL_GCM_LOCATION_MAIN) != shader_dma)
		{
			// Shader not loaded in XDR memory
			return false;
		}

		const auto current_fragment_shader_range = address_range32::start_length(shader_offset, current_fragment_program.total_length);
		if (!current_fragment_shader_range.overlaps(address_range32::start_length(dst_offset, size)))
		{
			// No range overlap
			return false;
		}

		// Data overlaps. Force ucode reload.
		m_graphics_state |= rsx::pipeline_state::fragment_program_ucode_dirty;
		return true;
	}

	void thread::reset()
	{
		rsx::method_registers.reset();
		check_zcull_status(false);
		nv4097::set_render_mode(m_ctx, 0, method_registers.registers[NV4097_SET_RENDER_ENABLE]);
		m_graphics_state |= pipeline_state::all_dirty;
	}

	void thread::init(u32 ctrlAddress)
	{
		dma_address = ctrlAddress;
		ctrl = vm::_ptr<RsxDmaControl>(ctrlAddress);
		flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE;
		fifo_ret_addr = RSX_CALL_STACK_EMPTY;

		vm::write32(device_addr + 0x30, 1);
		std::memset(display_buffers, 0, sizeof(display_buffers));

		rsx_thread_running = true;
	}

	std::pair<u32, u32> thread::calculate_memory_requirements(const vertex_input_layout& layout, u32 first_vertex, u32 vertex_count)
	{
		u32 persistent_memory_size = 0;
		u32 volatile_memory_size = 0;

		volatile_memory_size += ::size32(layout.referenced_registers) * 16u;

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array)
		{
			for (const auto &block : layout.interleaved_blocks)
			{
				volatile_memory_size += block->attribute_stride * vertex_count;
			}
		}
		else
		{
			//NOTE: Immediate commands can be index array only or both index array and vertex data
			//Check both - but only check volatile blocks if immediate_draw flag is set
			if (rsx::method_registers.current_draw_clause.is_immediate_draw)
			{
				for (const auto &info : layout.volatile_blocks)
				{
					volatile_memory_size += info.second;
				}
			}

			persistent_memory_size = layout.calculate_interleaved_memory_requirements(first_vertex, vertex_count);
		}

		return std::make_pair(persistent_memory_size, volatile_memory_size);
	}

	void thread::flip(const display_flip_info_t& info)
	{
		m_eng_interrupt_mask.clear(rsx::display_interrupt);

		if (async_flip_requested & flip_request::any)
		{
			// Deferred flip
			if (info.emu_flip)
			{
				async_flip_requested.clear(flip_request::emu_requested);
			}
			else
			{
				async_flip_requested.clear(flip_request::native_ui);
			}
		}

		if (info.emu_flip)
		{
			performance_counters.sampled_frames++;

			if (m_pause_after_x_flips && m_pause_after_x_flips-- == 1)
			{
				Emu.Pause();
			}
		}

		last_host_flip_timestamp = get_system_time();
	}

	void thread::check_zcull_status(bool framebuffer_swap)
	{
		const bool zcull_rendering_enabled = !!method_registers.registers[NV4097_SET_ZCULL_EN];
		const bool zcull_stats_enabled = !!method_registers.registers[NV4097_SET_ZCULL_STATS_ENABLE];
		const bool zcull_pixel_cnt_enabled = !!method_registers.registers[NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE];

		if (framebuffer_swap)
		{
			zcull_surface_active = false;
			const u32 zeta_address = m_depth_surface_info.address;

			if (zeta_address)
			{
				//Find zeta address in bound zculls
				for (const auto& zcull : zculls)
				{
					if (zcull.bound &&
						rsx::to_surface_depth_format(zcull.zFormat) == m_depth_surface_info.depth_format &&
						rsx::to_surface_antialiasing(zcull.aaFormat) == rsx::method_registers.surface_antialias())
					{
						const u32 rsx_address = rsx::get_address(zcull.offset, CELL_GCM_LOCATION_LOCAL);
						if (rsx_address == zeta_address)
						{
							zcull_surface_active = true;
							break;
						}
					}
				}
			}
		}

		zcull_ctrl->set_enabled(this, zcull_rendering_enabled);
		zcull_ctrl->set_status(this, zcull_surface_active, zcull_pixel_cnt_enabled, zcull_stats_enabled);
	}

	void thread::clear_zcull_stats(u32 type)
	{
		zcull_ctrl->clear(this, type);
	}

	void thread::get_zcull_stats(u32 type, vm::addr_t sink)
	{
		u32 value = 0;
		if (!g_cfg.video.disable_zcull_queries)
		{
			switch (type)
			{
			case CELL_GCM_ZPASS_PIXEL_CNT:
			case CELL_GCM_ZCULL_STATS:
			case CELL_GCM_ZCULL_STATS1:
			case CELL_GCM_ZCULL_STATS2:
			case CELL_GCM_ZCULL_STATS3:
			{
				zcull_ctrl->read_report(this, sink, type);
				return;
			}
			default:
				rsx_log.error("Unknown zcull stat type %d", type);
				break;
			}
		}

#ifdef RPCS3_PS4
		ps4_report_ring_note(ps4_report_get, type, sink, fifo_ctrl ? fifo_ctrl->get_pos() : 0, false, zcull_ctrl->ps4_state());

		// Earlier draws read their vertex sources before the guest can see this report
		ps4_copy_worker_barrier();

		{
			rsx::reservation_lock<true> lock(sink, 16);
			vm::_ptr<atomic_t<CellGcmReportData>>(sink)->store({timestamp(), value, 0});
		}

		ps4_report_ring_touched(sink, ps4_rsx_touch_reservations(sink, 16, true));
#else
		rsx::reservation_lock<true> lock(sink, 16);
		vm::_ptr<atomic_t<CellGcmReportData>>(sink)->store({timestamp(), value, 0});
#endif
	}

	u32 thread::copy_zcull_stats(u32 memory_range_start, u32 memory_range, u32 destination)
	{
		return zcull_ctrl->copy_reports_to(memory_range_start, memory_range, destination);
	}

	void thread::enable_conditional_rendering(vm::addr_t ref)
	{
		cond_render_ctrl.enable_conditional_render(this, ref);

		auto result = zcull_ctrl->find_query(ref, true);
		if (result.found)
		{
			if (!result.queries.empty())
			{
				cond_render_ctrl.set_eval_sources(result.queries);
				sync_hint(FIFO::interrupt_hint::conditional_render_eval, { .query = cond_render_ctrl.eval_sources.front(), .address = ref });
			}
			else
			{
				bool failed = (result.raw_zpass_result == 0);
				cond_render_ctrl.set_eval_result(this, failed);
			}
		}
		else
		{
			cond_render_ctrl.eval_result(this);
		}
	}

	void thread::disable_conditional_rendering()
	{
		cond_render_ctrl.disable_conditional_render(this);
	}

	void thread::begin_conditional_rendering(const std::vector<reports::occlusion_query_info*>& /*sources*/)
	{
		cond_render_ctrl.hw_cond_active = true;
		cond_render_ctrl.eval_sources.clear();
	}

	void thread::end_conditional_rendering()
	{
		cond_render_ctrl.hw_cond_active = false;
	}

	void thread::sync()
	{
		m_eng_interrupt_mask.clear(rsx::pipe_flush_interrupt);

		mm_flush();

		if (zcull_ctrl->has_pending())
		{
			zcull_ctrl->sync(this);
		}

		// Fragment constants may have been updated
		m_graphics_state |= rsx::pipeline_state::fragment_constants_dirty;

#ifdef RPCS3_PS4
		// 0.37: a full drain (USER_COMMAND, REF, notify, pipe-flushing labels) also finishes the vertex copies queued on
		// the RSX copy worker: a guest woken by it never sees earlier draws still reading their sources
		ps4_copy_worker_barrier();
#endif

		// DMA sync; if you need this, don't use MTRSX
		// g_fxo->get<rsx::dma_manager>().sync();

		//TODO: On sync every sub-unit should finish any pending tasks
		//Might cause zcull lockup due to zombie 'unclaimed reports' which are not forcefully removed currently
		//ensure(async_tasks_pending.load() == 0);
	}

	void thread::sync_hint(FIFO::interrupt_hint /*hint*/, rsx::reports::sync_hint_payload_t payload)
	{
		zcull_ctrl->on_sync_hint(payload);
	}

	bool thread::is_fifo_idle() const
	{
		return ctrl == nullptr || ctrl->get == (ctrl->put & ~3);
	}

	void thread::flush_fifo()
	{
		// Make sure GET value is exposed before sync points
		fifo_ctrl->sync_get();
		fifo_ctrl->invalidate_cache();
	}

	std::pair<u32, u32> thread::try_get_pc_of_x_cmds_backwards(s32 count, u32 get) const
	{
		if (!ctrl || state & cpu_flag::exit)
		{
			return {0, umax};
		}

		if (!count)
		{
			return {0, get};
		}

		u32 true_get = ctrl->get;
		u32 start = last_known_code_start;

		RSXDisAsm disasm(cpu_disasm_mode::survey_cmd_size, vm::g_sudo_addr, 0, this);

		std::vector<u32> pcs_of_valid_cmds;

		if (get > start)
		{
			pcs_of_valid_cmds.reserve(std::min<u32>((get - start) / 16, 0x4000)); // Rough estimation of final array size
		}

		auto probe_code_region = [&](u32 probe_start) -> std::pair<u32, u32>
		{
			if (probe_start > get)
			{
				return {0, get};
			}

			pcs_of_valid_cmds.clear();
			pcs_of_valid_cmds.push_back(probe_start);

			usz index_of_get = umax;
			usz until = umax;

			while (pcs_of_valid_cmds.size() < until)
			{
				if (u32 advance = disasm.disasm(pcs_of_valid_cmds.back()))
				{
					pcs_of_valid_cmds.push_back(utils::add_saturate<u32>(pcs_of_valid_cmds.back(), advance));
				}
				else
				{
					break;
				}

				if (index_of_get == umax && pcs_of_valid_cmds.back() >= get)
				{
					index_of_get = pcs_of_valid_cmds.size() - 1;
					until = index_of_get + 1;

					if (count < 0 && pcs_of_valid_cmds.back() == get)
					{
						until -= count;
					}
				}
			}

			if (index_of_get == umax || pcs_of_valid_cmds[index_of_get] != get)
			{
				return {0, get};
			}

			if (count < 0)
			{
				const u32 found_cmds_count = static_cast<u32>(std::min<s64>(-count, pcs_of_valid_cmds.size() - 1LL - index_of_get));

				return {found_cmds_count, pcs_of_valid_cmds[index_of_get + found_cmds_count]};
			}

			const u32 found_cmds_count = std::min<u32>(count, ::size32(pcs_of_valid_cmds) - 1);

			return {found_cmds_count, *(pcs_of_valid_cmds.end() - 1 - found_cmds_count)};
		};

		auto pair = probe_code_region(start);

		if (!pair.first)
		{
			pair = probe_code_region(true_get);
		}

		return pair;
	}

	void thread::recover_fifo(std::source_location src_loc)
	{
		bool kill_itself = g_cfg.core.rsx_fifo_accuracy == rsx_fifo_mode::as_ps3;

		const u64 current_time = get_system_time();

		if (recovered_fifo_cmds_history.size() == 20u)
		{
			const auto cmd_info = recovered_fifo_cmds_history.front();

			// Check timestamp of last tracked cmd
			// Shorten the range of forbidden difference if driver wake-up delay is used
			if (current_time - cmd_info.timestamp < 2'000'000u - std::min<u32>(g_cfg.video.driver_wakeup_delay * 700, 1'400'000))
			{
				// Probably hopeless
				kill_itself = true;
			}

			// Erase the last command from history, keep the size of the queue the same
			recovered_fifo_cmds_history.pop();
		}

		if (kill_itself)
		{
#ifdef RPCS3_PS4
			// The fatal record names the FIFO state and the guest's RSX setup (IO map, sys_rsx calls, SDK version)
			const std::string ps4_state = ps4_fifo_state_text();
			const std::string ps4_line = "RSX: Dead FIFO: " + ps4_state;
			rsx_log.error("PS4 %s", ps4_line);
			ps4_startup_checkpoint(ps4_line.c_str());

			if (s_ps4_dead_fifo_stop.observe())
			{
				// dead-fifo-stop.flag: the frontend stops the game as for a renderer stop request. This thread stays
				// alive (pause, stop) after the reset below, but run_FIFO fetches nothing more.
				ps4_fatal_note("RSX: dead FIFO; stopping the game: ", ps4_state.substr(0, 120).c_str());
				utils::ps4::vram::request_stop(utils::ps4::vram::stop_cause::rsx_dead_fifo, "Dead FIFO");
				m_ps4_fifo_dead = true;
			}
			else
#endif
			{
				fmt::throw_exception("Dead FIFO commands queue state has been detected!"
					"\nTry increasing \"Driver Wake-Up Delay\" setting or setting \"RSX FIFO Accuracy\" to \"%s\", both in Advanced settings. Called from %s", std::min<rsx_fifo_mode>(rsx_fifo_mode{static_cast<u32>(g_cfg.core.rsx_fifo_accuracy.get()) + 1}, rsx_fifo_mode::atomic_ordered), src_loc);
			}
		}

		// Error. Should reset the queue
		fifo_ctrl->set_get(restore_point);
		fifo_ret_addr = saved_fifo_ret;
		std::this_thread::sleep_for(2ms);
		fifo_ctrl->abort();

		if (std::exchange(in_begin_end, false) && !rsx::method_registers.current_draw_clause.empty())
		{
			execute_nop_draw();
			rsx::thread::end();
		}

		recovered_fifo_cmds_history.push({fifo_ctrl->last_cmd(), current_time});
	}

	void thread::dump_misc(std::string& ret, std::any& custom_data) const
	{
		cpu_thread::dump_misc(ret, custom_data);

		const auto flags = +state;

		if (is_paused(flags) && flags & cpu_flag::wait)
		{
			fmt::append(ret, "\nFragment Program Hash: %X.fp", current_fragment_program.get_data() ? program_hash_util::fragment_program_utils::get_fragment_program_ucode_hash(current_fragment_program) : 0);
			fmt::append(ret, "\nVertex Program Hash: %X.vp", current_vertex_program.data.empty() ? 0 : program_hash_util::vertex_program_utils::get_vertex_program_ucode_hash(current_vertex_program));
		}
		else
		{
			fmt::append(ret, "\n");
		}
	}

	std::vector<std::pair<u32, u32>> thread::dump_callstack_list() const
	{
		std::vector<std::pair<u32, u32>> result;

		if (u32 addr = fifo_ret_addr; addr != RSX_CALL_STACK_EMPTY)
		{
			result.emplace_back(addr, 0);
		}

		return result;
	}

	void thread::fifo_wake_delay(u64 div)
	{
		// TODO: Nanoseconds accuracy
		u64 remaining = g_cfg.video.driver_wakeup_delay;

		if (!remaining)
		{
			return;
		}

		// Some cases do not need full delay
		remaining = utils::aligned_div(remaining, div);
		const u64 until = get_system_time() + remaining;

		while (true)
		{
#ifdef __linux__
			// NOTE: Assumption that timer initialization has succeeded
			constexpr u64 host_min_quantum = 10;
#else
			// Host scheduler quantum for windows (worst case)
			// NOTE: On ps3 this function has very high accuracy
			constexpr u64 host_min_quantum = 500;
#endif
			if (remaining >= host_min_quantum)
			{
#ifdef __linux__
				thread_ctrl::wait_for(remaining, false);
#else
				// Wait on multiple of min quantum for large durations to avoid overloading low thread cpus
				thread_ctrl::wait_for(remaining - (remaining % host_min_quantum), false);
#endif
			}
			// TODO: Determine best value for yield delay
			else if (remaining >= host_min_quantum / 2)
			{
				std::this_thread::yield();
			}
			else
			{
				busy_wait(100);
			}

			const u64 current = get_system_time();

			if (current >= until)
			{
				break;
			}

			remaining = until - current;
		}
	}

	u32 thread::get_fifo_cmd() const
	{
		// Last fifo cmd for logging and utility
		return fifo_ctrl->last_cmd();
	}

	void invalid_method(context*, u32, u32);

	void thread::dump_regs(std::string& result, std::any& /*custom_data*/) const
	{
		if (ctrl)
		{
			fmt::append(result, "FIFO: GET=0x%07x, PUT=0x%07x, REF=0x%08x\n", +ctrl->get, +ctrl->put, +ctrl->ref);
		}

		for (u32 i = 0; i < 1 << 14; i++)
		{
			if (rsx::methods[i] == &invalid_method)
			{
				continue;
			}

			switch (i)
			{
			case NV4097_NO_OPERATION:
			case NV4097_INVALIDATE_L2:
			case NV4097_INVALIDATE_VERTEX_FILE:
			case NV4097_INVALIDATE_VERTEX_CACHE_FILE:
			case NV4097_INVALIDATE_ZCULL:
			case NV4097_WAIT_FOR_IDLE:
			case NV4097_PM_TRIGGER:
			case NV4097_ZCULL_SYNC:
				continue;

			case NV308A_COLOR:
			{
				i = NV3089_SET_OBJECT;
				continue;
			}
			default:
			{
				break;
			}
			}

			fmt::append(result, "[%04x] ", i);
			ensure(rsx::get_pretty_printing_function(i))(result, i, method_registers.registers[i]);
			result += '\n';
		}
	}

	flags32_t thread::read_barrier(u32 memory_address, u32 memory_range, bool unconditional)
	{
		flags32_t zcull_flags = (unconditional)? reports::sync_none : reports::sync_defer_copy;
		return zcull_ctrl->read_barrier(this, memory_address, memory_range, zcull_flags);
	}

	void thread::notify_zcull_info_changed()
	{
		check_zcull_status(false);
	}

	void thread::on_notify_memory_mapped(u32 address, u32 size)
	{
		// In the case where an unmap is followed shortly after by a remap of the same address space
		// we must block until RSX has invalidated the memory
		// or lock m_mtx_task and do it ourselves

		if (!rsx_thread_running)
			return;

		reader_lock lock(m_mtx_task);

		const auto map_range = address_range32::start_length(address, size);

		if (!m_invalidated_memory_range.valid())
			return;

		if (m_invalidated_memory_range.overlaps(map_range))
		{
			lock.upgrade();
			handle_invalidated_memory_range();
		}
	}

	void thread::on_notify_pre_memory_unmapped(u32 address, u32 size, std::vector<std::pair<u64, u64>>& event_data)
	{
		// Always flush MM if memory mapping is going to change.
		rsx::mm_flush();

		if (rsx_thread_running && address < rsx::constants::local_mem_base)
		{
			// Each bit represents io entry to be unmapped
			u64 unmap_status[512 / 64]{};

			for (u32 ea = address >> 20, end = ea + (size >> 20); ea < end; ea++)
			{
				const u32 io = std::rotl<u32>(iomap_table.io[ea], 32 - 20);

				if (io + 1)
				{
					unmap_status[io / 64] |= 1ull << (io & 63);
					iomap_table.io[ea].release(-1);
					iomap_table.ea[io].release(-1);
				}
			}

			auto& cfg = g_fxo->get<gcm_config>();

			std::optional<std::unique_lock<shared_mutex>> hle_lock;

			for (u32 i = 0; i < std::size(unmap_status); i++)
			{
				// TODO: Check order when sending multiple events
				if (u64 to_unmap = unmap_status[i])
				{
					if (isHLE)
					{
						if (!hle_lock)
						{
							hle_lock = std::unique_lock{cfg.gcmio_mutex};
						}

						int bit = 0;

						while (to_unmap)
						{
							bit = (std::countr_zero<u64>(std::rotl<u64>(to_unmap, 0 - bit)) + bit);
							to_unmap &= ~(1ull << bit);

							constexpr u16 null_entry = 0xFFFF;
							const u32 ea = std::exchange(cfg.offsetTable.eaAddress[(i * 64 + bit)], null_entry);

							if (ea < (rsx::constants::local_mem_base >> 20))
							{
								cfg.offsetTable.eaAddress[ea] = null_entry;
							}
						}

						continue;
					}

					// Each 64 entries are grouped by a bit
					const u64 io_event = SYS_RSX_EVENT_UNMAPPED_BASE << i;
					event_data.emplace_back(io_event, to_unmap);
				}
			}

			if (hle_lock)
			{
				hle_lock->unlock();
			}

			// Pause RSX thread momentarily to handle unmapping
			eng_lock elock(this);

			// Queue up memory invalidation
			std::lock_guard lock(m_mtx_task);
			const bool existing_range_valid = m_invalidated_memory_range.valid();
			const auto unmap_range = address_range32::start_length(address, size);

			if (existing_range_valid && m_invalidated_memory_range.touches(unmap_range))
			{
				// Merge range-to-invalidate in case of consecutive unmaps
				m_invalidated_memory_range.set_min_max(unmap_range);
			}
			else
			{
				if (existing_range_valid)
				{
					// We can only delay consecutive unmaps.
					// Otherwise, to avoid VirtualProtect failures, we need to do the invalidation here
					handle_invalidated_memory_range();
				}

				m_invalidated_memory_range = unmap_range;
			}

			m_eng_interrupt_mask |= rsx::memory_config_interrupt;
		}
	}

	void thread::on_notify_post_memory_unmapped(u64 event_data1, u64 event_data2)
	{
		if (!isHLE)
		{
			send_event(0, event_data1, event_data2);
		}
	}

	// NOTE: m_mtx_task lock must be acquired before calling this method
	void thread::handle_invalidated_memory_range()
	{
		AUDIT(!m_mtx_task.is_free());
		m_eng_interrupt_mask.clear(rsx::memory_config_interrupt);

		if (!m_invalidated_memory_range.valid())
		{
			return;
		}

		if (is_stopped())
		{
			// We only need to commit host-resident memory to the guest in case of savestates or captures.
			on_invalidate_memory_range(m_invalidated_memory_range, rsx::invalidation_cause::read);
		}

		// Host sync
		rsx::mm_flush();

		on_invalidate_memory_range(m_invalidated_memory_range, rsx::invalidation_cause::unmap);
		m_invalidated_memory_range.invalidate();
	}

	void thread::renderctl(u32 request_code, void* /*args*/)
	{
		switch (request_code)
		{
		case rsx::mm_backend_ctrl::cmd_mm_flush:
			rsx::mm_flush();
			break;
		default:
			fmt::throw_exception("Unknown backend request: 0x%x", request_code);
		}
	}

	//Pause/cont wrappers for FIFO ctrl. Never call this from rsx thread itself!
	void thread::pause()
	{
		external_interrupt_lock++;
#ifdef RPCS3_PS4
        auto next_report = std::chrono::steady_clock::now() + 5s;
        u32 spins = 0;
        const bool elevated = ps4_rsx_priority_elevated();
        utils::progress_backoff backoff;
#endif
		while (!external_interrupt_ack && !is_stopped())
		{
#ifdef RPCS3_PS4
            if (!elevated)
            {
                // Equal priorities: spin, yield, then short sleeps after ~1 ms so the RSX can ack.
                backoff.pause();
                if ((++spins & 0x3ff) == 0 && std::chrono::steady_clock::now() >= next_report)
                {
                    ps4_startup_checkpoint(fmt::format(
                        "RSX: pause still waiting; initialized=%u, locks=%u, ack=%u, state=0x%x",
                        is_initialized.load(), external_interrupt_lock.load(),
                        external_interrupt_ack.load(), static_cast<u32>(state.load())).c_str());
                    next_report = std::chrono::steady_clock::now() + 5s;
                }
                continue;
            }
#endif
			utils::pause();
#ifdef RPCS3_PS4
            if ((++spins & 0xffff) == 0)
            {
                // Retain the lock handshake; only report and let its owner run.
                if (std::chrono::steady_clock::now() >= next_report)
                {
                    ps4_startup_checkpoint(fmt::format(
                        "RSX: pause still waiting; initialized=%u, locks=%u, ack=%u, state=0x%x",
                        is_initialized.load(), external_interrupt_lock.load(),
                        external_interrupt_ack.load(), static_cast<u32>(state.load())).c_str());
                    next_report = std::chrono::steady_clock::now() + 5s;
                }
                // A yield can immediately select this waiting PPU again on
                // Orbis. Sleep briefly so the RSX and allocator owners can run.
                std::this_thread::sleep_for(1ms);
            }
#endif
		}
	}

	void thread::unpause()
	{
		// TODO: Clean this shit up
		external_interrupt_lock--;
	}

	void thread::wait_pause()
	{
#ifdef RPCS3_PS4
		// 0.29: STALL dump wait reason (an external pause holds the RSX here).
		const ps4_rsx_wait_scope ps4_wait(ps4_wait_pause);
#endif
		do
		{
#ifdef RPCS3_PS4
			// 0.37: the RSX copy worker reads guest memory: its copies finish before the pauser changes mappings
			ps4_copy_worker_barrier();
#endif
			if (g_cfg.video.multithreaded_rsx)
			{
				g_fxo->get<rsx::dma_manager>().sync();
			}

			external_interrupt_ack.store(true);

#ifdef RPCS3_PS4
			const bool elevated = ps4_rsx_priority_elevated();
			utils::progress_backoff backoff;
#endif

			while (external_interrupt_lock && (cpu_flag::ret - state))
			{
				// TODO: Investigate non busy-spinning method
#ifdef RPCS3_PS4
				if (!elevated)
				{
					// Equal priorities: spin, yield, then short sleeps after ~1 ms.
					backoff.pause();
					continue;
				}
#endif
				utils::pause();
#ifdef RPCS3_PS4
                // The RSX has elevated priority. Let the lock owner run to
                // complete the mapping and release the acknowledged pause.
                std::this_thread::sleep_for(1ms);
#endif
			}

			external_interrupt_ack.store(false);
		}
		while (external_interrupt_lock && (cpu_flag::ret - state));
	}

	u32 thread::get_load()
	{
		// Average load over around 30 frames
		if (!performance_counters.last_update_timestamp || performance_counters.sampled_frames > 30)
		{
			const auto timestamp = get_system_time();
			const auto idle = performance_counters.idle_time.load();
			const auto elapsed = timestamp - performance_counters.last_update_timestamp;

			if (elapsed > idle)
				performance_counters.approximate_load = static_cast<u32>((elapsed - idle) * 100 / elapsed);
			else
				performance_counters.approximate_load = 0u;

			performance_counters.idle_time = 0;
			performance_counters.sampled_frames = 0;
			performance_counters.last_update_timestamp = timestamp;
		}

		return performance_counters.approximate_load;
	}

	void thread::on_frame_end(u32 buffer, bool forced)
	{
#ifdef RPCS3_PS4
        static thread_local bool guest_flip_reported = false;
        if (!forced && !guest_flip_reported)
        {
            ps4_startup_checkpoint(fmt::format("RSX: first guest frame flip; buffer=%u", buffer).c_str());
            guest_flip_reported = true;
        }
        static thread_local u64 last_fps_report = 0;
        static thread_local u64 guest_frames = 0;
        // 0.29: RSX work per guest frame over the window. Shows whether in-world frame time follows
        // draws/render passes rather than play time.
        static thread_local u64 sum_draws = 0, sum_submits = 0, sum_setup = 0, sum_vtx = 0, sum_tex = 0, sum_exec = 0;
        // Frames of the window measured by the per-draw profiler and by the phase timers (1 of 8 by default), and
        // wait_ticks at the start of the window (monotonic).
        static thread_local u64 prof_frames = 0, phase_frames = 0, window_wait_ticks = 0;
        // 0.39: OpenGL program links and blocking occlusion query reads (process totals, GLGSRender.cpp) at the start
        // of the window: compiles, compile us, query waits, wait us.
        static thread_local std::array<u64, 4> gl_window{};
        const auto gl_totals = []()
        {
            const auto& ps = utils::ps4::rsx_programs::g_pipeline_stats;
            return std::array<u64, 4>{ps.gl_compiles.load(std::memory_order_relaxed), ps.gl_compile_us.load(std::memory_order_relaxed),
                ps.zcull_query_waits.load(std::memory_order_relaxed), ps.zcull_wait_us.load(std::memory_order_relaxed)};
        };
        if (!forced)
        {
            const u64 now = get_system_time();
            bool first_window_frame = false;
            if (!last_fps_report)
            {
                // First guest flip: start every window counter here, not at boot (menus, shader cache).
                first_window_frame = true;
                last_fps_report = now;
                auto& pc = g_ps4_rsx_counters;
                for (auto* counter : {&pc.idle_sleeps, &pc.idle_sleep_us, &pc.sema_waits, &pc.sema_wait_us, &pc.sema_lowered,
                    &pc.sema_sleeps, &pc.sema_extended,
                    &pc.cpu_waits, &pc.limit_waits, &pc.limit_wait_us, &pc.hard_syncs, &pc.av_drains, &pc.invisible_dma,
                    &pc.dma_event_waits, &pc.dma_event_wait_us, &pc.event_timeouts, &pc.fence_timeouts,
                    &pc.pipeline_skips, &pc.frames_with_skips, &pc.frames_held, &pc.hold_candidates, &pc.pipe_fast_linked,
                    &pc.present_rt, &pc.present_tex, &pc.present_cpu, &pc.present_switches, &pc.surface_tag_misses,
                    &pc.report_hints, &pc.report_forced, &pc.report_forced_written, &pc.report_idle_hints, &pc.report_idle_forced,
                    &pc.report_hint_submits, &pc.vtx_cache_single_requests, &pc.vtx_cache_single_misses,
                    &pc.vtx_cache_multi_requests, &pc.vtx_cache_multi_misses, &pc.index_bytes, &pc.tex_uploads, &pc.tex_upload_misses})
                {
                    counter->release(0);
                }
                for (auto& counter : pc.rp_close)
                {
                    counter.release(0);
                }
                pc.pipe_pending_max.release(pc.pipe_pending.load());
                // sema_timeouts stays monotonic (the line below prints differences).
                auto& ph = g_ps4_rsx_phase;
                for (auto* counter : {&ph.fifo_ticks, &ph.methods, &ph.begin_ticks, &ph.render_passes, &ph.upload_bytes,
                    &ph.clear_ticks, &ph.report_ticks, &ph.image_in_ticks, &ph.notify_ticks, &ph.release_ticks, &ph.transform_ticks})
                {
                    counter->release(0);
                }
                window_wait_ticks = ph.wait_ticks.load();
                // 0.37: index path and copy worker counters start here too (values discarded)
                u64 index_simd = 0, index_skip = 0, index_expand = 0;
                bool index_sse41 = false;
                ps4_take_index_path_counts(index_simd, index_skip, index_expand, index_sse41);
                (void)ps4_copy_worker_text(1.);
                gl_window = gl_totals();
            }
            ++guest_frames;
            // m_profiler and the phase switch were set at the previous flip, so they describe this frame (whose phase
            // ticks were just discarded if it opens the first window).
            prof_frames += m_profiler.enabled;
            phase_frames += !first_window_frame && g_ps4_rsx_phase.sampled.observe();
            sum_draws += m_frame_stats.draw_calls;
            sum_submits += m_frame_stats.submit_count;
            sum_setup += static_cast<u64>(std::max<s64>(m_frame_stats.setup_time, 0));
            sum_vtx += static_cast<u64>(std::max<s64>(m_frame_stats.vertex_upload_time, 0));
            sum_tex += static_cast<u64>(std::max<s64>(m_frame_stats.textures_upload_time, 0));
            sum_exec += static_cast<u64>(std::max<s64>(m_frame_stats.draw_exec_time, 0));
            if (now - last_fps_report >= 10'000'000)
            {
                const u64 window_us = now - last_fps_report;
                const double frames = static_cast<double>(guest_frames);
                auto& pc = g_ps4_rsx_counters;

                // Per-thread CPU time of the RSX: tells a spinning RSX (close to 100%) from a sleeping one.
                static thread_local u64 s_last_cpu_us = 0;
                std::string rsx_cpu = "n/a";
                if (struct rusage ru{}; ::getrusage(RUSAGE_THREAD, &ru) == 0)
                {
                    const u64 cpu_us = static_cast<u64>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1'000'000 + ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
                    if (s_last_cpu_us && cpu_us >= s_last_cpu_us)
                        rsx_cpu = fmt::format("%llu%%", (cpu_us - s_last_cpu_us) * 100 / window_us);
                    s_last_cpu_us = cpu_us;
                }

                // The profiler times (setup, vtx, tex, exec) cover only the frames it ran in.
                const double prof = static_cast<double>(std::max<u64>(prof_frames, 1));
                std::string line = fmt::format("RSX gameplay: %.2f FPS (%llu guest flips in %llu us); FIFO load=%u%%; RSX CPU=%s; "
                    "per frame: draws=%.0f submits=%.1f rsx_us setup=%.0f vtx=%.0f tex=%.0f exec=%.0f; "
                    "idle sleeps=%llu (%llu us); sema waits=%llu (%llu us, lowered %llu); cpu_waits=%llu; limiter=%llu (%llu us); hard syncs=%llu; "
                    "DMA: av drains=%llu fence-covered=%llu event waits=%llu (%llu us) event timeouts=%llu fence timeouts=%llu",
                    frames * 1'000'000.0 / window_us, guest_frames, window_us,
                    get_load(), rsx_cpu, // approximate_load is only refreshed by get_load()
                    sum_draws / frames, sum_submits / frames, sum_setup / prof, sum_vtx / prof, sum_tex / prof, sum_exec / prof,
                    pc.idle_sleeps.exchange(0), pc.idle_sleep_us.exchange(0), pc.sema_waits.exchange(0), pc.sema_wait_us.exchange(0),
                    pc.sema_lowered.exchange(0), pc.cpu_waits.exchange(0), pc.limit_waits.exchange(0), pc.limit_wait_us.exchange(0),
                    pc.hard_syncs.exchange(0), pc.av_drains.exchange(0), pc.invisible_dma.exchange(0), pc.dma_event_waits.exchange(0),
                    pc.dma_event_wait_us.exchange(0), pc.event_timeouts.exchange(0), pc.fence_timeouts.exchange(0));

                // 0.34: semaphore sleep phase and the TDR extension (timeouts: difference of a monotonic counter).
                static thread_local u64 s_last_sema_timeouts = 0;
                const u64 sema_timeouts = pc.sema_timeouts.load();
                fmt::append(line, "; sema sleeps=%llu, extended=%llu, timeouts=%llu",
                    pc.sema_sleeps.exchange(0), pc.sema_extended.exchange(0), sema_timeouts - std::min(s_last_sema_timeouts, sema_timeouts));
                s_last_sema_timeouts = sema_timeouts;

                // Deferred ZCULL reports serviced inside semaphore waits and FIFO idle (flush hints: command buffers
                // submitted for them; forced: drains with a GPU wait, in waits of 50 ms or more).
                fmt::append(line, "; reports: hints=%llu (flush hints %llu) forced=%llu (written %llu) idle=%llu (forced %llu)",
                    pc.report_hints.exchange(0), pc.report_hint_submits.exchange(0), pc.report_forced.exchange(0),
                    pc.report_forced_written.exchange(0), pc.report_idle_hints.exchange(0), pc.report_idle_forced.exchange(0));

                // 0.34: where the untimed RSX time goes. fifo_us covers the FIFO method packets (decode and every
                // handler, so it includes begin_us, the handler buckets and the draw timers above); RSX waits and the
                // flip are excluded. Timers run in the sampled frames only; counts cover every frame.
                auto& ph = g_ps4_rsx_phase;
                const double ticks_per_us = static_cast<double>(utils::get_tsc_freq()) / 1'000'000.;
                const u64 fifo_ticks = ph.fifo_ticks.exchange(0), begin_ticks = ph.begin_ticks.exchange(0);
                const u64 methods = ph.methods.exchange(0), render_passes = ph.render_passes.exchange(0);
                const u64 clear_ticks = ph.clear_ticks.exchange(0), report_ticks = ph.report_ticks.exchange(0);
                const u64 image_in_ticks = ph.image_in_ticks.exchange(0), notify_ticks = ph.notify_ticks.exchange(0);
                const u64 release_ticks = ph.release_ticks.exchange(0), transform_ticks = ph.transform_ticks.exchange(0);
                const u64 wait_ticks = ph.wait_ticks.load();
                if (ph.on.observe() && ticks_per_us > 0)
                {
                    const double sampled = static_cast<double>(std::max<u64>(phase_frames, 1)) * ticks_per_us;
                    fmt::append(line, "; phases sampled 1/8 (%llu frames): fifo_us/frame=%.0f begin_us/frame=%.0f methods/frame=%.0f render passes/frame=%.1f",
                        phase_frames, fifo_ticks / sampled, begin_ticks / sampled, methods / frames, render_passes / frames);
                    fmt::append(line, "; handler us/frame: clear=%.0f report=%.0f image_in=%.0f notify=%.0f release=%.0f transform=%.0f",
                        clear_ticks / sampled, report_ticks / sampled, image_in_ticks / sampled, notify_ticks / sampled,
                        release_ticks / sampled, transform_ticks / sampled);
                }
                else
                {
                    line += "; phases off (rsx-profiler-off.flag)";
                }
                if (ticks_per_us > 0)
                {
                    // Every RSX wait, the whole flip included (all frames).
                    fmt::append(line, "; flip+wait us/frame=%.0f", (wait_ticks - std::min(window_wait_ticks, wait_ticks)) / ticks_per_us / frames);
                }
                window_wait_ticks = wait_ticks;
                fmt::append(line, "; upload KiB/frame=%.0f (%s)", ph.upload_bytes.exchange(0) / 1024. / frames,
                    ps4_rsx_streaming_copy() ? "streaming stores" : "memcpy, rsx-plain-copy.flag");
                // 0.37: vertex copies split between the copy worker and the RSX thread (only while the worker is on)
                line += ps4_copy_worker_text(frames);
                // 0.37: source indices per frame by upload path (asmjit SIMD, skip-restart, CPU expansion)
                {
                    u64 index_simd = 0, index_skip = 0, index_expand = 0;
                    bool index_sse41 = false;
                    ps4_take_index_path_counts(index_simd, index_skip, index_expand, index_sse41);
                    fmt::append(line, "; index: simd %.0f, skip-restart %.0f (%s), expand %.0f",
                        index_simd / frames, index_skip / frames, index_sse41 ? "sse41" : "scalar", index_expand / frames);
                }

                // Vulkan backend counts (every frame): render-pass closes by cause, weak vertex cache, converted
                // index data, texture cache uploads.
                {
                    std::array<u64, ps4_rp_close_cause_count> closes{};
                    u64 closes_total = 0;
                    for (u32 i = 0; i < ps4_rp_close_cause_count; i++)
                    {
                        closes[i] = pc.rp_close[i].exchange(0);
                        closes_total += closes[i];
                    }

                    if (closes_total)
                    {
                        fmt::append(line, "; rp closes/frame=%.1f (barrier %.1f, texture copy %.1f, texture cache %.1f, query %.1f, explicit %.1f, pass change %.1f, compute %.1f, other %.1f)",
                            closes_total / frames, closes[ps4_rp_close_barrier] / frames, closes[ps4_rp_close_texture_copy] / frames,
                            closes[ps4_rp_close_texture_cache] / frames, closes[ps4_rp_close_query] / frames, closes[ps4_rp_close_explicit] / frames,
                            closes[ps4_rp_close_pass_change] / frames, closes[ps4_rp_close_compute] / frames, closes[ps4_rp_close_other] / frames);
                    }

                    const u64 single_req = pc.vtx_cache_single_requests.exchange(0), single_miss = pc.vtx_cache_single_misses.exchange(0);
                    const u64 multi_req = pc.vtx_cache_multi_requests.exchange(0), multi_miss = pc.vtx_cache_multi_misses.exchange(0);
                    const auto hit_text = [](u64 requests, u64 misses)
                    {
                        return requests ? fmt::format("%.0f%%", 100. * static_cast<double>(requests - std::min(misses, requests)) / static_cast<double>(requests)) : std::string("n/a");
                    };

                    if (single_req || multi_req)
                    {
                        fmt::append(line, "; vertex cache hit single %s of %.0f/frame, multi %s of %.0f/frame",
                            hit_text(single_req, single_miss), single_req / frames, hit_text(multi_req, multi_miss), multi_req / frames);
                    }

                    const u64 index_bytes = pc.index_bytes.exchange(0);
                    const u64 tex_uploads = pc.tex_uploads.exchange(0), tex_misses = pc.tex_upload_misses.exchange(0);
                    if (index_bytes || tex_uploads || tex_misses)
                    {
                        fmt::append(line, "; index KiB/frame=%.0f; tex uploads/frame=%.1f (misses %.1f)",
                            index_bytes / 1024. / frames, tex_uploads / frames, tex_misses / frames);
                    }
                }

                // 0.34: draws dropped while their pipelines compile (asynchronous recompiler, no interpreter
                // fallback) and the compile backlog; the hold counters belong to present-hold-ms.txt. Skips are
                // added at the emulated flip, so a frame still being drawn shows up in the next line.
                const u32 pipes_pending = pc.pipe_pending.load();
                const u32 pipes_pending_max = std::max(pc.pipe_pending_max.exchange(pipes_pending), pipes_pending);
                fmt::append(line, "; pipelines: skipped draws=%llu in %llu frames (hold candidates %llu, held %llu), pending now %u max %u, fast-linked=%llu",
                    pc.pipeline_skips.exchange(0), pc.frames_with_skips.exchange(0), pc.hold_candidates.exchange(0), pc.frames_held.exchange(0),
                    pipes_pending, pipes_pending_max, pc.pipe_fast_linked.exchange(0));

                // 0.34: where the presented image came from (render target, texture cache, CPU memory) and failed
                // surface tag tests: CPU/SPU-written frames that the surface cache may not see.
                fmt::append(line, "; present src rt=%llu tex=%llu cpu=%llu switches=%llu; tag misses=%llu",
                    pc.present_rt.exchange(0), pc.present_tex.exchange(0), pc.present_cpu.exchange(0), pc.present_switches.exchange(0),
                    pc.surface_tag_misses.exchange(0));

                // 0.39: OpenGL program compiles and blocking ZCULL query reads of the window (with their time)
                if (g_cfg.video.renderer == video_renderer::opengl)
                {
                    const std::array<u64, 4> gl_now = gl_totals();
                    const auto gl_diff = [&](usz i) { return gl_now[i] - std::min(gl_window[i], gl_now[i]); };
                    fmt::append(line, "; GL program compiles %llu (%llu ms), zcull query waits %llu (%llu ms)",
                        gl_diff(0), gl_diff(1) / 1000, gl_diff(2), gl_diff(3) / 1000);
                    gl_window = gl_now;
                }

                ps4_startup_checkpoint(line.c_str());
                last_fps_report = now;
                guest_frames = 0;
                prof_frames = phase_frames = 0;
                sum_draws = sum_submits = sum_setup = sum_vtx = sum_tex = sum_exec = 0;
            }
        }
#endif
		bool pause_emulator = false;

		// MM sync. This is a pre-emptive operation, so we can use a deferred request.
		rsx::mm_flush_lazy();

		// Marks the end of a frame scope GPU-side
		if (g_user_asked_for_frame_capture.exchange(false) && !capture_current_frame)
		{
			capture_current_frame = true;
			frame_debug.reset();
			frame_capture.reset();

			// random number just to jumpstart the size
			frame_capture.replay_commands.reserve(8000);

			// capture first tile state with nop cmd
			rsx::frame_capture_data::replay_command replay_cmd;
			replay_cmd.rsx_command = std::make_pair(NV4097_NO_OPERATION, 0);
			frame_capture.replay_commands.push_back(std::move(replay_cmd));
			capture::capture_display_tile_state(this, frame_capture.replay_commands.back());
		}
		else if (capture_current_frame)
		{
			capture_current_frame = false;

			const std::string file_path = fs::get_config_dir() + "captures/" + (Emu.GetTitleID().empty() ? Emu.GetTitle() : Emu.GetTitleID()) + "_" + date_time::current_time_narrow() + "_capture.rrc.gz";

			fs::pending_file temp(file_path);

			utils::serial save_manager;

			if (temp.file)
			{
				save_manager.m_file_handler = make_compressed_serialization_file_handler(temp.file);
				save_manager(frame_capture);

				save_manager.m_file_handler->finalize(save_manager);

				if (temp.commit(false))
				{
					rsx_log.success("Capture successful: %s", file_path);
					frame_capture.reset();
					pause_emulator = true;
				}
				else
				{
					rsx_log.error("Capture failed: %s (%s)", file_path, fs::g_tls_error);
				}
			}
			else
			{
				rsx_log.fatal("Capture failed: %s (%s)", file_path, fs::g_tls_error);
			}
		}

		if (zcull_ctrl->has_pending())
		{
			// NOTE: This is a workaround for buggy games.
			// Some applications leave the zpass/stats gathering active but don't use the information.
			// This can lead to the zcull unit using up all the memory queueing up operations that never get consumed.
			// Seen in Diablo III and Yakuza 5
			zcull_ctrl->clear(this, CELL_GCM_ZPASS_PIXEL_CNT | CELL_GCM_ZCULL_STATS);
		}

		// Save current state
		m_queued_flip.stats = m_frame_stats;
		m_queued_flip.push(buffer);
		m_queued_flip.skip_frame = skip_current_frame;

		if (!forced) [[likely]]
		{
			if (!g_cfg.video.disable_FIFO_reordering)
			{
				// Try to enable FIFO optimizations
				// Only rarely useful for some games like RE4
				m_flattener.evaluate_performance(m_frame_stats.draw_calls);
			}

			if (g_cfg.video.frame_skip_enabled)
			{
				m_skip_frame_ctr++;

				if (m_skip_frame_ctr >= g_cfg.video.consecutive_frames_to_draw)
					m_skip_frame_ctr = -g_cfg.video.consecutive_frames_to_skip;

				skip_current_frame = (m_skip_frame_ctr < 0);
			}
		}
		else
		{
			if (!g_cfg.video.disable_FIFO_reordering)
			{
				// Flattener is unusable due to forced random flips
				m_flattener.force_disable();
			}

			if (g_cfg.video.frame_skip_enabled)
			{
				rsx_log.error("Frame skip is not compatible with this application");
			}
		}

		if (pause_emulator)
		{
			Emu.Pause();
			thread_ctrl::wait_for(30'000);
		}

		// Reset current stats
		m_frame_stats = {};
#ifdef RPCS3_PS4
		// 0.29: the setup/vertex/texture/exec times of the 10 s 'RSX gameplay' line are 0 while the
		// profiler is off (Common/profiling_timer.hpp). Cost: a few clock reads per draw on the RSX thread;
		// /data/rpcs3/rsx-profiler-off.flag turns it off again (A/B of that cost; the debug overlay still wins).
		// The profiler and the phase timers (two TSC reads per FIFO packet) run in 1 frame of 8: the 10 s line
		// divides their sums by the frames they ran in.
		static thread_local u64 s_ps4_sample_frame = 0;
		const bool ps4_sample = s_ps4_rsx_profiler.observe() && (s_ps4_sample_frame++ % 8) == 0;
		g_ps4_rsx_phase.sampled.release(ps4_sample);
		m_profiler.enabled = ps4_sample || !!g_cfg.video.debug_overlay;
#else
		m_profiler.enabled = !!g_cfg.video.debug_overlay;
#endif
	}

	f64 thread::get_cached_display_refresh_rate()
	{
		constexpr u64 uses_per_query = 512;

		f64 result = m_cached_display_rate;
		u64 count = m_display_rate_fetch_count++;

		while (true)
		{
			if (count % 512 == 0)
			{
				result = get_display_refresh_rate();
				m_cached_display_rate.store(result);
				m_display_rate_fetch_count += uses_per_query; // Notify users of the new value
				break;
			}

			const u64 new_count = m_display_rate_fetch_count;
			const f64 new_cached = m_cached_display_rate;

			if (result == new_cached && count / uses_per_query == new_count / uses_per_query)
			{
				break;
			}

			// An update might have gone through
			count = new_count;
			result = new_cached;
		}

		return result;
	}

	bool thread::request_emu_flip(u32 buffer)
	{
		if (is_current_thread()) // requested through command buffer
		{
			// NOTE: The flip will clear any queued flip requests
			handle_emu_flip(buffer);
		}
		else // requested 'manually' through ppu syscall
		{
			if (async_flip_requested & flip_request::emu_requested)
			{
				// ignore multiple requests until previous happens
				return true;
			}

			async_flip_buffer = buffer;
			async_flip_requested |= flip_request::emu_requested;

			m_eng_interrupt_mask |= rsx::display_interrupt;

			if (state & cpu_flag::exit)
			{
				// Resubmit possibly-ignored flip on savestate load
				return false;
			}
		}

		return true;
	}

	void thread::handle_emu_flip(u32 buffer)
	{
		if (m_queued_flip.in_progress)
		{
			// Rescursion not allowed!
			return;
		}

		if (!m_queued_flip.pop(buffer))
		{
			// Frame was not queued before flipping
			on_frame_end(buffer, true);
			ensure(m_queued_flip.pop(buffer));
		}

		double limit = 0.;
		const auto frame_limit = g_disable_frame_limit ? frame_limit_type::none : g_cfg.video.frame_limit;

		switch (frame_limit)
		{
		case frame_limit_type::none: limit = g_cfg.core.max_cpu_preempt_count_per_frame ? static_cast<double>(g_cfg.video.vblank_rate) : 0.; break;
		case frame_limit_type::_30: limit = 30.; break;
		case frame_limit_type::_50: limit = 50.; break;
		case frame_limit_type::_60: limit = 60.; break;
 		case frame_limit_type::_120: limit = 120.; break;
		case frame_limit_type::display_rate: limit = get_cached_display_refresh_rate(); break;
		case frame_limit_type::_auto: limit = static_cast<double>(g_cfg.video.vblank_rate); break;
		case frame_limit_type::_ps3: limit = 0.; break;
		case frame_limit_type::infinite: limit = 0.; break;
		default:
			break;
		}

		if (double limit2 = g_cfg.video.second_frame_limit; limit2 >= 0.1 && (limit2 < limit || !limit))
		{
			// Apply a second limit
			limit = limit2;
		}

		if (limit)
		{
			const u64 needed_us = static_cast<u64>(1000000 / limit);
			const u64 time = std::max<u64>(get_system_time(), target_rsx_flip_time > needed_us ? target_rsx_flip_time - needed_us : 0);

			if (int_flip_index)
			{
				if (target_rsx_flip_time > time + 1000)
				{
					const auto delay_us = target_rsx_flip_time - time;
#ifdef RPCS3_PS4
					// 0.29: frame limiter sleeps (STALL wait reason, counted in the 10 s 'RSX gameplay' line).
					const ps4_rsx_wait_scope ps4_wait(ps4_wait_frame_limit);
					const u64 ps4_t0 = get_system_time();
#endif
					lv2_obj::wait_timeout(delay_us, nullptr, false);
#ifdef RPCS3_PS4
					g_ps4_rsx_counters.limit_waits++;
					g_ps4_rsx_counters.limit_wait_us += get_system_time() - ps4_t0;
#endif
					performance_counters.idle_time += delay_us;
				}
			}

			target_rsx_flip_time = std::max(time, target_rsx_flip_time) + needed_us;
			flip_notification_count = 1;
		}
		else if (frame_limit == frame_limit_type::_ps3)
		{
			bool exit = false;

			if (vblank_at_flip == umax)
			{
				vblank_at_flip = +vblank_count;
				flip_notification_count = 1;
				exit = true;
			}

			if (requested_vsync && (exit || vblank_at_flip == vblank_count))
			{
				// Not yet signaled, handle it later
				async_flip_requested |= flip_request::emu_requested;
				async_flip_buffer = buffer;
				return;
			}

			vblank_at_flip = umax;
		}
		else
		{
			flip_notification_count = 1;
		}

		int_flip_index += flip_notification_count;

		current_display_buffer = buffer;
		m_queued_flip.emu_flip = true;
		m_queued_flip.in_progress = true;
		m_queued_flip.skip_frame |= g_cfg.video.disable_video_output && !g_cfg.video.perf_overlay.enabled;

#ifdef RPCS3_PS4
		{
			// 0.29: STALL dump wait reason for the whole flip/present (nested fence waits show too).
			const ps4_rsx_wait_scope ps4_wait(ps4_wait_flip);
			flip(m_queued_flip);
		}
#else
		flip(m_queued_flip);
#endif

		last_guest_flip_timestamp = get_system_time() - 1000000;
		flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE;
		m_queued_flip.in_progress = false;

		while (flip_notification_count--)
		{
			if (!isHLE)
			{
				sys_rsx_context_attribute(0x55555555, 0xFEC, buffer, 0, 0, 0);

				if (unsent_gcm_events)
				{
					// TODO: A proper fix
					return;
				}

				continue;
			}

			if (auto ptr = flip_handler)
			{
				intr_thread->cmd_list
				({
					{ ppu_cmd::set_args, 1 }, u64{ 1 },
					{ ppu_cmd::lle_call, ptr },
					{ ppu_cmd::sleep, 0 }
				});

				intr_thread->cmd_notify.store(1);
				intr_thread->cmd_notify.notify_one();
			}
		}
	}

	void thread::evaluate_cpu_usage_reduction_limits()
	{
		const u64 max_preempt_count = g_cfg.core.max_cpu_preempt_count_per_frame;

		if (!max_preempt_count)
		{
			frame_times.clear();
			lv2_obj::set_yield_frequency(0, 0);
			return;
		}

		const u64 current_time = get_system_time();
		const u64 current_tsc = utils::get_tsc();
		u64 preempt_count = 0;

		if (frame_times.size() >= 60)
		{
			u64 diffs = 0;

			for (usz i = 1; i < frame_times.size(); i++)
			{
				const u64 cur_diff = frame_times[i].timestamp - frame_times[i - 1].timestamp;
				diffs += cur_diff;
			}

			const usz avg_frame_time = diffs / 59;

			u32 lowered_delay = 0;
			u32 raised_delay = 0;
			bool can_reevaluate = true;
			u64 prev_preempt_count = umax;

			for (usz i = frame_times.size() - 30; i < frame_times.size(); i++)
			{
				if (prev_preempt_count == umax)
				{
					prev_preempt_count = frame_times[i].preempt_count;
					continue;
				}

				if (prev_preempt_count != frame_times[i].preempt_count)
				{
					if (prev_preempt_count > frame_times[i].preempt_count)
					{
						lowered_delay++;
					}
					else if (prev_preempt_count < frame_times[i].preempt_count)
					{
						raised_delay++;
					}

					if (i > frame_times.size() - 30)
					{
						// Slow preemption count increase
						can_reevaluate = false;
					}
				}

				prev_preempt_count = frame_times[i].preempt_count;
			}

			preempt_count = std::min<u64>(frame_times.back().preempt_count, max_preempt_count);

			u32 fails = 0;
			u32 hard_fails = 0;
			bool is_last_frame_a_fail = false;

			auto abs_dst = [](u64 a, u64 b)
			{
				return a >= b ? a - b : b - a;
			};

			for (u32 i = 1; i <= frame_times.size(); i++)
			{
				const u64 cur_diff = (i == frame_times.size() ? current_time : frame_times[i].timestamp) - frame_times[i - 1].timestamp;

				if (const u64 diff_of_diff = abs_dst(cur_diff, avg_frame_time);
					diff_of_diff >= avg_frame_time / 7)
				{
					if (diff_of_diff >= avg_frame_time / 3)
					{
						raised_delay++;
						hard_fails++;

						if (i == frame_times.size())
						{
							is_last_frame_a_fail = true;
						}
					}

					if (fails != umax)
					{
						fails++;
					}
				}
			}

			bool hard_measures_taken = false;
			const usz fps_10 = 10'000'000 / avg_frame_time;

			auto lower_preemption_count = [&]()
			{
				if (preempt_count >= 10)
				{
					preempt_count -= 10;
				}
				else
				{
					preempt_count = 0;
				}

				if ((hard_fails > 2 || fails > 20) && is_last_frame_a_fail)
				{
					hard_measures_taken = preempt_count > 1;
					preempt_count = preempt_count * 7 / 8;
					prevent_preempt_increase_tickets = 10;
				}
				else
				{
					prevent_preempt_increase_tickets = std::max<u32>(7, prevent_preempt_increase_tickets);
				}
			};

			const u64 vblank_rate_10 = g_cfg.video.vblank_rate * 10;

			if (can_reevaluate)
			{
				const bool is_avg_fps_ok = (abs_dst(fps_10, 300) < 3 || abs_dst(fps_10, 600) < 4 || abs_dst(fps_10, vblank_rate_10) < 4 || abs_dst(fps_10, vblank_rate_10 / 2) < 3);

				if (!hard_fails && fails < 6 && is_avg_fps_ok)
				{
					if (prevent_preempt_increase_tickets)
					{
						prevent_preempt_increase_tickets--;
					}
					else
					{
						preempt_count = std::min<u64>(preempt_count + 4, max_preempt_count);
					}
				}
				else
				{
					lower_preemption_count();
				}
			}
			// Sudden FPS drop detection
			else if ((fails > 13 || hard_fails > 2 || !(abs_dst(fps_10, 300) < 20 || abs_dst(fps_10, 600) < 30 || abs_dst(fps_10, g_cfg.video.vblank_rate * 10) < 30 || abs_dst(fps_10, g_cfg.video.vblank_rate * 10 / 2) < 20)) && lowered_delay < raised_delay && is_last_frame_a_fail)
			{
				lower_preemption_count();
			}

			perf_log.trace("CPU preemption control: reeval=%d, preempt_count=%llu, fails=%u, hard=%u, avg_frame_time=%llu, highered=%u, lowered=%u, taken=%u", can_reevaluate, preempt_count, fails, hard_fails, avg_frame_time, raised_delay, lowered_delay, ::g_lv2_preempts_taken.load());

			if (hard_measures_taken)
			{
				preempt_fail_old_preempt_count = std::max<u64>(preempt_fail_old_preempt_count, std::min<u64>(frame_times.back().preempt_count, max_preempt_count));
			}
			else if (preempt_fail_old_preempt_count)
			{
				perf_log.error("Lowering current preemption count significantly due to a performance drop, if this issue persists frequently consider lowering max preemptions count to 'new-count' or lower. (old-count=%llu, new-count=%llu)", preempt_fail_old_preempt_count, preempt_count);
				preempt_fail_old_preempt_count = 0;
			}

			const u64 tsc_diff = (current_tsc - frame_times.back().tsc);
			const u64 time_diff = (current_time - frame_times.back().timestamp);
			const u64 preempt_diff = tsc_diff * (1'000'000 / 30) / (time_diff * std::max<u64>(preempt_count, 1ull));

			if (!preempt_count)
			{
				lv2_obj::set_yield_frequency(0, 0);
			}
			else if (abs_dst(fps_10, 300) < 30)
			{
				// Set an upper limit so a backoff technique would be taken if there is a sudden performance drop
				// Allow 4% of no yield to reduce significantly the risk of stutter
				lv2_obj::set_yield_frequency(preempt_diff, current_tsc + (tsc_diff * (1'000'000 * 96 / (30 * 100)) / time_diff));
			}
			else if (abs_dst(fps_10, 600) < 40)
			{
				// 5% for 60fps
				lv2_obj::set_yield_frequency(preempt_diff, current_tsc + (tsc_diff * (1'000'000 * 94 / (60 * 100)) / time_diff));
			}
			else if (abs_dst(fps_10, vblank_rate_10) < 40)
			{
				lv2_obj::set_yield_frequency(preempt_diff, current_tsc + (tsc_diff * (1'000'000 * 94 / (vblank_rate_10 * 10)) / time_diff));
			}
			else if (abs_dst(fps_10, vblank_rate_10 / 2) < 30)
			{
				lv2_obj::set_yield_frequency(preempt_diff, current_tsc + (tsc_diff * (1'000'000 * 96 / ((vblank_rate_10 / 2) * 10)) / time_diff));
			}
			else
			{
				// Undetected case, last 12% is with no yield
				lv2_obj::set_yield_frequency(preempt_diff, current_tsc + (tsc_diff * 88 / 100));
			}

			frame_times.pop_front();
		}
		else
		{
			lv2_obj::set_yield_frequency(0, 0);
		}

		frame_times.push_back(frame_time_t{preempt_count, current_time, current_tsc});
	}

	void vblank_thread::set_thread(std::shared_ptr<named_thread<std::function<void()>>> thread)
	{
		std::swap(m_thread, thread);
	}

	vblank_thread& vblank_thread::operator=(thread_state state)
	{
		if (m_thread)
		{
			*m_thread = state;
		}

		return *this;
	}
} // namespace rsx
