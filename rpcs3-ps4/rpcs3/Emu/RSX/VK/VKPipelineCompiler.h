#pragma once
#include "Utilities/lockless.h"
#include "VKProgramPipeline.h"
#include "vkutils/graphics_pipeline_state.hpp"
#include "util/fnv_hash.hpp"

namespace vk
{
	class render_device;

#ifdef RPCS3_PS4
	// 0.38: admission around every queued pipeline job on the RSX.W workers, so shader compiles share the memory
	// governor's compile slots with PPU/SPU compiles. enter(stopped) may wait and must return once stopped() is true
	// (the worker is being destroyed); leave() follows every enter() on the same thread, after the job's callback.
	// Never called on the RSX thread or for inline compiles. The hook object must outlive its installation;
	// nullptr uninstalls it. Unset: each job holds a utils::ps4::governor::compile_slot (rsx-compile-no-slot.flag: jobs
	// run unconditionally, as 0.37).
	struct ps4_pipe_compile_hook
	{
		void (*enter)(bool (*stopped)() noexcept) noexcept = nullptr;
		void (*leave)() noexcept = nullptr;
	};
	void ps4_set_pipe_compile_hook(const ps4_pipe_compile_hook* hook) noexcept;

	// 0.38 (VKProgramPipeline.cpp): while set on the calling thread, a program link the driver refuses after a memory
	// reclaim and one retry leaves the program unlinked (value() == VK_NULL_HANDLE) instead of ending the process. The
	// pipe compiler sets it for draw pipelines (game programs, shader interpreter), whose callers treat a missing
	// program as not ready; overlay and compute programs keep the fatal path. Returns the previous value.
	bool ps4_set_link_nullable(bool nullable) noexcept;
	bool ps4_link_nullable() noexcept;

	// 0.38 (vkutils/descriptors.cpp): before retrying a driver call that failed (what = the call): calls the CPU heap's
	// std::new_handler unless the failure was device memory, and trims video memory unless it was host memory. With
	// defer_trim (or off the RSX thread) the trim runs at the next frame end and only completed work is retired now,
	// so nothing the current draw has bound is evicted. Allocation-free, rate-limited startup.log note.
	void ps4_reclaim_for_driver(VkResult result, const char* what, bool defer_trim) noexcept;

	// 0.38 (vkutils/descriptors.cpp): while set on the calling thread (the draw path around program::bind), a descriptor
	// set the driver still refuses after the reclaim ladder is returned as VK_NULL_HANDLE instead of ending the process
	// (the ladder runs at most once per second, refusals for 10 s in a row request the graceful stop). Returns the
	// previous value. Sets refused that way since boot.
	bool ps4_set_descriptor_nullable(bool nullable) noexcept;
	u64 ps4_descriptor_sets_refused() noexcept;

	namespace glsl
	{
		// 0.38 (VKProgramPipeline.cpp): true once after a program::bind on this thread that recorded nothing because a
		// descriptor set was refused (the draw must be skipped).
		bool ps4_take_bind_refused() noexcept;

		// 0.38 (VKProgramPipeline.cpp, any thread, allocation-free): the next descriptor pool sweep (RSX thread, every
		// ~120 frames) releases the pools of every program table not committed since the previous sweep. For memory
		// shedders; idle tables, the 1024-pool count cap and the 48 MiB window cap are handled by every sweep.
		void ps4_request_descriptor_pool_release() noexcept;

		// 0.38 (VKProgramPipeline.cpp, RSX thread): hands a pipeline to the garbage collector, which destroys it once
		// every command buffer recorded so far has completed. Found by argument-dependent lookup from
		// program_state_cache (ProgramStateCache.h), whose idle-pipeline eviction is enabled only where it exists.
		void ps4_dispose_pipeline(std::unique_ptr<program>&& pipeline);
	}
#endif

	struct pipeline_props
	{
		graphics_pipeline_state state;
		u64 renderpass_key;

		bool operator==(const pipeline_props& other) const
		{
			if (renderpass_key != other.renderpass_key)
				return false;

			if (memcmp(&state.ia, &other.state.ia, sizeof(VkPipelineInputAssemblyStateCreateInfo)))
				return false;

			if (memcmp(&state.rs, &other.state.rs, sizeof(VkPipelineRasterizationStateCreateInfo)))
				return false;

			// Cannot memcmp cs due to pAttachments being a pointer to memory
			if (state.cs.attachmentCount != other.state.cs.attachmentCount ||
				state.cs.logicOp != other.state.cs.logicOp ||
				state.cs.logicOpEnable != other.state.cs.logicOpEnable ||
				memcmp(state.cs.blendConstants, other.state.cs.blendConstants, 4 * sizeof(f32)))
				return false;

			if (memcmp(state.att_state, &other.state.att_state, state.cs.attachmentCount * sizeof(VkPipelineColorBlendAttachmentState)))
				return false;

			if (memcmp(&state.ds, &other.state.ds, sizeof(VkPipelineDepthStencilStateCreateInfo)))
				return false;

			if (state.ms.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
			{
				if (memcmp(&state.ms, &other.state.ms, sizeof(VkPipelineMultisampleStateCreateInfo)))
					return false;

				if (state.temp_storage.msaa_sample_mask != other.state.temp_storage.msaa_sample_mask)
					return false;
			}

			return true;
		}
	};

	class pipe_compiler
	{
	public:
		enum op_flag_bits
		{
			COMPILE_DEFAULT = 0,
			COMPILE_INLINE = 1,
			COMPILE_DEFERRED = 2,
			SEPARATE_SHADER_OBJECTS = 4,
			USE_LAST_PROVOKING_VERTEX = 8,
#ifdef RPCS3_PS4
			// 0.39: a shader interpreter link (priority lane, interpreter slot class).
			PS4_INTERPRETER_LINK = 16,
#endif
		};

		using op_flags = rsx::flags32_t;

		using callback_t = std::function<void(std::unique_ptr<glsl::program>&)>;
		using graphics_pipe_create_callback_t = std::function<VkGraphicsPipelineCreateInfo()>;

		pipe_compiler();
		~pipe_compiler();

		void initialize(const vk::render_device* pdev, VkPipelineCache pipe_cache);

		std::unique_ptr<glsl::program> compile(
			const VkComputePipelineCreateInfo& cs,
			op_flags flags, callback_t callback = {},
			const std::vector<glsl::program_input>& cs_inputs = {});

		std::unique_ptr<glsl::program> compile(
			const VkGraphicsPipelineCreateInfo& create_info,
			op_flags flags, callback_t callback = {},
			const std::vector<glsl::program_input>& vs_inputs = {},
			const std::vector<glsl::program_input>& fs_inputs = {});

		std::unique_ptr<glsl::program> compile(
			const vk::pipeline_props &create_info,
			VkShaderModule vs,
			VkShaderModule fs,
			op_flags flags, callback_t callback = {},
			const std::vector<glsl::program_input>& vs_inputs = {},
			const std::vector<glsl::program_input>& fs_inputs = {});

		std::unique_ptr<glsl::program> compile(
			graphics_pipe_create_callback_t get_create_info,
			op_flags flags, callback_t callback,
			const std::vector<glsl::program_input>& vs_inputs,
			const std::vector<glsl::program_input>& fs_inputs);

		void operator()();

	private:
		class compute_pipeline_props : public VkComputePipelineCreateInfo
		{
			// Storage for the entry name
			std::string entry_name;

		public:
			compute_pipeline_props() = default;
			compute_pipeline_props(const VkComputePipelineCreateInfo& info)
			{
				(*static_cast<VkComputePipelineCreateInfo*>(this)) = info;
				entry_name = info.stage.pName;
				stage.pName = entry_name.c_str();
			}
		};

		struct pipe_compiler_job
		{
			bool is_graphics_job;
			callback_t callback_func;
			graphics_pipe_create_callback_t create_info_func;

			vk::pipeline_props graphics_data;
			compute_pipeline_props compute_data;
			VkShaderModule graphics_modules[2];
			std::vector<glsl::program_input> inputs;

			op_flags flags;

			pipe_compiler_job(
				const vk::pipeline_props& props,
				VkShaderModule modules[2],
				const std::vector<glsl::program_input>& vs_in,
				const std::vector<glsl::program_input>& fs_in,
				op_flags flags_,
				callback_t func)
			{
				callback_func = func;
				graphics_data = props;
				graphics_modules[0] = modules[0];
				graphics_modules[1] = modules[1];
				is_graphics_job = true;
				flags = flags_;

				inputs.reserve(vs_in.size() + fs_in.size());
				inputs.insert(inputs.end(), vs_in.begin(), vs_in.end());
				inputs.insert(inputs.end(), fs_in.begin(), fs_in.end());
			}

			pipe_compiler_job(
				graphics_pipe_create_callback_t pipe_info_create_fn,
				const std::vector<glsl::program_input>& vs_in,
				const std::vector<glsl::program_input>& fs_in,
				op_flags flags_,
				callback_t func)
			{
				callback_func = func;
				create_info_func = pipe_info_create_fn;
				is_graphics_job = true;
				flags = flags_;

				graphics_modules[0] = VK_NULL_HANDLE;
				graphics_modules[1] = VK_NULL_HANDLE;

				inputs.reserve(vs_in.size() + fs_in.size());
				inputs.insert(inputs.end(), vs_in.begin(), vs_in.end());
				inputs.insert(inputs.end(), fs_in.begin(), fs_in.end());
			}

			pipe_compiler_job(
				const VkComputePipelineCreateInfo& props,
				const std::vector<glsl::program_input>& cs_in,
				op_flags flags_,
				callback_t func)
			{
				callback_func = func;
				compute_data = props;
				is_graphics_job = false;
				flags = flags_;

				graphics_modules[0] = VK_NULL_HANDLE;
				graphics_modules[1] = VK_NULL_HANDLE;

				inputs = cs_in;
			}
		};

		const vk::render_device* m_device = nullptr;
		lf_queue<pipe_compiler_job> m_work_queue;
		VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;

#ifdef RPCS3_PS4
		// 0.39: shader interpreter links (PS4_INTERPRETER_LINK) of every worker, run before the next game pipeline
		// job of whichever worker is free first; dropped by destroy_pipe_compiler() once the workers are gone.
		static lf_queue<pipe_compiler_job> s_ps4_lane;

		// 0.39 (shader-translate-async.flag): translations of new vertex/fragment programs (ProgramStateCache.h), run
		// by whichever worker is free first, before the interpreter links; dropped by destroy_pipe_compiler().
		static lf_queue<std::function<void()>> s_ps4_translate;

		// Runs one job under the compile admission; false when it was dropped because the worker is being destroyed.
		bool ps4_run_job(pipe_compiler_job& job);
		// Runs the queued interpreter links (none while the worker is being destroyed).
		void ps4_drain_lane();
		// Runs the queued program translations (none while the worker is being destroyed: they stay queued).
		void ps4_drain_translations();

	public:
		static void ps4_drop_lane() noexcept;

		// Queues a program translation for the workers (renderer thread). The job decides itself whether it still has
		// to run (the renderer thread may have taken it back).
		static void ps4_post_translation(std::function<void()> job);
		static void ps4_drop_translations() noexcept;

	private:
#endif

		std::unique_ptr<glsl::program> int_compile_compute_pipe(
			const VkComputePipelineCreateInfo& create_info,
			const std::vector<glsl::program_input>& cs_inputs,
			op_flags flags);

		std::unique_ptr<glsl::program> int_compile_graphics_pipe(
			const VkGraphicsPipelineCreateInfo& create_info,
			const std::vector<glsl::program_input>& vs_inputs,
			const std::vector<glsl::program_input>& fs_inputs,
			op_flags flags);

		std::unique_ptr<glsl::program> int_compile_graphics_pipe(
			const vk::pipeline_props &create_info,
			VkShaderModule modules[2],
			const std::vector<glsl::program_input>& vs_inputs,
			const std::vector<glsl::program_input>& fs_inputs,
			op_flags flags);

		std::unique_ptr<glsl::program> int_compile_graphics_pipe(
			graphics_pipe_create_callback_t pipe_info_create_fn,
			const std::vector<glsl::program_input>& vs_inputs,
			const std::vector<glsl::program_input>& fs_inputs,
			op_flags flags);
	};

	void initialize_pipe_compiler(int num_worker_threads = 0, VkPipelineCache pipe_cache = VK_NULL_HANDLE);
	void resize_pipe_compiler(int num_worker_threads = 0);
	void destroy_pipe_compiler();
	pipe_compiler* get_pipe_compiler();
}

namespace rpcs3
{
	template <>
	inline usz hash_struct<vk::pipeline_props>(const vk::pipeline_props& pipelineProperties)
	{
		usz seed = hash_base(pipelineProperties.renderpass_key);
		seed ^= hash_struct(pipelineProperties.state.ia);
		seed ^= hash_struct(pipelineProperties.state.ds);
		seed ^= hash_struct(pipelineProperties.state.rs);
		seed ^= hash_struct(pipelineProperties.state.ms);
		seed ^= hash_base(pipelineProperties.state.temp_storage.msaa_sample_mask);

		// Do not compare pointers to memory!
		VkPipelineColorBlendStateCreateInfo tmp;
		memcpy(&tmp, &pipelineProperties.state.cs, sizeof(VkPipelineColorBlendStateCreateInfo));
		tmp.pAttachments = nullptr;

		for (usz i = 0; i < pipelineProperties.state.cs.attachmentCount; ++i)
		{
			seed ^= hash_struct(pipelineProperties.state.att_state[i]);
		}
		return hash_base(seed);
	}
}
