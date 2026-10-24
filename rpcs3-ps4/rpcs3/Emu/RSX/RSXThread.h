#pragma once

#include <queue>
#include <deque>

#include "RSXFIFO.h"
#include "RSXOffload.h"
#include "RSXZCULL.h"
#include "Common/bitfield.hpp"
#include "Common/profiling_timer.hpp"
#include "Common/texture_cache_types.h"
#include "Common/TextureUtils.h"
#include "Program/RSXVertexProgram.h"
#include "Program/RSXFragmentProgram.h"
#include "Utils/rsx_utils.h"

#include "Utilities/Thread.h"
#include "Utilities/geometry.h"
#include "Capture/rsx_trace.h"
#include "Capture/rsx_replay.h"

#include "Emu/Cell/lv2/sys_rsx.h"
#include "Emu/IdManager.h"

#include "Core/RSXDisplay.h"
#include "Core/RSXDrawCommands.h"
#include "Core/RSXDriverState.h"
#include "Core/RSXFrameBuffer.h"
#include "Core/RSXContext.h"
#include "Core/RSXVertexTypes.h"

#include "NV47/FW/GRAPH_backend.h"

#ifdef RPCS3_PS4
#include "util/tsc.hpp"
#endif

extern atomic_t<bool> g_user_asked_for_frame_capture;
extern atomic_t<bool> g_disable_frame_limit;
extern rsx::frame_trace_data frame_debug;
extern rsx::frame_capture_data frame_capture;

namespace rsx
{
	class RSXDMAWriter;

	struct context;

	namespace overlays
	{
		class display_manager;
	}

	enum framebuffer_creation_context : u8
	{
		context_draw = 0,
		context_clear_color = 1,
		context_clear_depth = 2,
		context_clear_all = context_clear_color | context_clear_depth
	};

	enum eng_interrupt_reason : u32
	{
		backend_interrupt       = 0x0001,        // Backend-related interrupt
		memory_config_interrupt = 0x0002,        // Memory configuration changed
		display_interrupt       = 0x0004,        // Display handling
		pipe_flush_interrupt    = 0x0008,        // Flush pipelines
		dma_control_interrupt   = 0x0010,        // DMA interrupt

		all_interrupt_bits = memory_config_interrupt | backend_interrupt | display_interrupt | pipe_flush_interrupt
	};

	enum result_flags: u8
	{
		result_none = 0,
		result_error = 1,
		result_zcull_intr = 2
	};

	u32 get_vertex_type_size_on_host(vertex_base_type type, u32 size);

	u32 get_address(u32 offset, u32 location, u32 size_to_check = 0, std::source_location src_loc = std::source_location::current());

	struct backend_configuration
	{
		bool supports_multidraw;               // Draw call batching
		bool supports_hw_a2c;                  // Alpha to coverage
		bool supports_hw_a2c_1spp;             // Alpha to coverage at 1 sample per pixel
		bool supports_hw_renormalization;      // Should be true on NV hardware which matches PS3 texture renormalization behaviour
		bool supports_hw_msaa;                 // MSAA support
		bool supports_hw_a2one;                // Alpha to one
		bool supports_hw_conditional_render;   // Conditional render
		bool supports_hw_instanced_rendering;  // Instanced draws
		bool supports_passthrough_dma;         // DMA passthrough
		bool supports_asynchronous_compute;    // Async compute
		bool supports_host_gpu_labels;         // Advanced host synchronization
		bool supports_normalized_barycentrics; // Basically all GPUs except NVIDIA have properly normalized barycentrics
		bool supports_last_provoking_vertex;   // Flat shading using RSX's last-vertex convention
		bool supports_programmable_blending;   // Can handle programmable blending requests
	};

	struct desync_fifo_cmd_info
	{
		u32 cmd;
		u64 timestamp;
	};

	// TODO: This class is a mess, this needs to be broken into smaller chunks, like I did for RSXFIFO and RSXZCULL (kd)
	class thread : public cpu_thread, public GCM_context, public GRAPH_backend
	{
		u64 timestamp_ctrl = 0;
		u64 timestamp_subvalue = 0;
		u64 m_cycles_counter = 0;

		display_flip_info_t m_queued_flip{};

		void cpu_task() override;
	protected:

		s32 m_skip_frame_ctr = 0;
		bool skip_current_frame = false;

		primitive_class m_current_draw_mode = primitive_class::polygon;

		backend_configuration backend_config{};

		// FIFO
	public:
		std::unique_ptr<FIFO::FIFO_control> fifo_ctrl;
		atomic_t<bool> rsx_thread_running{ false };
		std::vector<std::pair<u32, u32>> dump_callstack_list() const override;
		void dump_misc(std::string& ret, std::any& custom_data) const override;

	protected:
		FIFO::flattening_helper m_flattener;
		u32 fifo_ret_addr = RSX_CALL_STACK_EMPTY;
		u32 saved_fifo_ret = RSX_CALL_STACK_EMPTY;
		u32 restore_fifo_cmd = 0;
		u32 restore_fifo_count = 0;

		// Occlusion query
		bool zcull_surface_active = false;
		std::unique_ptr<reports::ZCULL_control> zcull_ctrl;

		// Framebuffer setup
		rsx::gcm_framebuffer_info m_surface_info[rsx::limits::color_buffers_count];
		rsx::gcm_framebuffer_info m_depth_surface_info;
		framebuffer_layout m_framebuffer_layout{};

		// Overlays
		rsx::overlays::display_manager* m_overlay_manager = nullptr;

		atomic_t<u64> m_display_rate_fetch_count = 0;
		atomic_t<f64> m_cached_display_rate = 60.;
		f64 get_cached_display_refresh_rate();
		virtual f64 get_display_refresh_rate() const = 0;

		// Invalidated memory range
		address_range32 m_invalidated_memory_range;

		// Profiler
		rsx::profiling_timer m_profiler;
		frame_statistics_t m_frame_stats{};

		// Savestates related
		u32 m_pause_after_x_flips = 0;

		// Context
		context* m_ctx = nullptr;

		// Host DMA
		std::unique_ptr<RSXDMAWriter> m_host_dma_ctrl;

		// Draw call management
		draw_command_processor m_draw_processor;

	public:
		atomic_t<u64> new_get_put = u64{umax};
		u32 restore_point = 0;
		u32 dbg_step_pc = 0;
		u32 last_known_code_start = 0;
		atomic_t<u32> external_interrupt_lock{ 0 };
		atomic_t<bool> external_interrupt_ack{ false };
		atomic_t<u32> is_initialized{0};

		bool is_fifo_idle() const;
		void flush_fifo();

		// Returns [count of found commands, PC of their start]
		std::pair<u32, u32> try_get_pc_of_x_cmds_backwards(s32 count, u32 get) const;

		void recover_fifo(std::source_location src_loc = std::source_location::current());

		static void fifo_wake_delay(u64 div = 1);
		u32 get_fifo_cmd() const;

		void dump_regs(std::string&, std::any& custom_data) const override;
		void cpu_wait(bs_t<cpu_flag> old) override;

		static constexpr u32 id_base = 0x5555'5555; // See get_current_cpu_thread()

		// Performance approximation counters
		struct
		{
			atomic_t<u64> idle_time{ 0 };  // Time spent idling in microseconds
			u64 last_update_timestamp = 0; // Timestamp of last load update
			u64 FIFO_idle_timestamp = 0;   // Timestamp of when FIFO queue becomes idle
			FIFO::state state = FIFO::state::running;
			u32 approximate_load = 0;
			u32 sampled_frames = 0;
		}
		performance_counters;

		enum class flip_request : u32
		{
			emu_requested = 1,
			native_ui = 2,

			any = emu_requested | native_ui
		};

		atomic_bitmask_t<flip_request> async_flip_requested{};
		u8 async_flip_buffer{ 0 };

		surface_scaling_config_t resolution_scaling_config{};

		void capture_frame(const std::string& name) const;
		const backend_configuration& get_backend_config() const { return backend_config; }

		const draw_command_processor* draw_processor() const { return &m_draw_processor; }

	public:
		shared_ptr<named_thread<ppu_thread>> intr_thread;

		// I hate this flag, but until hle is closer to lle, its needed
		bool isHLE{ false };
		bool serialized = false;

		u32 flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE;
		int debug_level = CELL_GCM_DEBUG_LEVEL0;

		atomic_t<bool> requested_vsync{true};
		atomic_t<bool> enable_second_vhandler{false};

		bool send_event(u64, u64, u64);

		std::array<bool, 16> m_textures_dirty;
		std::array<bool, 4> m_vertex_textures_dirty;
		rsx::framebuffer_creation_context m_current_framebuffer_context = rsx::framebuffer_creation_context::context_draw;

		rsx::atomic_bitmask_t<rsx::eng_interrupt_reason> m_eng_interrupt_mask;
		rsx::bitmask_t<rsx::pipeline_state> m_graphics_state;

		u64 ROP_sync_timestamp = 0;

		program_hash_util::fragment_program_utils::fragment_program_metadata current_fp_metadata = {};
		program_hash_util::vertex_program_utils::vertex_program_metadata current_vp_metadata = {};

		std::array<std::unique_ptr<rsx::sampled_image_descriptor_base>, rsx::limits::fragment_textures_count> fs_sampler_state = {};
		std::array<std::unique_ptr<rsx::sampled_image_descriptor_base>, rsx::limits::vertex_textures_count> vs_sampler_state = {};

		std::array<u32, 4> get_color_surface_addresses() const;
		u32 get_zeta_surface_address() const;

	protected:
		void get_framebuffer_layout(rsx::framebuffer_creation_context context, framebuffer_layout &layout);
		bool get_scissor(areau& region, bool clip_viewport);

		// Notify framebuffer layout has been committed.
		// FIXME: This should not be here
		void on_framebuffer_layout_updated();

		RSXVertexProgram current_vertex_program = {};
		RSXFragmentProgram current_fragment_program = {};

		vertex_program_texture_state current_vp_texture_state = {};
		fragment_program_texture_state current_fp_texture_state = {};

		program_cache_hint_t m_program_cache_hint;

		// Runs shader prefetch and resolves pipeline status flags
		void analyse_current_rsx_pipeline();

		// Prefetch and analyze the currently active fragment program ucode
		void prefetch_fragment_program();

		// Prefetch and analyze the currently active vertex program ucode
		void prefetch_vertex_program();

		// Update fragment program export configuration. Can invalidate the current program.
		rsx::flags32_t get_fragment_program_export_config();

		// Gets the current vertex program and associated state. Can invalidate the bound progam.
		void get_current_vertex_program(const std::array<std::unique_ptr<rsx::sampled_image_descriptor_base>, rsx::limits::vertex_textures_count>& sampler_descriptors);

		// Gets current fragment program and associated fragment state. Can invalidate the bound program.
		void get_current_fragment_program(const std::array<std::unique_ptr<rsx::sampled_image_descriptor_base>, rsx::limits::fragment_textures_count>& sampler_descriptors);

	public:
		bool invalidate_fragment_program(u32 dst_dma, u32 dst_offset, u32 size);
		void on_framebuffer_options_changed(u32 opt);

	public:
		u64 target_rsx_flip_time = 0;
		u64 int_flip_index = 0;
		u64 last_guest_flip_timestamp = 0;
		u64 last_host_flip_timestamp = 0;

		vm::ptr<void(u32)> flip_handler = vm::null;
		vm::ptr<void(u32)> user_handler = vm::null;
		vm::ptr<void(u32)> vblank_handler = vm::null;
		vm::ptr<void(u32)> queue_handler = vm::null;
		atomic_t<u64> vblank_count{0};
		bool capture_current_frame = false;

		u64 vblank_at_flip = umax;
		u64 flip_notification_count = 0;
		void post_vblank_event(u64 post_event_time);

	public:
		atomic_t<bool> sync_point_request = false;
		atomic_t<bool> pause_on_draw = false;
		bool in_begin_end = false;

		std::queue<desync_fifo_cmd_info> recovered_fifo_cmds_history;
		std::deque<frame_time_t> frame_times;
		u32 prevent_preempt_increase_tickets = 0;
		u64 preempt_fail_old_preempt_count = 0;

		atomic_t<s32> async_tasks_pending{ 0 };

		reports::conditional_render_eval cond_render_ctrl;

		virtual u64 get_cycles() = 0;
		virtual ~thread();

		static constexpr auto thread_name = "rsx::thread"sv;

	protected:
		thread(utils::serial* ar);

		thread() : thread(static_cast<utils::serial*>(nullptr)) {}

		virtual void on_task();
		virtual void on_exit();

		/**
		 * Execute a backend local task queue
		 */
		virtual void do_local_task(FIFO::state state);

		virtual void emit_geometry(u32) {}

		void run_FIFO();

	public:
		thread(const thread&) = delete;
		thread& operator=(const thread&) = delete;
		void save(utils::serial& ar);

		virtual void clear_surface(u32 /*arg*/) {}
		virtual void begin();
		virtual void end();
		virtual void execute_nop_draw();

		virtual void on_init_thread() = 0;
		virtual void on_frame_end(u32 buffer, bool forced = false);
		virtual void flip(const display_flip_info_t& info) = 0;
		virtual u64 timestamp();
		virtual bool on_access_violation(u32 /*address*/, bool /*is_writing*/) { return false; }
		virtual void on_invalidate_memory_range(const address_range32 & /*range*/, rsx::invalidation_cause) {}
		virtual void notify_tile_unbound(u32 /*tile*/) {}

		// control
		virtual void renderctl(u32 request_code, void* args);

		// zcull
		void notify_zcull_info_changed();
		void clear_zcull_stats(u32 type);
		void check_zcull_status(bool framebuffer_swap);
		void get_zcull_stats(u32 type, vm::addr_t sink);
		u32  copy_zcull_stats(u32 memory_range_start, u32 memory_range, u32 destination);

		void enable_conditional_rendering(vm::addr_t ref);
		void disable_conditional_rendering();
		virtual void begin_conditional_rendering(const std::vector<reports::occlusion_query_info*>& sources);
		virtual void end_conditional_rendering();

		// sync
		void sync();
		flags32_t read_barrier(u32 memory_address, u32 memory_range, bool unconditional);
		virtual void write_barrier(u32 /*memory_address*/, u32 /*memory_range*/) {}
		virtual void sync_hint(FIFO::interrupt_hint hint, reports::sync_hint_payload_t payload);
		virtual bool release_GCM_label(u32 /*type*/, u32 /*address*/, u32 /*value*/) { return false; }

	protected:

		/**
		 * Computes VRAM requirements needed to upload raw vertex streams
		 * result.first contains persistent memory requirements
		 * result.second contains volatile memory requirements
		 */
		std::pair<u32, u32> calculate_memory_requirements(const vertex_input_layout& layout, u32 first_vertex, u32 vertex_count);

		void evaluate_cpu_usage_reduction_limits();

	private:
		shared_mutex m_mtx_task;

		void handle_emu_flip(u32 buffer);
		void handle_invalidated_memory_range();

	public:

		draw_command_processor& GRAPH_frontend() { return m_draw_processor; }

		/**
		 * Notify that a section of memory has been mapped
		 * If there is a notify_memory_unmapped request on this range yet to be handled,
		 * handles it immediately.
		 */
		void on_notify_memory_mapped(u32 address_base, u32 size);

		/**
		 * Notify that a section of memory is to be unmapped
		 * Any data held in the defined range is discarded
		 * Sets optional unmap event data
		 */
		void on_notify_pre_memory_unmapped(u32 address_base, u32 size, std::vector<std::pair<u64, u64>>& event_data);

		/**
		 * Notify that a section of memory has been unmapped
		 * Any data held in the defined range is discarded
		 */
		void on_notify_post_memory_unmapped(u64 event_data1, u64 event_data2);

		/**
		 * Notify to check internal state during semaphore wait
		 */
		virtual void on_semaphore_acquire_wait() {}

#ifdef RPCS3_PS4
		// NV406E acquire that has blocked for `waited_us` (called from 200 us on, every poll): writes the deferred
		// ZCULL reports issued before it, which the label it waits for may depend on. First call of a wait: submit
		// the command buffer of the newest claimed report's query and poll (non-blocking); later calls poll; from
		// 50 ms on, one forced drain. `hinted` and `forced` belong to the wait and start false.
		// /data/rpcs3/rsx-no-report-drain.flag disables it. RSX thread only.
		void ps4_service_reports_while_blocked(u32 sema_addr, u64 waited_us, bool& hinted, bool& forced);

		// The same service while the FIFO is idle (empty or jump-to-self) for `idle_us`, called before each idle
		// sleep: from 2 ms on, one submit per idle episode and polls; from 50 ms on, one forced drain per episode.
		void ps4_service_reports_at_idle(u64 idle_us);

		// NV406E timeout note: whether ZCULL still keeps the label's page without access, and the report queue depth
		// ("label page ZCULL-locked yes|no; ZCULL queue: N pending ..."). RSX thread only.
		std::string ps4_label_zcull_text(u32 label_addr);

		// FIFO errors of this renderer (RSX thread); the first two write ps4_fifo_state_text() to startup.log.
		u32 m_ps4_fifo_errors = 0;

		// Set by a dead FIFO with /data/rpcs3/dead-fifo-stop.flag: the game stop was requested and run_FIFO fetches
		// nothing more (RSX thread).
		bool m_ps4_fifo_dead = false;

		// The "Frame skip is not compatible" error was logged for this boot (RSX thread).
		bool m_ps4_frame_skip_told = false;

		// Flip-time Resolution Scale sync (VKPresent.cpp, GLPresent.cpp): true when the configured scale differs from
		// the running one but must not be applied now, because MSAA is on (a live change with MSAA surfaces crashed
		// the RSX). The setting keeps the new value for Salvar and the next boot; one line and one toast per boot.
		// RSX thread.
		bool ps4_scale_change_deferred(const surface_scaling_config_t& wanted);
		bool m_ps4_scale_deferred_told = false;
#endif

		/**
		 * Load an image from memory with optional scaling and rotation.
		 * Returns false to tell the HW decoder to perform the operation on the CPU as a fallback when the operation cannot be safely accelerated.
		 */
		virtual bool scaled_image_from_memory(const blit_src_info& /*src_info*/, const blit_dst_info& /*dst_info*/, bool /*interpolate*/) { return false; }


		// Program public "get" handlers
		virtual std::pair<std::string, std::string> get_programs() const { return std::make_pair("", ""); }

		bool is_current_vertex_program_instanced() const { return !!(current_vertex_program.ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS); }

		virtual bool is_current_program_interpreted() const { return false; }

	public:
		void reset();
		void init(u32 ctrlAddress);

		// Emu App/Game flip, only immediately flips when called from rsxthread
		bool request_emu_flip(u32 buffer);

		void pause();
		void unpause();
		void wait_pause();

		// Get RSX approximate load in %
		u32 get_load();

		// Get stats object
		frame_statistics_t& get_stats() { return m_frame_stats; }

		// Returns true if the current thread is the active RSX thread
		inline bool is_current_thread() const
		{
			return !!cpu_thread::get_current<rsx::thread>();
		}
	};

	inline thread* get_current_renderer()
	{
		return g_fxo->try_get<rsx::thread>();
	}

	inline const backend_configuration& get_renderer_backend_config()
	{
		return g_fxo->get<rsx::thread>().get_backend_config();
	}

#ifdef RPCS3_PS4
	// 0.29: what the RSX thread is blocked on, for the frontend STALL dump. Kept as bits so
	// nested waits show together (e.g. flip+vk_fence). Only the RSX thread changes them; elsewhere a no-op.
	enum ps4_rsx_wait_reason : u32
	{
		ps4_wait_fifo_idle = 0,  // FIFO empty / jump-to-self host sleep
		ps4_wait_semaphore,      // NV406E semaphore acquire
		ps4_wait_flip,           // inside flip/present (frame fences, swapchain)
		ps4_wait_frame_limit,    // frame limiter sleep before a flip
		ps4_wait_zcull,          // ZCULL / occlusion query readback
		ps4_wait_cpu_wait,       // thread::cpu_wait (pause/suspend flags)
		ps4_wait_pause,          // thread::wait_pause (external pause handshake)
		ps4_wait_vk_fence,       // vk::wait_for_fence
		ps4_wait_vk_event,       // vk::wait_for_event
		ps4_wait_flush_request,  // waiting for an access-violation thread to finish its flush
		ps4_wait_reason_count
	};

	// Returns the previous bits (umax when the caller is not the RSX thread); pass them to ps4_rsx_wait_leave.
	u32 ps4_rsx_wait_enter(ps4_rsx_wait_reason reason) noexcept;
	void ps4_rsx_wait_leave(u32 previous) noexcept;

	class ps4_rsx_wait_scope
	{
		u32 m_previous;

	public:
		explicit ps4_rsx_wait_scope(ps4_rsx_wait_reason reason) noexcept
			: m_previous(ps4_rsx_wait_enter(reason))
		{
		}

		~ps4_rsx_wait_scope()
		{
			ps4_rsx_wait_leave(m_previous);
		}

		ps4_rsx_wait_scope(const ps4_rsx_wait_scope&) = delete;
		ps4_rsx_wait_scope& operator=(const ps4_rsx_wait_scope&) = delete;
	};

	// 0.29: one line for the frontend STALL dump: loop heartbeat age, last method/argument, wait bits and age.
	std::string ps4_rsx_wait_state() noexcept;

	// 0.29: true on the thread running rsx::thread::on_task.
	bool ps4_is_rsx_thread() noexcept;

	// RSXThread.cpp (0.26): true by default (RSX one step above the PPU/SPU threads); false with rsx-normal-priority.flag.
	bool ps4_rsx_priority_elevated() noexcept;

	// 0.29: FIFO-idle poll window of the raised RSX before its tick-quantized sleep
	// (/data/rpcs3/rsx-idle-poll-us.txt, 0 disables, default 200).
	u32 ps4_rsx_idle_poll_us() noexcept;

	// 0.34: true by default: NV406E semaphore waits step down to the guest priority, then sleep one kernel tick per
	// poll (ps4_semaphore_wait, nv406e.cpp). False with /data/rpcs3/rsx-sema-spin.flag (the 0.33 spin).
	bool ps4_rsx_sema_yield() noexcept;

	// 0.34: wait age in us after which an NV406E semaphore wait sleeps per poll (/data/rpcs3/rsx-sema-sleep-us.txt,
	// default 1000, 0 = never sleep). Cool mode lowers a non-zero value to 200. Read live.
	u32 ps4_rsx_sema_sleep_us() noexcept;

	// 0.34: flush requests the RSX thread has serviced (parks in ps4_wait_flush_request). A semaphore wait that sees
	// this change polls without sleeping again: a guest thread is faulting on RSX memory and waits for each service.
	u64 ps4_rsx_flush_services() noexcept;

	// 0.34: one startup.log line per NV406E wait longer than 1 s, at release or at timeout (at most 32 per process,
	// then only counted). A timeout also writes the label ring.
	void ps4_note_semaphore(u32 addr, u32 expected, u32 value, u32 get, u64 waited_us, u32 spu_compiles, u32 compile_slots, bool timed_out) noexcept;

	// 0.34: last 32 label operations of the RSX thread (NV406E acquire/release, NV4097 label writes) for the
	// STALL dump. Written by the RSX thread only (plain stores, no lock).
	enum ps4_label_op : u32
	{
		ps4_label_acquire = 0, // acquire found the value already there
		ps4_label_wait,        // acquire started waiting (value = expected)
		ps4_label_acquired,    // ... and the expected value arrived
		ps4_label_timeout,     // ... and gave up (TDR)
		ps4_label_release,     // label write (value = written)
		ps4_label_op_count
	};

	void ps4_label_ring_note(ps4_label_op op, u32 addr, u32 value, u32 get) noexcept;

	// Newest first, one line per entry, with the age and the current value of each label. Safe from any thread;
	// entries being rewritten while it runs may show mixed fields. Followed by the current values of labels
	// 0x48-0x5f, the last ZCULL queue summary and the report ring (ps4_report_ring_text).
	std::string ps4_label_ring_text();

	// True with the rsx_label_sudo feature (rsx-label-sudo.flag or features.txt; read at RSX start): the RSX thread reads
	// and writes semaphore labels through the unprotected (sudo) view, so a label access never faults on a page ZCULL
	// keeps without access for deferred reports (16 KiB pages hold the labels and the first report slots). False by
	// default: the label sites run the 0.38 guest-view expressions unchanged.
	bool ps4_rsx_label_sudo() noexcept;

	// sys_rsx.cpp: the last 8 guest sys_rsx_* calls with their arguments (display flip/queue/event attribute packages
	// only counted) and the SDK version the process reports. Safe from any thread.
	std::string ps4_sys_rsx_ring_text();

	// FIFO error and Dead FIFO records: get/put, whether the IO offset at get is mapped, the IO map (mapped MiB, IO 0,
	// first 4 entries), the context's system mode and ps4_sys_rsx_ring_text(). RSX thread.
	std::string ps4_fifo_state_text();

	// The FIFO puller read a PUT value (RSXFIFO.cpp, at each command cache refill): a change is kept in a 16-entry
	// ring with the GET it was read at and the time. Two loads and a compare otherwise. RSX thread.
	void ps4_note_put(u32 put, u32 get) noexcept;

	// The first FIFO desync and the Dead FIFO of a boot: the PUT ring (oldest first) and the 64 command words around
	// GET as the guest memory holds them now, in a few log lines ('what' names the event). RSX thread.
	void ps4_dump_fifo_desync(const char* what, u32 get);

	// Last 32 ZCULL report operations (NV4097 GET_REPORT and CLEAR_REPORT_VALUE) with their queue state and the
	// time each queued report reached memory. Written by the RSX thread only (plain stores, no lock).
	enum ps4_report_op : u32
	{
		ps4_report_get = 0,   // GET_REPORT (sink = report address)
		ps4_report_clear,     // CLEAR_REPORT_VALUE
		ps4_report_op_count
	};

	// `queued`: deferred behind a pending query or older reports (false: written at once). `st`: the queue after it.
	void ps4_report_ring_note(ps4_report_op op, u32 type, u32 sink, u32 get, bool queued, const reports::ps4_zcull_state& st) noexcept;

	// A queued report to `sink` reached memory (ZCULL_control::retire).
	void ps4_report_ring_retired(u32 sink) noexcept;

	// Publishes a ZCULL queue summary for dumps written by other threads (STALL).
	void ps4_zcull_publish(const reports::ps4_zcull_state& st) noexcept;

	// Newest first; the last published queue summary first. Safe from any thread.
	std::string ps4_report_ring_text();

	// 0.37: an RSX store into guest main memory (report, label, notify, CPU-path DMA or blit) bypasses the
	// reservations: break those of the touched 128-byte lines and wake their sleepers, as a snooped store does on HW
	// (vm::ps4_touch_reservations; RSX local memory is skipped). Call after the store and after any
	// rsx::reservation_lock scope, on the RSX thread. `kick` (16-byte report, label and notify writes) also counts the
	// line's sleepers before the touch and logs a kick that lands while an SPU readback is in flight.
	struct ps4_touch_result
	{
		u32 waiters = 0; // sleepers on the line before the touch (kick only)
		u32 woke = 0;    // touched lines that had sleepers
	};

	ps4_touch_result ps4_rsx_touch_reservations(u32 addr, u32 size, bool kick = false) noexcept;

	// Records the touch of the report or label just written at `addr` in its ring entry (RSX thread).
	void ps4_report_ring_touched(u32 sink, const ps4_touch_result& result) noexcept;
	void ps4_label_ring_touched(u32 addr, const ps4_touch_result& result) noexcept;

	// Render-pass closes by cause (ps4_rsx_counters_t::rp_close), tagged by the Vulkan backend.
	enum ps4_rp_close_cause : u32
	{
		ps4_rp_close_other = 0,      // untagged
		ps4_rp_close_barrier,        // vkutils/barriers.cpp image/buffer/memory barriers
		ps4_rp_close_texture_copy,   // texture copies, blits and layout changes
		ps4_rp_close_texture_cache,  // texture cache uploads and readbacks (VKTextureCache.cpp)
		ps4_rp_close_query,          // occlusion query pool work (VKQueryPool.cpp)
		ps4_rp_close_explicit,       // close_render_pass (flush, submit, clears)
		ps4_rp_close_pass_change,    // begin_renderpass with another pass or framebuffer
		ps4_rp_close_compute,        // compute dispatches
		ps4_rp_close_cause_count
	};

	// 0.29: counters read and reset by the 10 s 'RSX gameplay' line.
	struct ps4_rsx_counters_t
	{
		// RSX thread
		atomic_t<u64> idle_sleeps{0}, idle_sleep_us{0};   // FIFO-idle host sleeps (RSXFIFO.cpp)
		atomic_t<u64> sema_waits{0}, sema_wait_us{0};     // NV406E acquires that had to wait
		atomic_t<u64> sema_lowered{0};                    // ... that stepped down to the guest priority
		atomic_t<u64> sema_sleeps{0};                     // 0.34: sceKernelUsleep calls inside those waits
		atomic_t<u64> sema_extended{0};                   // 0.34: waits that outlived the TDR setting and went on
		// 0.34: NV406E timeouts. Never reset (the 10 s line prints the difference), so a reader can compare two
		// samples; sema_timeout_at is get_system_time() of the last one (0 = none yet).
		atomic_t<u64> sema_timeouts{0}, sema_timeout_at{0};
		atomic_t<u64> cpu_waits{0};
		atomic_t<u64> limit_waits{0}, limit_wait_us{0};   // frame limiter sleeps
		atomic_t<u64> hard_syncs{0};                      // VKGSRender::flush_command_queue(hard_sync)
		// Any thread
		atomic_t<u64> av_drains{0};                       // VKGSRender::on_access_violation queue drains
		atomic_t<u64> invisible_dma{0};                   // imp_flush: DMA vk::event unset, covered by a waited fence
		atomic_t<u64> dma_event_waits{0}, dma_event_wait_us{0}; // imp_flush: 100 ms-capped vk::event waits
		atomic_t<u64> event_timeouts{0};                  // vk::wait_for_event timeouts
		atomic_t<u64> fence_timeouts{0};                  // timed vk::wait_for_fence that returned VK_TIMEOUT
		// 0.34: draws dropped while their pipeline compiles (added per emulated flip, VKPresent.cpp) and the frames
		// that lost draws: presented, held back (present-hold-ms.txt), or selected by the hold rule (counted with a
		// reference budget while holding is off).
		atomic_t<u64> pipeline_skips{0}, frames_with_skips{0}, frames_held{0}, hold_candidates{0};
		// 0.34: asynchronous pipeline jobs (VKPipelineCompiler.cpp). pipe_pending is a gauge (queued or compiling)
		// and pipe_submitted is monotonic: neither is reset by the 10 s line. pipe_pending_max is the peak since
		// the last line, pipe_burst_max the peak since the burst tracker last saw an empty queue.
		atomic_t<u32> pipe_pending{0}, pipe_pending_max{0}, pipe_burst_max{0};
		atomic_t<u64> pipe_submitted{0};
		atomic_t<u64> pipe_fast_linked{0};                // linked without driver optimizations (vk-fast-pipelines.flag)
		// 0.34: image source of each presented frame (VKPresent.cpp get_present_source): render target, texture
		// cache, CPU memory upload; switches = frames whose source kind differs from the previous frame.
		atomic_t<u64> present_rt{0}, present_tex{0}, present_cpu{0}, present_switches{0};
		atomic_t<u64> surface_tag_misses{0};              // surface_store.h: surfaces whose memory tag test failed
		// Deferred ZCULL reports serviced while the RSX waits (RSX thread): NV406E waits given a submit and polls,
		// forced drains in waits of 50 ms or more and the report writes they retired, the same at FIFO idle, and the
		// command buffers those services submitted (a wait whose queries were already submitted needs none).
		atomic_t<u64> report_hints{0}, report_forced{0}, report_forced_written{0};
		atomic_t<u64> report_idle_hints{0}, report_idle_forced{0}, report_hint_submits{0};
		// Filled by the Vulkan backend on the RSX thread, on every frame (not sampled); the 10 s line divides by
		// guest frames. rp_close: render-pass closes by ps4_rp_close_cause. Vertex cache: weak per-frame cache
		// lookups and misses for layouts of one interleaved block (single) and of more blocks (multi). index_bytes:
		// converted index data uploaded. tex_uploads/tex_upload_misses: texture cache upload calls and misses.
		std::array<atomic_t<u64>, ps4_rp_close_cause_count> rp_close{};
		atomic_t<u64> vtx_cache_single_requests{0}, vtx_cache_single_misses{0};
		atomic_t<u64> vtx_cache_multi_requests{0}, vtx_cache_multi_misses{0};
		atomic_t<u64> index_bytes{0};
		atomic_t<u64> tex_uploads{0}, tex_upload_misses{0};
	};

	extern ps4_rsx_counters_t g_ps4_rsx_counters;

	// 0.34: draws dropped in the current frame because their pipeline was not compiled yet (VKDraw.cpp). Only the
	// RSX thread touches it: incremented per dropped draw, read and reset at each emulated flip (VKPresent.cpp).
	extern u32 ps4_frame_skips;

	// 0.34: counts a failed surface memory tag test (declared again in Common/surface_store.h).
	void ps4_note_surface_tag_miss() noexcept;

	// 0.34: RSX phase telemetry for the 10 s line. Only the RSX thread writes these (ps4_rsx_phase_add: a plain
	// load and store, no locked instruction); TSC ticks are converted once per line.
	struct ps4_rsx_phase_t
	{
		atomic_t<bool> on{true};         // false with /data/rpcs3/rsx-profiler-off.flag
		// The phase timers (ps4_rsx_phase_scope) and the per-draw profiler run in this frame: on, 1 frame of 8.
		atomic_t<bool> sampled{true};
		atomic_t<u64> wait_ticks{0};     // inside the outermost ps4_rsx_wait_scope (any reason, the whole flip too); every frame, never reset
		atomic_t<u64> fifo_ticks{0};     // FIFO method packets (run_FIFO decode loop and handlers), waits excluded
		atomic_t<u64> methods{0};        // FIFO methods decoded (every frame)
		atomic_t<u64> begin_ticks{0};    // VKGSRender::begin (init_buffers), waits excluded
		atomic_t<u64> render_passes{0};  // VKGSRender::begin_render_pass calls for draws (every frame while on)
		atomic_t<u64> upload_bytes{0};   // dma_manager::copy inline bytes (always counted)
		// Method handlers inside fifo_ticks (sampled frames): NV4097 clear and GET_REPORT/CLEAR_REPORT_VALUE,
		// NV3089 image_in, NV0039 buffer_notify, NV406E and NV4097 label releases, transform constant and program loads.
		atomic_t<u64> clear_ticks{0}, report_ticks{0}, image_in_ticks{0}, notify_ticks{0}, release_ticks{0}, transform_ticks{0};
	};

	extern ps4_rsx_phase_t g_ps4_rsx_phase;

	// RSXOffload.cpp (0.34): true when the inline dma_manager::copy writes copies of 4 KiB or more with streaming
	// stores (default); false with /data/rpcs3/rsx-plain-copy.flag. Read at dma_manager::init.
	bool ps4_rsx_streaming_copy() noexcept;

	// RSXOffload.cpp (0.37): finishes the vertex copies queued on the RSX copy worker (RSX thread: runs the unclaimed
	// ones itself, then waits for the worker's). Called before the RSX writes memory a guest thread or a later copy
	// can observe (labels, reports, notifies, REF, CPU-path transfers, ZCULL page locks, external pauses) or signals
	// the guest (thread::sync, flips), so the guest never sees such a write before earlier draws read their vertex
	// sources. Only from RSX method, idle and pause contexts (never inside a submit). Two loads when nothing is queued.
	void ps4_copy_worker_barrier() noexcept;

	// RSXOffload.cpp (0.37): the 10 s line field "; copies: worker KiB/frame=..." (empty while the worker is off);
	// reads and resets the window counters. RSX thread.
	std::string ps4_copy_worker_text(double frames);

	// RSXZCULL.cpp (0.37): true when [addr, addr + size) overlaps a page ZCULL keeps without access for deferred
	// reports (or when that cannot be told). RSX thread: such copies stay off the copy worker.
	bool ps4_zcull_range_locked(u32 addr, u32 size) noexcept;

	// Common/BufferUtils.cpp (0.37): source indices per path since the last call (RSX thread): asmjit SIMD (no restart
	// or strip restart), skip-restart (disjoint primitives with restart), CPU expansion (fans, polygons, quads).
	// `sse41`: the skip-restart path uses the SSE4.1 kernels.
	void ps4_take_index_path_counts(u64& simd, u64& skip_restart, u64& expand, bool& sse41) noexcept;

	inline void ps4_rsx_phase_add(atomic_t<u64>& counter, u64 value) noexcept
	{
		counter.release(counter.observe() + value);
	}

	// Adds the TSC ticks of its lifetime, minus the RSX waits inside it, to one phase counter (sampled frames only).
	class ps4_rsx_phase_scope
	{
		atomic_t<u64>* m_ticks = nullptr;
		u64 m_t0 = 0;
		u64 m_w0 = 0;

	public:
		explicit ps4_rsx_phase_scope(atomic_t<u64>& ticks) noexcept
		{
			if (g_ps4_rsx_phase.sampled.observe())
			{
				m_ticks = &ticks;
				m_w0 = g_ps4_rsx_phase.wait_ticks.observe();
				m_t0 = utils::get_tsc();
			}
		}

		~ps4_rsx_phase_scope()
		{
			if (m_ticks)
			{
				const u64 spent = utils::get_tsc() - m_t0;
				const u64 waited = g_ps4_rsx_phase.wait_ticks.observe() - m_w0;
				ps4_rsx_phase_add(*m_ticks, spent > waited ? spent - waited : 0);
			}
		}

		ps4_rsx_phase_scope(const ps4_rsx_phase_scope&) = delete;
		ps4_rsx_phase_scope& operator=(const ps4_rsx_phase_scope&) = delete;
	};

	// 0.29: last FIFO method (register << 2) and argument handed to a method handler (RSXFIFO.cpp, RSX thread).
	extern atomic_t<u32> g_ps4_rsx_last_method;
	extern atomic_t<u32> g_ps4_rsx_last_arg;
#endif
}
