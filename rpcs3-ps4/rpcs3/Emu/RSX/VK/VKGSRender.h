#pragma once

#include "upscalers/upscaling.h"

#include "vkutils/descriptors.h"
#include "vkutils/data_heap.h"
#include "vkutils/ex.h"
#include "vkutils/instance.h"
#include "vkutils/sync.h"
#include "vkutils/swapchain.h"

#include "VKGSRenderTypes.hpp"
#include "VKTextureCache.h"
#include "VKRenderTargets.h"
#include "VKFormats.h"
#include "VKOverlays.h"
#include "VKProgramBuffer.h"
#include "VKFramebuffer.h"
#include "VKShaderInterpreter.h"
#include "VKQueryPool.h"

#include "Emu/RSX/GSRender.h"
#include "Emu/RSX/Host/RSXDMAWriter.h"
#include <functional>
#include <initializer_list>
#ifdef RPCS3_PS4
#include "util/ps4_vram_policy.hpp"
#include <set>
#endif

using namespace vk::vmm_allocation_pool_; // clang workaround.
using namespace vk::upscaling_flags_;     // ditto

using vs_binding_table_t = decltype(VKVertexProgram::binding_table);
using fs_binding_table_t = decltype(VKFragmentProgram::binding_table);

namespace vk
{
	using host_data_t = rsx::host_gpu_context_t;

#ifdef RPCS3_PS4
	// vkutils/data_heap.cpp (0.39): replaces the ring's buffer by one of new_size bytes (built before the old one goes to
	// the garbage collector; a refusal leaves the ring as it is) and empties the ring. Only right after a hard sync,
	// when nothing of the ring is in use. True when the ring has the new size.
	bool ps4_resize_ring(data_heap& ring, usz new_size);

	// vkutils/sync.cpp (0.39): stops the game after a confirmed GPU hang (no-hang-stop.flag: no-op). Any thread.
	void ps4_request_gpu_hang_stop(const char* why) noexcept;

	// VKGSRender.cpp: the GPU liveness scan, from an RSX-thread wait on the GPU that has lasted a second or more
	// (occlusion query results, timed event waits), so a submission the GPU never finishes is confirmed while the RSX
	// thread is blocked on it. No-op on other threads; the scan itself runs at most once a second.
	void ps4_gpu_liveness_in_wait() noexcept;

	// VKPipelineCompiler.cpp (0.34): asynchronous pipeline jobs queued or compiling right now.
	u32 ps4_pipelines_pending() noexcept;

	// VKShaderInterpreter.cpp (0.37): pipelines the newest interpreter preload linked, and the interpreter variants
	// linked synchronously on first use since boot (returned; their total link time in total_us). Any thread.
	u32 ps4_interpreter_preload_pipelines() noexcept;
	u64 ps4_interpreter_first_use_links(u64& total_us) noexcept;

	// 0.37: RSX-thread time inside RADV calls for the 10 s 'Vulkan backend' line ('radv us/frame'). Timed only in the
	// frames the RSX phase timers sample (1 of 8, off with rsx-profiler-off.flag), RSX waits excluded, outermost scope
	// only; other threads are never timed.
	enum ps4_radv_site : u32
	{
		ps4_radv_draw = 0,     // vkCmdDraw* and the index buffer bind (emit_geometry)
		ps4_radv_bind,         // pipeline and descriptor set binds with their descriptor writes, push constants, dynamic state
		ps4_radv_render_pass,  // VKGSRender::begin_render_pass / close_render_pass
		ps4_radv_barrier,      // render target write barriers before a draw
		ps4_radv_descriptors,  // descriptor writes deferred to the submit (vk::descriptors::flush in queue_submit)
		ps4_radv_texture_copy, // texture cache image copies and blits (copy_transfer_regions_impl)
		ps4_radv_site_count
	};

	// Out of line, reached only in sampled frames. begin returns false when the scope is not timed.
	bool ps4_radv_scope_begin(u64& t0, u64& w0) noexcept;
	void ps4_radv_scope_end(ps4_radv_site site, u64 t0, u64 w0) noexcept;

	class ps4_radv_scope
	{
		u64 m_t0 = 0;
		u64 m_w0 = 0;
		ps4_radv_site m_site;
		bool m_on = false;

	public:
		explicit ps4_radv_scope(ps4_radv_site site) noexcept
			: m_site(site)
		{
			if (rsx::g_ps4_rsx_phase.sampled.observe()) [[unlikely]]
			{
				m_on = ps4_radv_scope_begin(m_t0, m_w0);
			}
		}

		~ps4_radv_scope()
		{
			if (m_on) [[unlikely]]
			{
				ps4_radv_scope_end(m_site, m_t0, m_w0);
			}
		}

		ps4_radv_scope(const ps4_radv_scope&) = delete;
		ps4_radv_scope& operator=(const ps4_radv_scope&) = delete;
	};

	// 0.37: per-frame counts for the same line. RSX thread only.
	struct ps4_vk_draw_counters_t
	{
		u64 frames = 0;            // guest frames with at least one draw (VKGSRender::begin)
		u64 sampled_frames = 0;    // ... of which the RSX phase timers sampled (the radv scopes ran)
		u64 xform_full_bytes = 0;  // transform constants uploaded as the whole 8 KiB block (interpreter, indexed constants)
		u64 xform_reloc_bytes = 0; // ... as the constants the program uses
		u64 xform_uploads = 0;
		u64 interpreter_draws = 0; // draws whose pipeline was still compiling, drawn with the shader interpreter
		u64 skipped_not_ready = 0;   // 0.38: draws skipped without a program (compiling, or refused by the driver)
		u64 skipped_descriptors = 0; // 0.38: draws skipped because a descriptor set was refused for lack of memory
		u64 submits = 0;             // primary command buffers submitted (close_and_submit_command_buffer)
		u64 cb_splits = 0;           // ... of which ended by the command buffer split after a device loss
	};

	extern ps4_vk_draw_counters_t g_ps4_vk_draw_counters;

	// Draws made with the shader interpreter since the process started (never reset; the pipeline burst line takes
	// differences). RSX thread only.
	extern u64 g_ps4_interpreter_draws_total;

	// VKQueryPool.cpp: the occlusion query pool could not be created after a memory reclaim and one retry; the
	// renderer runs without occlusion queries and every one answers visible. Any thread.
	bool ps4_query_pool_unavailable() noexcept;
#endif
}

class VKGSRender : public GSRender, public ::rsx::reports::ZCULL_control
{
private:
	enum frame_context_state : u32
	{
		dirty = 1
	};

	enum flush_queue_state : u32
	{
		ok = 0,
		flushing = 1,
		deadlock = 2
	};

private:
	const VKFragmentProgram *m_fragment_prog = nullptr;
	const VKVertexProgram *m_vertex_prog = nullptr;
	vk::glsl::program *m_program = nullptr;
	vk::glsl::program *m_prev_program = nullptr;
	vk::pipeline_props m_pipeline_properties;

	const vs_binding_table_t* m_vs_binding_table = nullptr;
	const fs_binding_table_t* m_fs_binding_table = nullptr;

	vk::texture_cache m_texture_cache;
	vk::surface_cache m_rtts;

	std::unique_ptr<vk::buffer> null_buffer;
	std::unique_ptr<vk::buffer_view> null_buffer_view;

	std::unique_ptr<vk::upscaler> m_upscaler;
	output_scaling_mode m_output_scaling{output_scaling_mode::bilinear};

	std::unique_ptr<vk::buffer> m_cond_render_buffer;
	u64 m_cond_render_sync_tag = 0;

	shared_mutex m_sampler_mutex;
	atomic_t<bool> m_samplers_dirty = { true };
	std::unique_ptr<vk::sampler> m_stencil_mirror_sampler;
	std::array<vk::sampler*, rsx::limits::fragment_textures_count> fs_sampler_handles{};
	std::array<vk::sampler*, rsx::limits::vertex_textures_count> vs_sampler_handles{};

	std::unique_ptr<vk::buffer_view> m_persistent_attribute_storage;
	std::unique_ptr<vk::buffer_view> m_volatile_attribute_storage;

	VkDependencyInfoKHR m_async_compute_dependency_info {};
	VkMemoryBarrier2KHR m_async_compute_memory_barrier {};

	std::pair<const vs_binding_table_t*, const fs_binding_table_t*> get_binding_table() const;

public:
	//vk::fbo draw_fbo;
	std::unique_ptr<vk::vertex_cache> m_vertex_cache;
	std::unique_ptr<vk::shader_cache> m_shaders_cache;

private:
	std::unique_ptr<vk::program_cache> m_prog_buffer;

	std::unique_ptr<vk::swapchain_base> m_swapchain;
	vk::instance m_instance;
	vk::render_device *m_device;

	//Vulkan internals
	std::unique_ptr<vk::query_pool_manager> m_occlusion_query_manager;
	bool m_occlusion_query_active = false;
	rsx::reports::occlusion_query_info *m_active_query_info = nullptr;
	std::vector<vk::occlusion_data> m_occlusion_map;

	shared_mutex m_secondary_cb_guard;
	vk::command_pool m_secondary_command_buffer_pool;
	vk::command_buffer_chain<VK_MAX_ASYNC_CB_COUNT> m_secondary_cb_list;

	vk::command_pool m_command_buffer_pool;
	vk::command_buffer_chain<VK_MAX_ASYNC_CB_COUNT> m_primary_cb_list;
	vk::command_buffer_chunk* m_current_command_buffer = nullptr;

	std::unique_ptr<vk::buffer> m_host_object_data;
	vk::framebuffer_holder* m_draw_fbo = nullptr;

	sizeu m_swapchain_dims{};
	bool swapchain_unavailable = false;
	bool should_reinitialize_swapchain = false;

	u64 m_last_heap_sync_time = 0;
	u32 m_texbuffer_view_size = 0;

	vk::data_heap m_attrib_ring_info;                         // Vertex data
	vk::data_heap m_fragment_constants_ring_info;             // Fragment program constants
	vk::data_heap m_transform_constants_ring_info;            // Transform program constants
	vk::data_heap m_fragment_env_ring_info;                   // Fragment environment params
	vk::data_heap m_vertex_env_ring_info;                     // Vertex environment params
	vk::data_heap m_fragment_texture_params_ring_info;        // Fragment texture params
	vk::data_heap m_vertex_layout_ring_info;                  // Vertex layout structure
	vk::data_heap m_index_buffer_ring_info;                   // Index data
	vk::data_heap m_texture_upload_buffer_ring_info;          // Texture upload heap
	vk::data_heap m_raster_env_ring_info;                     // Raster control such as polygon and line stipple
	vk::data_heap m_instancing_buffer_ring_info;              // Instanced rendering data (constants indirection table + instanced constants)

	vk::data_heap m_fragment_instructions_buffer;             // Interpreter FP block
	vk::data_heap m_vertex_instructions_buffer;               // Interpreter VP block

	rsx::simple_array<vk::data_heap*> m_flushable_data_heaps; // List of heaps that can be 'dirty' and need manual flush

	VkDescriptorBufferInfoEx m_vertex_env_buffer_info {};
	VkDescriptorBufferInfoEx m_fragment_env_buffer_info {};
	VkDescriptorBufferInfoEx m_vertex_layout_stream_info {};
	VkDescriptorBufferInfoEx m_vertex_constants_buffer_info {};
	VkDescriptorBufferInfoEx m_fragment_constants_buffer_info {};
	VkDescriptorBufferInfoEx m_fragment_texture_params_buffer_info {};
	VkDescriptorBufferInfoEx m_raster_env_buffer_info {};
	VkDescriptorBufferInfoEx m_instancing_indirection_buffer_info {};
	VkDescriptorBufferInfoEx m_instancing_constants_array_buffer_info{};

	VkDescriptorBufferInfoEx m_vertex_instructions_buffer_info {};
	VkDescriptorBufferInfoEx m_fragment_instructions_buffer_info {};

	rsx::simple_array<u8> m_multidraw_parameters_buffer;
	u64 m_xform_constants_dynamic_offset = 0;          // We manage transform_constants dynamic offset manually to alleviate performance penalty of doing a hot-patch of constants.
	u64 m_vertex_env_dynamic_offset = 0;
	u64 m_vertex_layout_dynamic_offset = 0;
	u64 m_fragment_constants_dynamic_offset = 0;
	u64 m_fragment_env_dynamic_offset = 0;
	u64 m_texture_parameters_dynamic_offset = 0;
	u64 m_stipple_array_dynamic_offset = 0;

	std::unique_ptr<rsx::data_heap::bulk_allocator<256, 96>> m_vertex_env_allocator;
	std::unique_ptr<rsx::data_heap::bulk_allocator<256, 16>> m_transform_constants_allocator;
	std::unique_ptr<rsx::data_heap::bulk_allocator<256, 16>> m_fragment_constants_allocator;

	std::vector<vk::frame_context_t> m_frame_context_storage;
	u32 m_max_async_frames = 0u;
	// Temp frame context to use if the real frame queue is overburdened. Only used for storage
	vk::frame_context_t m_aux_frame_context;

	u32 m_current_queue_index = 0;
	vk::frame_context_t* m_current_frame = nullptr;
	std::deque<vk::frame_context_t*> m_queued_frames;

	VkViewport m_viewport {};
	VkRect2D m_scissor {};

	std::vector<u8> m_draw_buffers;

	shared_mutex m_flush_queue_mutex;
	vk::flush_request_task m_flush_requests;
#ifdef RPCS3_PS4
	// 0.32: true only while the RSX thread waits in do_local_task for faulting threads to finish their flush. The
	// flush request can also be cleared by a hard sync that does not park (flush_command_queue(true), ZCULL reads),
	// so a faulting thread cannot assume the RSX thread is idle just because its request was served.
	atomic_t<bool> m_ps4_rsx_parked_for_flush{ false };

	// 0.34: frames that lost draws to compiling pipelines (VKPresent.cpp, RSX thread only). The hold rule runs on
	// every emulated flip; without present-hold-ms.txt it only counts the frames it would hold.
	u64 m_ps4_hold_budget_us = umax;          // umax: present-hold-ms.txt not read yet; 0: holding off
	u64 m_ps4_hold_us = 0;                    // time the display has kept the last complete image in this run
	u64 m_ps4_last_flip_us = 0;
	bool m_ps4_last_present_complete = false; // the image on screen had no dropped draw
	bool m_ps4_holding = false;               // the previous emulated flip was held (or would have been)
	u32 m_ps4_last_present_src = umax;        // 0 render target, 1 texture cache, 2 CPU memory

	// 0.34: asynchronous pipeline bursts (the compile queue leaves and reaches empty again), sampled per flip.
	struct ps4_pipeline_burst_t
	{
		bool active = false;
		u64 start_us = 0;             // last emulated flip that saw an empty queue
		u64 submitted_at_start = 0;   // rsx::g_ps4_rsx_counters.pipe_submitted at that flip
		u64 frames_with_skips = 0;
		u32 worst_skips = 0;
		u32 worst_draws = 0;
		u32 notes = 0;                // startup.log lines written by this renderer
		// RPCS3.log burst line: draws skipped in the burst, and the interpreter links, their time and the interpreter
		// draws counted since the last empty queue.
		u64 skipped_total = 0;
		u64 links_at_start = 0;
		u64 link_us_at_start = 0;
		u64 interp_draws_at_start = 0;
		u32 log_notes = 0;            // RPCS3.log lines written by this renderer
	} m_ps4_burst;

	// Command buffer split after a device loss of this game (cb_split_after_loss and the title's cb_split adapt key,
	// read at renderer creation): a submit after this many draws (0 = off), and the draws recorded since the last
	// submit. RSX thread only.
	u32 m_ps4_cb_split_draws = 0;
	u32 m_ps4_cb_draws = 0;

	// 0.36: the managed rings (the interpreter blocks only when created), checked before every draw so that a ring
	// within its guard reclaims space (queued frames, then a hard sync) instead of growing. RSX thread only.
	std::array<vk::data_heap*, 13> m_ps4_rings{};
	bool m_ps4_ring_reclaim = false; // ring-reclaim-off.flag absent
	u32 m_ps4_ring_reclaims = 0;
	u32 m_ps4_ring_hard_syncs = 0;

	// 0.39: adaptive ring growth (util/ps4_vram_policy.hpp ring rules), in m_ps4_rings order: reclaim rate, largest
	// size and the earliest retry after a refused growth. m_ps4_ring_growth = feature ring_growth (off by default). RSX
	// thread only.
	bool m_ps4_ring_growth = false;
	std::array<utils::ps4::vram::ring_rate, 13> m_ps4_ring_rate{};
	std::array<u32, 13> m_ps4_ring_max_mib{};
	std::array<u64, 13> m_ps4_ring_retry_us{};
	u32 m_ps4_ring_growths = 0;
	u32 m_ps4_ring_growth_skips = 0; // growths the window or the allocator did not admit
	std::string m_ps4_ring_title; // title whose ring record the growths update

	// 0.39: the auxiliary upload heap grows by the same rule, fed by the uploads that did not fit it (own staging
	// buffer): the count already seen, its rate and the earliest retry after a refused growth. RSX thread only.
	u32 m_ps4_aux_staging_seen = 0;
	utils::ps4::vram::ring_rate m_ps4_aux_rate{};
	u64 m_ps4_aux_retry_us = 0;

	// 0.36: GPU liveness from the primary command buffers' fences (do_local_task, about once per second): per buffer,
	// the reset id of the submission seen pending and when it was first seen.
	struct ps4_pending_cb_t
	{
		u64 reset_id = 0;
		u64 first_seen_us = 0;
	};
	std::array<ps4_pending_cb_t, VK_MAX_ASYNC_CB_COUNT> m_ps4_pending_cbs{};
	u64 m_ps4_next_liveness_scan_us = 0;
	bool m_ps4_unfinished_logged = false;
	u64 m_ps4_foreground_since_us = 0; // 0.39: start of the current foreground stretch seen by the scan (0 = background)
	u32 m_ps4_liveness_epoch = 0;      // ps4_focus_epoch() at the last scan: a change (process away) starts the table over

	// 0.36: texture-cache claims of pages ZCULL still protects for deferred reports (any thread).
	atomic_t<u32> m_ps4_zcull_pages_claimed{ 0 };

	// 0.39: CPU faults that found the RSX thread ended while their flush request was pending (any thread). Once set,
	// later faults do not post a request (on_access_violation answers "not handled").
	atomic_t<u32> m_ps4_av_dead_rsx{ 0 };
	// m_flush_requests.producer_wait() bounded by the RSX thread's life: false = the RSX thread ended (thread fatal or
	// return) with the request still pending.
	bool ps4_wait_flush_served(u32 address);

	// 0.36: release of idle descriptor pools every 120 frames; 0 = no-pool-release.flag not read yet, 1 = on, 2 = off.
	u32 m_ps4_pool_sweep_frames = 0;
	u32 m_ps4_pool_release_state = 0;

	// 0.37: window of the 10 s 'Vulkan backend' line (do_local_task, RSX thread): start time and the monotonic
	// counters at that time.
	u64 m_ps4_vk_report_start_us = 0;
	u64 m_ps4_vk_report_links = 0;
	u64 m_ps4_vk_report_link_us = 0;
	u64 m_ps4_vk_report_spills = 0;
	// 0.39: new shader programs translated on the RSX thread (count, us), on the pipeline workers, taken back.
	u64 m_ps4_vk_report_translate = 0;
	u64 m_ps4_vk_report_translate_us = 0;
	u64 m_ps4_vk_report_translate_async = 0;
	u64 m_ps4_vk_report_translate_back = 0;

	// 0.38: memory shedding of the program caches (ps4_shed_programs, do_local_task about once per second): next
	// check, shedding on, last descriptor pool release request, times shedding started.
	u64 m_ps4_next_shed_check_us = 0;
	u64 m_ps4_last_shed_release_us = 0;
	bool m_ps4_shedding = false;
	u32 m_ps4_shed_runs = 0;
	void ps4_shed_programs();

	// 0.38: deep reclaim callback (utils::ps4::vram::register_reclaimer, RSX thread): shader interpreter cap and the
	// descriptor pools of program tables not committed since the previous sweep. Estimated bytes freed.
	static u64 ps4_program_reclaimer(int level) noexcept;

	// 0.38: GPU window policy (util/ps4_vram_policy.hpp), RSX thread only. Age-based texture eviction: state of
	// texture-age-evict-off.flag (0 = not read, 1 = on, 2 = off), the last run and the runs; DMA mirror sweep frames;
	// deep reclaims run and the guard against running one inside another.
	u32 m_ps4_age_evict_state = 0;
	u64 m_ps4_last_age_eviction_us = 0;
	u32 m_ps4_age_evictions = 0;
	u32 m_ps4_dma_sweep_frames = 0;
	u32 m_ps4_dma_age_out_state = 0; // dma-age-out-off.flag: 0 = not read, 1 = on, 2 = off
	u32 m_ps4_deep_reclaims = 0;
	bool m_ps4_in_deep_reclaim = false;
	bool m_ps4_upload_staging = true; // upload-staging-off.flag absent

	// Addresses of the textures the current draw binds (never evicted).
	std::set<u32> ps4_bound_texture_addresses() const;
	// Severe trims with the window at 90% or more (or the textures over the quota): textures not looked up for 300
	// frames, least recently used first, until the load would fall to 85%; at most once per second. Bytes evicted.
	u64 ps4_evict_textures_by_age();
	// DMA mirror blocks: idle for 30 s, or above 64 MiB the least recently used idle for 2 s ('deep': every unpinned
	// block idle for 2 s). Bytes released.
	u64 ps4_sweep_dma(bool deep);

	void ps4_check_ring_heaps();
	// Waits for everything and starts the rings over (before a draw allocates anything).
	void ps4_restart_rings();
	// Grows the auxiliary upload heap when uploads that did not fit it come often (from ps4_check_ring_heaps).
	void ps4_check_aux_upload_heap();
	void ps4_check_gpu_liveness();
	void ps4_report_vk_backend();
#endif

	ullong m_last_cond_render_eval_hint = 0;

	// Offloader thread deadlock recovery
	rsx::atomic_bitmask_t<flush_queue_state> m_queue_status;
	utils::address_range32 m_offloader_fault_range;
	rsx::invalidation_cause m_offloader_fault_cause;

	vk::draw_call_t m_current_draw {};
	u64 m_current_renderpass_key = 0;
	VkRenderPass m_cached_renderpass = VK_NULL_HANDLE;
	std::vector<vk::image*> m_fbo_images;

	std::unique_ptr<vk::image> m_overlay_recording_img;

	//Vertex layout
	rsx::vertex_input_layout m_vertex_layout;

	vk::shader_interpreter m_shader_interpreter;
	u32 m_interpreter_state;

#if defined(HAVE_X11) && defined(HAVE_VULKAN)
	Display *m_display_handle = nullptr;
#endif

public:
	u64 get_cycles() final;
	~VKGSRender() override;

	VKGSRender(utils::serial* ar) noexcept;
	VKGSRender() noexcept : VKGSRender(nullptr) {}

private:
	void prepare_rtts(rsx::framebuffer_creation_context context);

	void close_and_submit_command_buffer(
		vk::fence* fence = nullptr,
		VkSemaphore wait_semaphore = VK_NULL_HANDLE,
		VkSemaphore signal_semaphore = VK_NULL_HANDLE,
		VkPipelineStageFlags pipeline_stage_flags = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	void flush_command_queue(bool hard_sync = false, bool do_not_switch = false);
	void queue_swap_request();
	void frame_context_cleanup(vk::frame_context_t *ctx);
	void advance_queued_frames();
	void present(vk::frame_context_t *ctx);
	bool reinitialize_swapchain();

	vk::viewable_image* get_present_source(vk::present_surface_info* info, const rsx::avconf& avconfig);

	void begin_render_pass();
	void close_render_pass();
	VkRenderPass get_render_pass();
	void invalidate_render_pass();

	void update_draw_state();
	void check_present_status();

	vk::vertex_upload_info upload_vertex_data();
	rsx::simple_array<u8> m_scratch_mem;

	bool load_program();
	void load_program_env();
	void update_vertex_env(u32 id, const vk::vertex_upload_info& vertex_info);
	void upload_transform_constants(const rsx::io_buffer& buffer);

	void load_texture_env();
	bool bind_texture_env();
	bool bind_interpreter_texture_env();

public:
	void init_buffers(rsx::framebuffer_creation_context context, bool skip_reading = false);
	void set_viewport();
	void set_scissor(bool clip_viewport);
	void bind_viewport();

	// Sync
	void write_barrier(u32 address, u32 range) override;
	void sync_hint(rsx::FIFO::interrupt_hint hint, rsx::reports::sync_hint_payload_t payload) override;
	bool release_GCM_label(u32 type, u32 address, u32 data) override;

	void begin_occlusion_query(rsx::reports::occlusion_query_info* query) override;
	void end_occlusion_query(rsx::reports::occlusion_query_info* query) override;
	bool check_occlusion_query_status(rsx::reports::occlusion_query_info* query) override;
	void get_occlusion_query_result(rsx::reports::occlusion_query_info* query) override;
	void discard_occlusion_query(rsx::reports::occlusion_query_info* query) override;

	// External callback in case we need to suddenly submit a commandlist unexpectedly, e.g in a violation handler
	void emergency_query_cleanup(vk::command_buffer* commands);

	// External callback to handle out of video memory problems
	bool on_vram_exhausted(rsx::problem_severity severity);

#ifdef RPCS3_PS4
	// 0.32: submits the frame recorded so far, waits for it and restarts the same command buffer, so that an RSX-side
	// readback is recorded and submitted alone (texture_cache::prepare_for_dma_transfers). Does nothing unless cmd is
	// the current primary buffer and recording, the caller is the RSX thread outside a submit, and dma.flag isolate=on.
	void ps4_submit_frame_before_dma(vk::command_buffer& cmd);

	// 0.32: memory recovery that is safe off the RSX thread and inside uninterruptible sections: waits for (RSX
	// thread) or polls (a faulting thread while the RSX thread is parked for its flush) submitted work and lets the
	// garbage collector free what it released. Touches no cache. True when work completed or memory was freed, so
	// a failed allocation is worth retrying.
	bool ps4_retire_completed_work();

	// 0.34 (VKPresent.cpp, emulated flips only): takes the frame's dropped-draw count, updates the counters and the
	// burst tracker, and returns true when the frame must not be presented (present-hold-ms.txt).
	bool ps4_hold_incomplete_frame(const rsx::display_flip_info_t& info);
	void ps4_track_pipeline_burst(u64 now, u32 skipped, u32 draws);

	// 0.36 (vk::data_heap::grow, ring reclaim on): makes room in one of this renderer's rings without growing it.
	// Only on the RSX thread outside uninterruptible sections and submits: a hard sync that keeps the command buffer
	// and retires every queued frame. True when it ran.
	bool ps4_make_ring_room(rsx::data_heap& heap, usz size);

	// 0.38 (vkutils/memory.cpp through vk::ps4_vram_deep_reclaim): a required allocation failed after the pressure
	// pass. Hard sync, then every texture not bound (regardless of the quota), unreleased/unlocked sections, reusable
	// images, temporary subresources, stale surfaces, idle DMA mirrors and the registered reclaimers; a second hard sync
	// lets the garbage collector free it all. Same conditions as a fatal on_vram_exhausted. True when it ran.
	bool ps4_deep_reclaim(const char* why);

	// 0.38 (vk::upload_image): true for the primary command buffer being recorded on the RSX thread, whose
	// submission the garbage collector tracks (a buffer disposed after recording into it is freed once it completed),
	// while upload-staging-off.flag is absent.
	bool ps4_is_current_primary_cb(const vk::command_buffer& cmd) const;

	// vk::ps4_gpu_liveness_in_wait: the liveness scan (rate-limited) from a long GPU wait of the RSX thread.
	void ps4_liveness_scan_in_wait() { ps4_check_gpu_liveness(); }
#endif

	// Handle pool creation failure due to fragmentation
	void on_descriptor_pool_fragmentation(bool is_fatal);

	// Conditional rendering
	void begin_conditional_rendering(const std::vector<rsx::reports::occlusion_query_info*>& sources) override;
	void end_conditional_rendering() override;

	// Host sync object
	std::pair<volatile vk::host_data_t*, VkBuffer> map_host_object_data() const;
	void on_guest_texture_read(const vk::command_buffer& cmd);

	// GRAPH backend
	void patch_transform_constants(rsx::context* ctx, u32 index, u32 count) override;

	// Misc
	bool is_current_program_interpreted() const override;

protected:
	void clear_surface(u32 mask) override;
	void begin() override;
	void end() override;
	void emit_geometry(u32 sub_index) override;

	void on_init_thread() override;
	void on_exit() override;
	void flip(const rsx::display_flip_info_t& info) override;

	void renderctl(u32 request_code, void* args) override;

	void do_local_task(rsx::FIFO::state state) override;
	bool scaled_image_from_memory(const rsx::blit_src_info& src, const rsx::blit_dst_info& dst, bool interpolate) override;
	void notify_tile_unbound(u32 tile) override;

	bool on_access_violation(u32 address, bool is_writing) override;
	void on_invalidate_memory_range(const utils::address_range32 &range, rsx::invalidation_cause cause) override;
	void on_semaphore_acquire_wait() override;
};
