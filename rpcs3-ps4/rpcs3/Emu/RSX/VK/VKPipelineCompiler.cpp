#include "stdafx.h"
#include "VKPipelineCompiler.h"
#include "VKRenderPass.h"
#include "vkutils/device.h"
#include "Utilities/Thread.h"
#include "Emu/system_config.h"

#include "util/sysinfo.hpp"

#ifdef RPCS3_PS4
#include "Emu/RSX/RSXThread.h"
#include "ps4_launcher.h"
#include "Utilities/File.h"
#include "util/ps4_memory_governor.hpp"
#include "util/ps4_rsx_programs.hpp"

#include <atomic>
#include <optional>
#endif

namespace vk
{
	// Global list of worker threads
	std::unique_ptr<named_thread_group<pipe_compiler>> g_pipe_compilers;
	int g_num_pipe_compilers = 0;
	atomic_t<int> g_compiler_index{};
	VkPipelineCache g_pipeline_cache = VK_NULL_HANDLE;

#ifdef RPCS3_PS4
	// 0.34: asynchronous pipeline backlog (rsx::g_ps4_rsx_counters) for the 10 s 'RSX gameplay' line and the burst
	// checkpoints. Counted before the push, so a worker can never retire a job that was not counted yet.
	static void ps4_pipe_queued() noexcept
	{
		auto& pc = rsx::g_ps4_rsx_counters;
		const u32 pending = pc.pipe_pending.add_fetch(1);
		pc.pipe_submitted++;

		for (auto* peak : { &pc.pipe_pending_max, &pc.pipe_burst_max })
		{
			peak->fetch_op([pending](u32& value)
			{
				if (value >= pending)
				{
					return false;
				}

				value = pending;
				return true;
			});
		}
	}

	static void ps4_pipe_retired() noexcept
	{
		// Floor at 0: the gauge is reset when the workers (and the jobs still queued) are destroyed.
		rsx::g_ps4_rsx_counters.pipe_pending.fetch_op([](u32& value)
		{
			if (!value)
			{
				return false;
			}

			value--;
			return true;
		});
	}

	// 0.34: /data/rpcs3/vk-fast-pipelines.flag (read per renderer): a graphics job that starts while 4 or more jobs
	// are pending is linked with VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT, so a backlog drains sooner; those
	// shaders run unoptimized until the next boot, whose shader cache preload compiles inline (never flagged).
	static atomic_t<bool> s_ps4_fast_pipelines{false};
	static constexpr u32 ps4_fast_link_backlog = 4;
	static thread_local bool t_ps4_fast_link = false;

	u32 ps4_pipelines_pending() noexcept
	{
		return rsx::g_ps4_rsx_counters.pipe_pending.load();
	}

	// 0.38: compile admission hook (VKPipelineCompiler.h). Without one, each job takes a compile slot of the memory
	// governor (utils::ps4::governor::compile_slot), like the PPU/SPU LLVM workers: under heap or thread pressure the
	// compiles run one at a time. rsx-compile-no-slot.flag (read per worker group): jobs never wait (0.37).
	// 0.39: only from memory level 2 (a real shortage) up; below it the workers run in parallel as in 0.37, instead of
	// sharing one slot with every PPU/SPU compile from level 1 (pipeline bursts at ~0.5 s per pipeline).
	static std::atomic<const ps4_pipe_compile_hook*> s_ps4_compile_hook{ nullptr };
	static atomic_t<bool> s_ps4_compile_slot{true};

	// 0.39: shader interpreter links run before queued game pipelines (interp-no-lane.flag: in the FIFO, as 0.38).
	static atomic_t<bool> s_ps4_lane_enabled{true};
	lf_queue<pipe_compiler::pipe_compiler_job> pipe_compiler::s_ps4_lane;
	lf_queue<std::function<void()>> pipe_compiler::s_ps4_translate;

	// Governor slot class of a job: a game pipeline or a shader interpreter link.
	enum class ps4_job_kind : u8
	{
		rsx,
		interp,
	};

	// The governor's slot classes (taken from level 2 up, where it admits one compile in the whole process): neither
	// class waits for the level to fall first, as PPU/SPU compiles do (the RSX draws with what is ready meanwhile).
	static void ps4_take_compile_slot(std::optional<utils::ps4::governor::compile_slot>& slot, ps4_job_kind kind, bool (*stopped)() noexcept) noexcept
	{
		using utils::ps4::governor::slot_kind;
		slot.emplace(stopped, kind == ps4_job_kind::interp ? slot_kind::interp : slot_kind::rsx);
	}

	void ps4_set_pipe_compile_hook(const ps4_pipe_compile_hook* hook) noexcept
	{
		s_ps4_compile_hook.store(hook, std::memory_order_release);
	}

	static bool ps4_pipe_worker_stopped() noexcept
	{
		return thread_ctrl::state() == thread_state::aborting;
	}

	// One queued job: enter() before it, leave() after its callback (the same hook object for both), or the governor's
	// slot for the same span. A worker waiting for a slot returns once its group is being destroyed.
	class ps4_compile_admission
	{
		const ps4_pipe_compile_hook* m_hook = s_ps4_compile_hook.load(std::memory_order_acquire);
		std::optional<utils::ps4::governor::compile_slot> m_slot;

	public:
		explicit ps4_compile_admission(ps4_job_kind kind) noexcept
		{
			if (m_hook && m_hook->enter)
			{
				m_hook->enter(&ps4_pipe_worker_stopped);
				return;
			}

			if (utils::ps4::rsx_programs::rsx_job_takes_slot(s_ps4_compile_slot.observe(), utils::ps4::governor::memory_level()))
			{
				ps4_take_compile_slot(m_slot, kind, &ps4_pipe_worker_stopped);
			}
		}

		~ps4_compile_admission()
		{
			if (m_hook && m_hook->enter && m_hook->leave)
			{
				m_hook->leave();
			}
		}

		ps4_compile_admission(const ps4_compile_admission&) = delete;
		ps4_compile_admission& operator=(const ps4_compile_admission&) = delete;
	};

	// Draw pipelines (game programs, shader interpreter): a link the driver refuses after the reclaim and retry in
	// program::link returns no program instead of ending the process.
	class ps4_nullable_link_scope
	{
		const bool m_previous = ps4_set_link_nullable(true);

	public:
		ps4_nullable_link_scope() noexcept = default;

		~ps4_nullable_link_scope()
		{
			ps4_set_link_nullable(m_previous);
		}

		ps4_nullable_link_scope(const ps4_nullable_link_scope&) = delete;
		ps4_nullable_link_scope& operator=(const ps4_nullable_link_scope&) = delete;
	};
#endif

	pipe_compiler::pipe_compiler()
	{
		// TODO: Initialize workqueue
	}

	pipe_compiler::~pipe_compiler()
	{
		// TODO: Destroy and do cleanup
		m_pipeline_cache = VK_NULL_HANDLE;
	}

	void pipe_compiler::initialize(const vk::render_device* pdev, VkPipelineCache pipeline_cache)
	{
		m_device = pdev;
		m_pipeline_cache = pipeline_cache;
	}

#ifdef RPCS3_PS4
	bool pipe_compiler::ps4_run_job(pipe_compiler_job& job)
	{
		// Under a memory shortage the governor runs one compile at a time (PPU, SPU and these jobs).
		const ps4_compile_admission admission((job.flags & PS4_INTERPRETER_LINK) ? ps4_job_kind::interp : ps4_job_kind::rsx);

		if (ps4_pipe_worker_stopped())
		{
			// 0.39: the workers are being destroyed (a slot waiter returns then): not compiled, dropped with the queue
			// (its owner is destroyed right after the workers).
			ps4_pipe_retired();
			return false;
		}

		if (!job.is_graphics_job)
		{
			auto compiled = int_compile_compute_pipe(job.compute_data, job.inputs, job.flags);
			job.callback_func(compiled);
			ps4_pipe_retired();
			return true;
		}

		t_ps4_fast_link = s_ps4_fast_pipelines.observe() && ps4_pipelines_pending() >= ps4_fast_link_backlog;
		auto compiled = job.create_info_func
			? int_compile_graphics_pipe(job.create_info_func, job.inputs, {}, job.flags)
			: int_compile_graphics_pipe(job.graphics_data, job.graphics_modules, job.inputs, {}, job.flags);
		t_ps4_fast_link = false;
		job.callback_func(compiled);
		ps4_pipe_retired();
		return true;
	}

	void pipe_compiler::ps4_drain_lane()
	{
		// Left in the lane while the workers are being destroyed: a new worker group runs them, or
		// destroy_pipe_compiler() drops them.
		while (s_ps4_lane && !ps4_pipe_worker_stopped())
		{
			for (auto&& job : s_ps4_lane.pop_all())
			{
				if (ps4_run_job(job))
				{
					utils::ps4::rsx_programs::g_pipeline_stats.lane_links.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
	}

	void pipe_compiler::ps4_drop_lane() noexcept
	{
		u32 dropped = 0;
		for (auto&& job : s_ps4_lane.pop_all())
		{
			static_cast<void>(job);
			dropped++;
		}

		if (dropped)
		{
			rsx_log.notice("PS4 RSX: %u queued shader interpreter link(s) dropped with the pipeline workers", dropped);
		}
	}

	void pipe_compiler::ps4_post_translation(std::function<void()> job)
	{
		s_ps4_translate.push(std::move(job));
	}

	void pipe_compiler::ps4_drain_translations()
	{
		while (s_ps4_translate && !ps4_pipe_worker_stopped())
		{
			for (auto&& job : s_ps4_translate.pop_all())
			{
				// Under a memory shortage the governor runs one compile at a time (as for a game pipeline).
				const ps4_compile_admission admission(ps4_job_kind::rsx);

				if (ps4_pipe_worker_stopped())
				{
					// Kept for the next worker group (a resize), or dropped with the queue at the renderer's exit.
					s_ps4_translate.push<false>(std::move(job));
					continue;
				}

				job();
			}
		}
	}

	void pipe_compiler::ps4_drop_translations() noexcept
	{
		u32 dropped = 0;
		for (auto&& job : s_ps4_translate.pop_all())
		{
			static_cast<void>(job);
			dropped++;
		}

		if (dropped)
		{
			rsx_log.notice("PS4 RSX: %u queued shader program translation(s) dropped with the pipeline workers", dropped);
		}
	}
#endif

	void pipe_compiler::operator()()
	{
		while (thread_ctrl::state() != thread_state::aborting)
		{
#ifdef RPCS3_PS4
			// 0.39: shader interpreter links first, also between two game pipelines of one batch. Once the workers are
			// being destroyed, the jobs not started are dropped instead of compiled (the stop no longer waits for every
			// queued link, 'RSX.W is too sleepy').
			// 0.39: program translations (shader-translate-async.flag) before the interpreter links.
			ps4_drain_translations();
			ps4_drain_lane();

			u32 dropped = 0;
			for (auto&& job : m_work_queue.pop_all())
			{
				if (ps4_pipe_worker_stopped())
				{
					ps4_pipe_retired();
					dropped++;
					continue;
				}

				ps4_drain_translations();
				ps4_drain_lane();

				if (!ps4_run_job(job))
				{
					dropped++;
				}
			}

			if (dropped)
			{
				rsx_log.notice("PS4 RSX: pipeline worker stopping: %u queued job(s) dropped", dropped);
			}

			thread_ctrl::wait_on_custom<3>([&](atomic_wait::list<5>& list)
			{
				list.template set<0>(m_work_queue);
				list.template set<1>(s_ps4_lane);
				list.template set<2>(s_ps4_translate);
			});
#else
			for (auto&& job : m_work_queue.pop_all())
			{
				if (!job.is_graphics_job)
				{
					auto compiled = int_compile_compute_pipe(job.compute_data, job.inputs, job.flags);
					job.callback_func(compiled);
					continue;
				}

				if (job.create_info_func)
				{
					auto compiled = int_compile_graphics_pipe(job.create_info_func, job.inputs, {}, job.flags);
					job.callback_func(compiled);
					continue;
				}

				auto compiled = int_compile_graphics_pipe(job.graphics_data, job.graphics_modules, job.inputs, {}, job.flags);
				job.callback_func(compiled);
			}

			thread_ctrl::wait_on(m_work_queue);
#endif
		}
	}

	std::unique_ptr<glsl::program> pipe_compiler::int_compile_compute_pipe(
		const VkComputePipelineCreateInfo& create_info,
		const std::vector<glsl::program_input>& cs_inputs,
		op_flags flags)
	{
		auto program = std::make_unique<glsl::program>(*m_device, create_info, cs_inputs);
		program->link(m_pipeline_cache, flags & SEPARATE_SHADER_OBJECTS);
		return program;
	}

	std::unique_ptr<glsl::program> pipe_compiler::int_compile_graphics_pipe(
		const VkGraphicsPipelineCreateInfo& create_info,
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs,
		op_flags flags)
	{
#ifdef RPCS3_PS4
		if (t_ps4_fast_link)
		{
			// Set only on a worker thread around an asynchronous graphics job (operator() above). The program keeps
			// its own copy of the create info, so the flag lives as long as this pipeline object.
			VkGraphicsPipelineCreateInfo fast_info = create_info;
			fast_info.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
			auto program = std::make_unique<glsl::program>(*m_device, fast_info, vs_inputs, fs_inputs);
			program->link(m_pipeline_cache, flags & SEPARATE_SHADER_OBJECTS);
			if (!program->value())
			{
				// 0.38: refused by the driver (nullable draw pipeline only, see program::link).
				return {};
			}

			rsx::g_ps4_rsx_counters.pipe_fast_linked++;
			return program;
		}
#endif
		auto program = std::make_unique<glsl::program>(*m_device, create_info, vs_inputs, fs_inputs);
		program->link(m_pipeline_cache, flags & SEPARATE_SHADER_OBJECTS);
#ifdef RPCS3_PS4
		if (!program->value())
		{
			// 0.38: refused by the driver (nullable draw pipeline only, see program::link): no program, not ready.
			return {};
		}
#endif
		return program;
	}

	std::unique_ptr<glsl::program> pipe_compiler::int_compile_graphics_pipe(
		const vk::pipeline_props &create_info,
		VkShaderModule modules[2],
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs,
		op_flags flags)
	{
#ifdef RPCS3_PS4
		// 0.38: a game draw pipeline; the program cache treats a missing one as not ready (interpreter or skipped draw).
		const ps4_nullable_link_scope nullable;
#endif
		VkPipelineShaderStageCreateInfo shader_stages[2] = {};
		shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		shader_stages[0].module = modules[0];
		shader_stages[0].pName = "main";

		shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		shader_stages[1].module = modules[1];
		shader_stages[1].pName = "main";

		std::vector<VkDynamicState> dynamic_state_descriptors;
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_VIEWPORT);
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_SCISSOR);
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_LINE_WIDTH);
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK);
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);

		auto pdss = &create_info.state.ds;
		VkPipelineDepthStencilStateCreateInfo ds2;
		if (g_render_device->get_depth_bounds_support()) [[likely]]
		{
			dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS);
		}
		else if (pdss->depthBoundsTestEnable)
		{
			rsx_log.warning("Depth bounds test is enabled in the pipeline object but not supported by the current driver.");

			ds2 = *pdss;
			pdss = &ds2;
			ds2.depthBoundsTestEnable = VK_FALSE;
		}

		VkPipelineDynamicStateCreateInfo dynamic_state_info = {};
		dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamic_state_info.pDynamicStates = dynamic_state_descriptors.data();
		dynamic_state_info.dynamicStateCount = ::size32(dynamic_state_descriptors);

		VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

		VkPipelineViewportStateCreateInfo vp = {};
		vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		vp.viewportCount = 1;
		vp.scissorCount = 1;

		auto pmss = &create_info.state.ms;
		VkPipelineMultisampleStateCreateInfo ms2;
		ensure(pmss->rasterizationSamples == VkSampleCountFlagBits((create_info.renderpass_key >> 16) & 0xF)); // "Multisample state mismatch!"

		if (pmss->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT || pmss->sampleShadingEnable) [[unlikely]]
		{
			ms2 = *pmss;
			pmss = &ms2;

			if (ms2.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
			{
				// Update the sample mask pointer
				ms2.pSampleMask = &create_info.state.temp_storage.msaa_sample_mask;
			}

			if (g_cfg.video.antialiasing_level == msaa_level::none && ms2.sampleShadingEnable)
			{
				// Do not compile with MSAA enabled if multisampling is disabled
				rsx_log.warning("MSAA is disabled globally but a shader with multi-sampling enabled was submitted for compilation.");
				ms2.sampleShadingEnable = VK_FALSE;
			}
		}

		// Rebase pointers from pipeline structure in case it is moved/copied
		VkPipelineColorBlendStateCreateInfo cs = create_info.state.cs;
		cs.pAttachments = create_info.state.att_state;

		VkPipelineRasterizationStateCreateInfo rs = create_info.state.rs;
		VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_vertex_state{};
		if (flags & USE_LAST_PROVOKING_VERTEX)
		{
			ensure(m_device->get_provoking_vertex_last_support());
			provoking_vertex_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT;
			provoking_vertex_state.pNext = rs.pNext;
			provoking_vertex_state.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
			rs.pNext = &provoking_vertex_state;
		}

		VkGraphicsPipelineCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		info.pVertexInputState = &vi;
		info.pInputAssemblyState = &create_info.state.ia;
		info.pRasterizationState = &rs;
		info.pColorBlendState = &cs;
		info.pMultisampleState = pmss;
		info.pViewportState = &vp;
		info.pDepthStencilState = pdss;
		info.stageCount = 2;
		info.pStages = shader_stages;
		info.pDynamicState = &dynamic_state_info;
		info.layout = VK_NULL_HANDLE;
		info.basePipelineIndex = -1;
		info.basePipelineHandle = VK_NULL_HANDLE;
		info.renderPass = vk::get_renderpass(*m_device, create_info.renderpass_key);

		return int_compile_graphics_pipe(info, vs_inputs, fs_inputs, flags);
	}

	std::unique_ptr<glsl::program> pipe_compiler::int_compile_graphics_pipe(
		graphics_pipe_create_callback_t pipe_info_create_fn,
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs,
		op_flags flags)
	{
#ifdef RPCS3_PS4
		// 0.38: shader interpreter pipelines; VKShaderInterpreter.cpp treats a missing one as not ready.
		const ps4_nullable_link_scope nullable;
#endif
		VkGraphicsPipelineCreateInfo create_info = pipe_info_create_fn();
		return int_compile_graphics_pipe(create_info, vs_inputs, fs_inputs, flags);
	}

	std::unique_ptr<glsl::program> pipe_compiler::compile(
		const VkComputePipelineCreateInfo& create_info,
		op_flags flags, callback_t callback,
		const std::vector<glsl::program_input>& cs_inputs)
	{
		if (flags & COMPILE_INLINE)
		{
			return int_compile_compute_pipe(create_info, cs_inputs, flags);
		}

#ifdef RPCS3_PS4
		ps4_pipe_queued();
#endif
		m_work_queue.push(create_info, cs_inputs, flags, callback);
		return {};
	}

	std::unique_ptr<glsl::program> pipe_compiler::compile(
		const VkGraphicsPipelineCreateInfo& create_info,
		op_flags flags, callback_t /*callback*/,
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs)
	{
		// It is very inefficient to defer this as all pointers need to be saved
		ensure(flags & COMPILE_INLINE, "Asynchronous compilation is not allowed for raw graphics pipeline input");
		return int_compile_graphics_pipe(create_info, vs_inputs, fs_inputs, flags);
	}

	std::unique_ptr<glsl::program> pipe_compiler::compile(
		const vk::pipeline_props &create_info,
		VkShaderModule vs,
		VkShaderModule fs,
		op_flags flags, callback_t callback,
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs)
	{
		VkShaderModule modules[] = { vs, fs };
		if (flags & COMPILE_INLINE)
		{
			return int_compile_graphics_pipe(create_info, modules, vs_inputs, fs_inputs, flags);
		}

#ifdef RPCS3_PS4
		ps4_pipe_queued();
#endif
		m_work_queue.push(create_info, modules, vs_inputs, fs_inputs, flags, callback);
		return {};
	}

	std::unique_ptr<glsl::program> pipe_compiler::compile(
		graphics_pipe_create_callback_t get_create_info,
		op_flags flags, callback_t callback,
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs)
	{
		if (flags & COMPILE_INLINE)
		{
			return int_compile_graphics_pipe(get_create_info, vs_inputs, fs_inputs, flags);
		}

#ifdef RPCS3_PS4
		ps4_pipe_queued();

		if ((flags & PS4_INTERPRETER_LINK) && s_ps4_lane_enabled.observe())
		{
			// 0.39: the next free worker runs it before its next game pipeline (0.38: behind the whole FIFO, median 5 s).
			s_ps4_lane.push(get_create_info, vs_inputs, fs_inputs, flags, callback);
			return {};
		}
#endif
		m_work_queue.push(get_create_info, vs_inputs, fs_inputs, flags, callback);
		return {};
	}

	int decay_num_worker_threads(int num_worker_threads)
	{
		if (num_worker_threads <= 0)
		{
			// Select a conservative but modern default for async pipeline compilation.
			// Older heuristics topped out too early on high-core CPUs and left large
			// shader bursts queued longer than necessary.
			const auto hw_threads = utils::get_thread_count();

			if (hw_threads >= 24)
			{
				num_worker_threads = 12;
			}
			else if (hw_threads >= 16)
			{
				num_worker_threads = 8;
			}
			else if (hw_threads > 12)
			{
				num_worker_threads = 6;
			}
			else if (hw_threads > 8)
			{
				num_worker_threads = 4;
			}
			else if (hw_threads == 8)
			{
				num_worker_threads = 2;
			}
			else
			{
				num_worker_threads = 1;
			}

			rsx_log.notice("Async pipeline compiler auto-selected %d worker(s) for %u host thread(s).",
				num_worker_threads, hw_threads);
		}

		return num_worker_threads;
	}

	void initialize_pipe_compiler(int num_worker_threads, VkPipelineCache pipe_cache)
	{
		num_worker_threads = decay_num_worker_threads(num_worker_threads);

		ensure(num_worker_threads >= 1);
		ensure(g_render_device); // "Cannot initialize pipe compiler before creating a logical device"

		// Create the shared pipeline cache
#ifdef RPCS3_PS4
		// 0.38: RADV (Mesa 26.3) holds a strong reference to every shader created through an application VkPipelineCache
		// until the cache is destroyed at the game's stop, also after RPCS3 destroyed the pipeline, so no pipeline or
		// interpreter eviction could return memory. Without one RADV uses its device cache, whose references are weak
		// (identical shaders are still shared while alive). Nothing is lost: this cache was never saved to disk.
		const bool ps4_app_cache = fs::is_file(utils::ps4::rsx_programs::app_pipeline_cache_flag);
		ps4_startup_checkpoint(ps4_app_cache
			? "Vulkan: pipelines are created through an application VkPipelineCache (vk-app-pipeline-cache.flag, as 0.37)"
			: "Vulkan: pipelines are created without an application VkPipelineCache (RADV keeps no shader past its pipelines)");

		if (!pipe_cache && ps4_app_cache)
#else
		if (!pipe_cache)
#endif
		{
			VkPipelineCacheCreateInfo drv_cache_info{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
			vkCreatePipelineCache(*g_render_device, &drv_cache_info, nullptr, &g_pipeline_cache);
		}
		else
		{
			g_pipeline_cache = pipe_cache;
		}

		// Create the thread pool
		g_pipe_compilers = std::make_unique<named_thread_group<pipe_compiler>>("RSX.W", num_worker_threads);
		g_num_pipe_compilers = num_worker_threads;

		// Initialize the workers. At least one inline compiler shall exist (doesn't actually run)
		for (pipe_compiler& compiler : *g_pipe_compilers.get())
		{
			compiler.initialize(g_render_device, g_pipeline_cache);
		}

#ifdef RPCS3_PS4
		// New workers, empty queues: jobs queued to a previous worker group were dropped with it.
		rsx::g_ps4_rsx_counters.pipe_pending.release(0);

		const bool compile_slot = !fs::is_file(utils::ps4::rsx_programs::compile_no_slot_flag);
		s_ps4_compile_slot.release(compile_slot);
		if (!compile_slot)
		{
			ps4_startup_checkpoint("RSX: pipeline compile jobs run without the memory governor's compile slots (rsx-compile-no-slot.flag)");
		}

		const bool lane = !fs::is_file(utils::ps4::rsx_programs::interp_no_lane_flag);
		s_ps4_lane_enabled.release(lane);
		ps4_startup_checkpoint(fmt::format("RSX: %u pipeline worker(s); %s%s", num_worker_threads,
			compile_slot ? fmt::format("compile jobs take a memory governor compile slot from memory level %d up; ", utils::ps4::rsx_programs::compile_slot_level) : std::string(),
			lane ? "shader interpreter links run before queued game pipelines (interp-no-lane.flag: in the queue)"
				: "shader interpreter links wait in the pipeline queue (interp-no-lane.flag)").c_str());

		const bool fast_pipelines = fs::is_file("/data/rpcs3/vk-fast-pipelines.flag");
		s_ps4_fast_pipelines.release(fast_pipelines);
		if (fast_pipelines)
		{
			ps4_startup_checkpoint(fmt::format("RSX: pipelines compiled while %u or more are pending are linked without driver optimizations "
				"(vk-fast-pipelines.flag); the next boot rebuilds them optimized from the shader cache", ps4_fast_link_backlog).c_str());
		}
#endif
	}

	void resize_pipe_compiler(int num_worker_threads)
	{
		num_worker_threads = decay_num_worker_threads(num_worker_threads);
		if (static_cast<u32>(num_worker_threads) <= g_pipe_compilers->size())
		{
			// Just lie about how many compilers we have
			g_num_pipe_compilers = num_worker_threads;
			return;
		}

		// We need a bigger thread pool. Very unlikely but provided for correctness.
		g_pipe_compilers.reset();
		g_num_pipe_compilers = 0;

		initialize_pipe_compiler(num_worker_threads, g_pipeline_cache);
	}

	void destroy_pipe_compiler()
	{
		g_pipe_compilers.reset();
#ifdef RPCS3_PS4
		// The workers are gone: queued interpreter links and program translations reference the renderer being
		// destroyed.
		pipe_compiler::ps4_drop_lane();
		pipe_compiler::ps4_drop_translations();
		rsx::g_ps4_rsx_counters.pipe_pending.release(0);
#endif

		if (g_pipeline_cache)
		{
			vkDestroyPipelineCache(*g_render_device, g_pipeline_cache, nullptr);
			g_pipeline_cache = VK_NULL_HANDLE;
		}
	}

	pipe_compiler* get_pipe_compiler()
	{
		ensure(g_pipe_compilers);
		int thread_index = g_compiler_index++;

		return g_pipe_compilers.get()->begin() + (thread_index % g_num_pipe_compilers);
	}
}
