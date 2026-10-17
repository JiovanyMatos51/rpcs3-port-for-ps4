#include "stdafx.h"

#include "VKShaderInterpreter.h"
#include "VKVertexProgram.h"
#include "VKFragmentProgram.h"
#include "VKHelpers.h"
#include "VKRenderPass.h"

#include "Emu/RSX/Overlays/Shaders/shader_loading_dialog.h"
#include "Emu/RSX/Program/GLSLCommon.h"
#include "Emu/RSX/Program/ShaderInterpreter.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/localized_string.h"

#include <thread>

#ifdef RPCS3_PS4
#include "ps4_launcher.h"
#include "vkutils/garbage_collector.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/system_config.h"
#include "Utilities/File.h"
#include "util/ps4_rsx_programs.hpp"

#include <algorithm>
#include <atomic>
#endif

namespace vk
{
	using enum program_common::interpreter::compiler_option;
	using enum program_common::interpreter::cached_pipeline_flags;

#ifdef RPCS3_PS4
	namespace
	{
		namespace rp = utils::ps4::rsx_programs;

		// Options the generic interpreter shaders always enable (ShaderInterpreter.cpp base masks).
		constexpr u64 ps4_base_vs_mask = COMPILER_OPT_ALL_VS_MASK & ~COMPILER_OPT_VS_EXCL_MASK;
		constexpr u64 ps4_base_fs_mask = COMPILER_OPT_BASE_FS_MASK & ~COMPILER_OPT_FS_EXCL_MASK;

		// A variant whose link the driver refused is tried again after this long.
		constexpr u64 ps4_failed_link_retry_us = 30'000'000;

		std::atomic<u32> s_ps4_preload_pipelines{ 0 };
		std::atomic<u64> s_ps4_first_use_links{ 0 };
		std::atomic<u64> s_ps4_first_use_link_us{ 0 };
		std::atomic<u32> s_ps4_slow_links_noted{ 0 };

		std::atomic<u32> s_ps4_cap_request{ 0 };
		std::atomic<u32> s_ps4_live{ 0 };
		std::atomic<u64> s_ps4_evicted{ 0 };
		std::atomic<u64> s_ps4_failed{ 0 };
		std::atomic<u64> s_ps4_deferred{ 0 };
		std::atomic<u32> s_ps4_eviction_passes{ 0 };

		// 0.37: a variant linked on first use. RPCS3.log gets the 1st-256th and then every power of two; startup.log the
		// first 16 and the first 16 of 250 ms or more. 0.38: an asynchronous link (us = queue to ready, the draws of the
		// variant were skipped meanwhile) or a refused one.
		void ps4_note_first_use_link(u64 compiler_opt, u64 us, bool async = false, bool refused = false)
		{
			const u64 number = s_ps4_first_use_links.fetch_add(1, std::memory_order_relaxed) + 1;
			s_ps4_first_use_link_us.fetch_add(us, std::memory_order_relaxed);

			if (refused)
			{
				s_ps4_failed.fetch_add(1, std::memory_order_relaxed);
			}

			const bool slow = us >= 250'000;
			const bool to_rpcs3_log = number <= 256 || !(number & (number - 1)) || slow || refused;
			const bool to_startup_log = number <= 16 || refused || (slow && s_ps4_slow_links_noted.fetch_add(1, std::memory_order_relaxed) < 16);

			if (!to_rpcs3_log && !to_startup_log)
			{
				return;
			}

			const std::string line = refused
				? fmt::format("Vulkan: interpreter variant 0x%llx was refused by the driver after a memory reclaim and one retry; "
					"its draws are skipped, it links again in %llu s", compiler_opt, ps4_failed_link_retry_us / 1'000'000)
				: fmt::format("Vulkan: interpreter variant 0x%llx linked in %llu ms (first use%s)", compiler_opt, us / 1000,
					async ? ", generic form, asynchronous" : "");

			if (to_rpcs3_log)
			{
				rsx_log.notice("PS4 %s", line);
			}

			if (to_startup_log)
			{
				ps4_startup_checkpoint(line.c_str());
			}
		}

		// The program is destroyed by the garbage collector once every command buffer recorded so far has completed.
		void ps4_dispose_program(std::shared_ptr<glsl::program>&& program)
		{
			if (!program)
			{
				return;
			}

			auto holder = std::make_unique<std::shared_ptr<glsl::program>>(std::move(program));
			vk::get_gc()->dispose(holder);
		}
	}

	u32 ps4_interpreter_preload_pipelines() noexcept
	{
		return s_ps4_preload_pipelines.load(std::memory_order_relaxed);
	}

	u64 ps4_interpreter_first_use_links(u64& total_us) noexcept
	{
		total_us = s_ps4_first_use_link_us.load(std::memory_order_relaxed);
		return s_ps4_first_use_links.load(std::memory_order_relaxed);
	}

	u32 ps4_interpreter_pipelines_live(u64& evicted, u64& failed, u64& deferred) noexcept
	{
		evicted = s_ps4_evicted.load(std::memory_order_relaxed);
		failed = s_ps4_failed.load(std::memory_order_relaxed);
		deferred = s_ps4_deferred.load(std::memory_order_relaxed);
		return s_ps4_live.load(std::memory_order_relaxed);
	}

	void ps4_set_interpreter_cap(u32 cap) noexcept
	{
		s_ps4_cap_request.store(cap, std::memory_order_relaxed);
	}
#endif

	class async_pipe_compiler_context
	{
		vk::pipeline_props m_properties{};
		VkDevice m_device = VK_NULL_HANDLE;
		VkShaderModule m_vs = VK_NULL_HANDLE;
		VkShaderModule m_fs = VK_NULL_HANDLE;

		VkPipelineShaderStageCreateInfo m_shader_stages[2];
		VkPipelineDynamicStateCreateInfo m_dynamic_state_info{};
		VkPipelineVertexInputStateCreateInfo m_vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		VkPipelineViewportStateCreateInfo m_vp{};
		VkPipelineMultisampleStateCreateInfo m_ms{};
		VkPipelineColorBlendStateCreateInfo m_cs{};
		VkPipelineTessellationStateCreateInfo m_ts{};

		std::vector<VkDynamicState> m_dynamic_state_descriptors
		{
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR,
			VK_DYNAMIC_STATE_LINE_WIDTH,
			VK_DYNAMIC_STATE_BLEND_CONSTANTS,
			VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
			VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
			VK_DYNAMIC_STATE_STENCIL_REFERENCE,
			VK_DYNAMIC_STATE_DEPTH_BIAS
		};

	public:

		async_pipe_compiler_context(
			const vk::pipeline_props& props,
			VkDevice device,
			VkShaderModule vs,
			VkShaderModule fs)
			: m_properties(props)
			, m_device(device)
			, m_vs(vs)
			, m_fs(fs)
		{
		}

		VkGraphicsPipelineCreateInfo compile()
		{
			m_shader_stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
			m_shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			m_shader_stages[0].module = m_vs;
			m_shader_stages[0].pName = "main";

			m_shader_stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO  };
			m_shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			m_shader_stages[1].module = m_fs;
			m_shader_stages[1].pName = "main";

			if (vk::get_current_renderer()->get_depth_bounds_support())
			{
				m_dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS);
			}

			m_dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
			m_dynamic_state_info.pDynamicStates = m_dynamic_state_descriptors.data();
			m_dynamic_state_info.dynamicStateCount = ::size32(m_dynamic_state_descriptors);

			m_vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
			m_vp.viewportCount = 1;
			m_vp.scissorCount = 1;

			m_ms = m_properties.state.ms;
			ensure(m_ms.rasterizationSamples == VkSampleCountFlagBits((m_properties.renderpass_key >> 16) & 0xF)); // "Multisample state mismatch!"
			if (m_ms.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
			{
				// Update the sample mask pointer
				m_ms.pSampleMask = &m_properties.state.temp_storage.msaa_sample_mask;
			}

			// Rebase pointers from pipeline structure in case it is moved/copied
			m_cs = m_properties.state.cs;
			m_cs.pAttachments = m_properties.state.att_state;

			m_ts = {};
			m_ts.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;

			VkGraphicsPipelineCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			info.pVertexInputState = &m_vi;
			info.pInputAssemblyState = &m_properties.state.ia;
			info.pRasterizationState = &m_properties.state.rs;
			info.pColorBlendState = &m_cs;
			info.pMultisampleState = &m_ms;
			info.pViewportState = &m_vp;
			info.pDepthStencilState = &m_properties.state.ds;
			info.pTessellationState = &m_ts;
			info.stageCount = 2;
			info.pStages = m_shader_stages;
			info.pDynamicState = &m_dynamic_state_info;
			info.layout = VK_NULL_HANDLE;
			info.basePipelineIndex = -1;
			info.basePipelineHandle = VK_NULL_HANDLE;
			info.renderPass = vk::get_renderpass(m_device, m_properties.renderpass_key);
			return info;
		}
	};

	u32 shader_interpreter::init(std::shared_ptr<VKVertexProgram>& vk_prog, u64 compiler_options) const
	{
		std::memset(&vk_prog->binding_table, 0xff, sizeof(vk_prog->binding_table));

		u32 location = 0;
		vk_prog->binding_table.vertex_buffers_location = location;
		location += 3;

		vk_prog->binding_table.context_buffer_location = location++;

		if (vk::emulate_conditional_rendering())
		{
			vk_prog->binding_table.cr_pred_buffer_location = location++;
		}

		if (compiler_options & COMPILER_OPT_ENABLE_INSTANCING)
		{
			vk_prog->binding_table.instanced_lut_buffer_location = location++;
			vk_prog->binding_table.instanced_cbuf_location = location++;
		}
		else
		{
			vk_prog->binding_table.cbuf_location = location++;
		}

		// Return next index
		return location;
	}

	u32 shader_interpreter::init(std::shared_ptr<VKFragmentProgram>& vk_prog, u64 /*compiler_opt*/) const
	{
		std::memset(&vk_prog->binding_table, 0xff, sizeof(vk_prog->binding_table));

		vk_prog->binding_table.context_buffer_location = 0;
		vk_prog->binding_table.tex_param_location = 1;
		vk_prog->binding_table.polygon_stipple_params_location = 2;

		// Return next index
		return 3;
	}

	std::shared_ptr<VKVertexProgram> shader_interpreter::build_vs(u64 compiler_options)
	{
		compiler_options &= COMPILER_OPT_ALL_VS_MASK;
		{
			reader_lock lock(m_vs_shader_cache_lock);
			if (auto found = m_vs_shader_cache.find(compiler_options);
				found != m_vs_shader_cache.end())
			{
				return found->second;
			}
		}

		::glsl::shader_properties properties{};
		properties.domain = ::glsl::program_domain::glsl_vertex_program;
		properties.require_lit_emulation = true;
		properties.require_clip_plane_functions = true;

		RSXVertexProgram null_prog;
		std::string shader_str;
		ParamArray arr;

		// Initialize binding layout
		auto vk_prog = std::make_shared<VKVertexProgram>();
		const u32 vertex_instruction_start = init(vk_prog, compiler_options);

		null_prog.ctrl = (compiler_options & COMPILER_OPT_ENABLE_INSTANCING)
			? RSX_SHADER_CONTROL_INSTANCED_CONSTANTS
			: 0;
		VKVertexDecompilerThread comp(null_prog, shader_str, arr, *vk_prog);

		// Initialize compiler properties
		comp.properties.has_indexed_constants = true;

		ParamType uniforms = { PF_PARAM_UNIFORM, "vec4" };
		uniforms.items.emplace_back("vc[468]", -1);

		std::stringstream builder;
		comp.insertHeader(builder);
		comp.insertConstants(builder, { uniforms });
		comp.insertInputs(builder, {});

		// Outputs
		builder << "layout(location=16) out flat uvec4 draw_params_payload;\n\n";

		builder <<
		"#define xform_constants_offset get_draw_params().xform_constants_offset\n"
		"#define scale_offset_mat get_vertex_context().scale_offset_mat\n"
		"#define transform_branch_bits get_vertex_context().transform_branch_bits\n"
		"#define point_size get_vertex_context().point_size\n"
		"#define z_near get_vertex_context().z_near\n"
		"#define z_far get_vertex_context().z_far\n\n";

		// Insert vp stream input
		builder << "\n"
		"layout(std140, set=0, binding=" << vertex_instruction_start << ") readonly restrict buffer VertexInstructionBlock\n"
		"{\n"
		"	uint base_address;\n"
		"	uint entry;\n"
		"	uint output_mask;\n"
		"	uint control;\n"
		"	uvec4 vp_instructions[];\n"
		"};\n\n";

		if (compiler_options & COMPILER_OPT_ENABLE_VTX_TEXTURES)
		{
			// FIXME: Unimplemented
			rsx_log.todo("Vertex textures are currently not implemented for the shader interpreter.");
		}

		if (compiler_options & COMPILER_OPT_ENABLE_INSTANCING)
		{
			builder << "#define _ENABLE_INSTANCED_CONSTANTS\n";
		}

		if (compiler_options)
		{
			builder << "\n";
		}

		::glsl::insert_glsl_legacy_function(builder, properties);
		::glsl::insert_vertex_input_fetch(builder, ::glsl::glsl_rules::glsl_rules_vulkan);
		comp.insertFSExport(builder);

		builder << program_common::interpreter::get_vertex_interpreter();
		const std::string s = builder.str();

		auto vs = &vk_prog->shader;
		vs->create(::glsl::program_domain::glsl_vertex_program, s);
		vs->compile();

		// Declare local inputs
		auto vs_inputs = comp.get_inputs();

		vs_inputs.push_back(vk::glsl::program_input::make
		(
			::glsl::glsl_vertex_program,
			"VertexInstructionBlock",
			glsl::input_type_storage_buffer,
			0,
			vertex_instruction_start
		));

		vk_prog->SetInputs(vs_inputs);

#ifdef RPCS3_PS4
		if (!vs->get_handle())
		{
			// 0.38: module refused by the driver (shader::compile): not cached, so the next link of these options builds
			// it again; this link is refused as not ready (program::link).
			return vk_prog;
		}
#endif

		std::lock_guard lock(m_vs_shader_cache_lock);
		m_vs_shader_cache[compiler_options] = vk_prog;
		return vk_prog;
	}

	std::shared_ptr<VKFragmentProgram> shader_interpreter::build_fs(u64 compiler_options)
	{
		compiler_options &= COMPILER_OPT_ALL_FS_MASK;
		{
			reader_lock lock(m_fs_shader_cache_lock);
			if (auto found = m_fs_shader_cache.find(compiler_options);
				found != m_fs_shader_cache.end())
			{
				return found->second;
			}
		}

		::glsl::shader_properties properties
		{
			.domain = ::glsl::program_domain::glsl_fragment_program,
			.require_lit_emulation = true,
			.ROP_channel_remap = !!(compiler_options & COMPILER_OPT_ENABLE_ROP_REMAP),
		};

		u32 len;
		ParamArray arr;
		std::string shader_str;
		RSXFragmentProgram frag;

		frag.ctrl |= RSX_SHADER_CONTROL_INTERPRETER_MODEL;

		auto vk_prog = std::make_shared<VKFragmentProgram>();
		const u32 fragment_instruction_start = init(vk_prog, compiler_options);
		const u32 fragment_textures_start = fragment_instruction_start + 1;

		VKFragmentDecompilerThread comp(shader_str, arr, frag, len, *vk_prog);

		std::stringstream builder;
		builder <<
		"#version 450\n"
		"#extension GL_EXT_scalar_block_layout : require\n"
		"#extension GL_EXT_uniform_buffer_unsized_array : require\n"
		"#extension GL_ARB_separate_shader_objects : enable\n\n";

		::glsl::insert_subheader_block(builder);
		comp.insertConstants(builder);

		builder << "layout(location=16) in flat uvec4 draw_params_payload;\n\n";

		builder <<
		"#define fog_param0 fs_contexts[_fs_context_offset].fog_param0\n"
		"#define fog_param1 fs_contexts[_fs_context_offset].fog_param1\n"
		"#define fog_mode fs_contexts[_fs_context_offset].fog_mode\n"
		"#define wpos_scale fs_contexts[_fs_context_offset].wpos_scale\n"
		"#define wpos_bias fs_contexts[_fs_context_offset].wpos_bias\n\n";

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_GE)
		{
			builder << "#define ALPHA_TEST_GEQUAL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_G)
		{
			builder << "#define ALPHA_TEST_GREATER\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_LE)
		{
			builder << "#define ALPHA_TEST_LEQUAL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_L)
		{
			builder << "#define ALPHA_TEST_LESS\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_EQ)
		{
			builder << "#define ALPHA_TEST_EQUAL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_NE)
		{
			builder << "#define ALPHA_TEST_NEQUAL\n";
		}

		if (!(compiler_options & COMPILER_OPT_ENABLE_F32_EXPORT))
		{
			builder << "#define WITH_HALF_OUTPUT_REGISTER\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_DEPTH_EXPORT)
		{
			builder << "#define WITH_DEPTH_EXPORT\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_FLOW_CTRL)
		{
			builder << "#define WITH_FLOW_CTRL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_PACKING)
		{
			builder << "#define WITH_PACKING\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_KIL)
		{
			builder << "#define WITH_KIL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_STIPPLING)
		{
			builder << "#define WITH_STIPPLING\n";
		}

		const char* type_names[] = { "sampler1D", "sampler2D", "sampler3D", "samplerCube" };
		if (compiler_options & COMPILER_OPT_ENABLE_TEXTURES)
		{
			builder << "#define WITH_TEXTURES\n\n";

			for (int i = 0, bind_location = fragment_textures_start; i < 4; ++i)
			{
				builder << "layout(set=1, binding=" << bind_location++ << ") " << "uniform " << type_names[i] << " " << type_names[i] << "_array[16];\n";
			}

			builder << "\n"
				"#undef  TEX_PARAM\n"
				"#define TEX_PARAM(index) texture_parameters[index + texture_base_index]\n"
				"#define IS_TEXTURE_RESIDENT(index) true\n"
				"#define SAMPLER1D(index) sampler1D_array[index]\n"
				"#define SAMPLER2D(index) sampler2D_array[index]\n"
				"#define SAMPLER3D(index) sampler3D_array[index]\n"
				"#define SAMPLERCUBE(index) samplerCube_array[index]\n"
				"#define texture_base_index _fs_texture_base_index\n\n";
		}

		builder <<
			"layout(std430, set=1, binding=" << fragment_instruction_start << ") readonly restrict buffer FragmentInstructionBlock\n"
			"{\n"
			"	uint shader_control;\n"
			"	uint texture_control;\n"
			"	uint reserved1;\n"
			"	uint reserved2;\n"
			"	uvec4 fp_instructions[];\n"
			"};\n\n";

		builder <<
			"	uint rop_control = fs_contexts[_fs_context_offset].rop_control;\n"
			"	float alpha_ref = fs_contexts[_fs_context_offset].alpha_ref;\n\n";

		::glsl::insert_glsl_legacy_function(builder, properties);
		builder << program_common::interpreter::get_fragment_interpreter();
		const std::string s = builder.str();

		auto fs = &vk_prog->shader;
		fs->create(::glsl::program_domain::glsl_fragment_program, s);
		fs->compile();

		// Declare local inputs
		auto inputs = comp.get_inputs();

		vk::glsl::program_input in = vk::glsl::program_input::make
		(
			::glsl::glsl_fragment_program,
			"FragmentInstructionBlock",
			glsl::input_type_storage_buffer,
			1,
			fragment_instruction_start
		);
		inputs.push_back(in);

		if (compiler_options & COMPILER_OPT_ENABLE_TEXTURES)
		{
			for (int i = 0, location = fragment_textures_start; i < 4; ++i, ++location)
			{
				in.location = location;
				in.name = std::string(type_names[i]) + "_array[16]";
				in.type = glsl::input_type_texture;
				inputs.push_back(in);
			}
		}

		vk_prog->SetInputs(inputs);

#ifdef RPCS3_PS4
		if (!fs->get_handle())
		{
			// 0.38: module refused by the driver: not cached (see build_vs).
			return vk_prog;
		}
#endif

		std::lock_guard lock(m_fs_shader_cache_lock);
		m_fs_shader_cache[compiler_options] = vk_prog;
		return vk_prog;
	}

	void shader_interpreter::init(const vk::render_device& dev)
	{
		m_device = dev;

#ifdef RPCS3_PS4
		// 0.38: the 0.37 preload linked 6650 pipelines before the game started (87 min on FIFA 14) and kept them all, and
		// a variant missing on first use was linked on the RSX thread (~1.6 s on the console). Now a missing variant is
		// linked on a pipe compiler thread in its generic form, whose pipeline every variant with the same exclusive
		// options can use; the draws that need it are skipped until it is ready. interpreter_only mode keeps the
		// synchronous exact link (no draw may be skipped there).
		m_ps4_async_links = g_cfg.video.shadermode == shader_mode::async_with_interpreter && !fs::is_file(rp::interpreter_sync_flag);
		m_ps4_lru = !fs::is_file(rp::interpreter_no_lru_flag);
		s_ps4_live.store(0, std::memory_order_relaxed);

		ps4_startup_checkpoint(fmt::format("Vulkan: shader interpreter variants link on first use, %s; %s",
			m_ps4_async_links ? "asynchronously in their generic form (interpreter-sync-link.flag: synchronous, as 0.37)" : "synchronously",
			m_ps4_lru ? fmt::format("at most %u pipelines kept, least recently used released (interpreter-no-lru.flag keeps all)",
				rp::interpreter_cap(s_ps4_cap_request.load(std::memory_order_relaxed)))
				: std::string("every pipeline kept (interpreter-no-lru.flag)")).c_str());
#endif
	}

	void shader_interpreter::destroy()
	{
		m_current_interpreter.reset();
		m_program_cache.clear();
		m_vs_shader_cache.clear();
		m_fs_shader_cache.clear();

#ifdef RPCS3_PS4
		// The device is idle and the pipe compiler threads are gone: nothing references these programs any more.
		m_ps4_pending.clear();
		m_ps4_refused.clear();
		m_ps4_retired.clear();
		m_ps4_retired_count.store(0, std::memory_order_relaxed);
		s_ps4_live.store(0, std::memory_order_relaxed);
#endif
	}

	std::shared_ptr<glsl::program> shader_interpreter::link(const vk::pipeline_props& properties, u64 compiler_opt, bool async, async_build_fn_callback async_callback)
	{
		auto vs = build_vs(compiler_opt);
		auto fs = build_fs(compiler_opt);

		async_pipe_compiler_context context{ properties, m_device, vs->shader.get_handle(), fs->shader.get_handle() };
		auto create_graphics_info_fn = [=]() mutable
		{
			return context.compile();
		};

		auto callback_fn = [=, this](std::unique_ptr<glsl::program>& prog)
		{
			if (!async)
			{
				return;
			}

			pipeline_key key{};
			key.compiler_opt = compiler_opt;
			key.properties = properties;

			std::shared_ptr<glsl::program> result = std::move(prog);
#ifdef RPCS3_PS4
			{
				std::lock_guard lock(this->m_program_cache_lock);
				this->m_ps4_pending.erase(key);

				if (!result)
				{
					// The driver refused the pipeline after a memory reclaim and one retry (pipe compiler): not cached,
					// the draws that need it stay skipped, and the next use links it again after a pause.
					this->m_ps4_refused[key] = get_system_time() + ps4_failed_link_retry_us;
				}
				else
				{
					auto& entry = this->m_program_cache[key];
					if (entry.program)
					{
						// The replaced program may be bound right now: the RSX thread releases it once it is not.
						this->m_ps4_retired.push_back(std::move(entry.program));
						this->m_ps4_retired_count.store(::size32(this->m_ps4_retired), std::memory_order_release);
					}

					entry.flags = 0;
					entry.program = result;
					entry.ps4_last_use = this->m_ps4_use_seq.load(std::memory_order_relaxed);
					s_ps4_live.store(::size32(this->m_program_cache), std::memory_order_relaxed);
				}
			}

			if (async_callback)
			{
				async_callback(result);
			}
#else
			std::lock_guard lock(this->m_program_cache_lock);

			pipeline_cache_entry_t cached_program
			{
				.flags = 0,
				.program = result
			};
			this->m_program_cache[key] = cached_program;

			if (async_callback)
			{
				async_callback(result);
			}
#endif
		};

		vk::pipe_compiler::op_flags flags = vk::pipe_compiler::SEPARATE_SHADER_OBJECTS;
		flags |= (async ? vk::pipe_compiler::COMPILE_DEFERRED : vk::pipe_compiler::COMPILE_INLINE);
#ifdef RPCS3_PS4
		// 0.39: queued links run on the pipe compiler's priority lane, ahead of queued game pipelines.
		flags |= vk::pipe_compiler::PS4_INTERPRETER_LINK;
#endif

		auto compiler = vk::get_pipe_compiler();
		auto program = compiler->compile(
			create_graphics_info_fn,
			flags,
			callback_fn,
			vs->uniforms,
			fs->uniforms);

		return program;
	}

	void shader_interpreter::update_fragment_textures(const std::array<VkDescriptorImageInfoEx, 68>& sampled_images)
	{
		// FIXME: Cannot use m_fragment_textures.start now since each interpreter has its own binding layout
		auto [set, binding] = m_current_interpreter->get_uniform_location(::glsl::glsl_fragment_program, glsl::input_type_texture, "sampler1D_array[16]");
		if (binding == umax)
		{
			return;
		}

		const VkDescriptorImageInfoEx* texture_ptr = sampled_images.data();
		for (u32 i = 0; i < 4; ++i, ++binding, texture_ptr += 16)
		{
			m_current_interpreter->bind_uniform_array({ texture_ptr, 16 }, set, binding);
		}
	}

	glsl::program* shader_interpreter::get(
		const vk::pipeline_props& properties,
		const program_hash_util::fragment_program_utils::fragment_program_metadata& fp_metadata,
		const program_hash_util::vertex_program_utils::vertex_program_metadata& vp_metadata,
		u32 vp_ctrl,
		u32 fp_ctrl)
	{
		pipeline_key key;
		key.compiler_opt = 0;
		key.properties = properties;

		if (rsx::method_registers.alpha_test_enabled()) [[unlikely]]
		{
			switch (rsx::method_registers.alpha_func())
			{
			case rsx::comparison_function::always:
				break;
			case rsx::comparison_function::never:
				return nullptr;
			case rsx::comparison_function::greater_or_equal:
				key.compiler_opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_GE;
				break;
			case rsx::comparison_function::greater:
				key.compiler_opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_G;
				break;
			case rsx::comparison_function::less_or_equal:
				key.compiler_opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_LE;
				break;
			case rsx::comparison_function::less:
				key.compiler_opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_L;
				break;
			case rsx::comparison_function::equal:
				key.compiler_opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_EQ;
				break;
			case rsx::comparison_function::not_equal:
				key.compiler_opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_NE;
				break;
			}
		}

		if (fp_ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT) key.compiler_opt |= COMPILER_OPT_ENABLE_DEPTH_EXPORT;
		if (fp_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) key.compiler_opt |= COMPILER_OPT_ENABLE_F32_EXPORT;
		if (fp_ctrl & RSX_SHADER_CONTROL_USES_KIL) key.compiler_opt |= COMPILER_OPT_ENABLE_KIL;
		if (fp_ctrl & RSX_SHADER_CONTROL_ROP_OUTPUT_REMAP) key.compiler_opt |= COMPILER_OPT_ENABLE_ROP_REMAP;
		if (fp_metadata.referenced_textures_mask) key.compiler_opt |= COMPILER_OPT_ENABLE_TEXTURES;
		if (fp_metadata.has_branch_instructions) key.compiler_opt |= COMPILER_OPT_ENABLE_FLOW_CTRL;
		if (fp_metadata.has_pack_instructions) key.compiler_opt |= COMPILER_OPT_ENABLE_PACKING;
		if (rsx::method_registers.polygon_stipple_enabled()) key.compiler_opt |= COMPILER_OPT_ENABLE_STIPPLING;
		if (vp_ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS) key.compiler_opt |= COMPILER_OPT_ENABLE_INSTANCING;
		if (vp_metadata.referenced_textures_mask) key.compiler_opt |= COMPILER_OPT_ENABLE_VTX_TEXTURES;

		if (m_current_key == key) [[likely]]
		{
			return m_current_interpreter.get();
		}
#ifdef RPCS3_PS4
		return ps4_get(key);
#else
		else
		{
			m_current_pipeline_info_ex = *get_pipeline_info_ex(key.compiler_opt);
			m_current_key = key;
		}

		{
			reader_lock lock(m_program_cache_lock);

			auto found = m_program_cache.find(key);
			if (found != m_program_cache.end()) [[likely]]
			{
				m_current_interpreter = found->second.program;

				if ((found->second.flags & (CACHED_PIPE_UNOPTIMIZED | CACHED_PIPE_RECOMPILING)) == CACHED_PIPE_UNOPTIMIZED)
				{
					found->second.flags |= CACHED_PIPE_RECOMPILING;
					link(properties, key.compiler_opt, true, {});
				}

				return m_current_interpreter.get();
			}
		}

		const auto start = std::chrono::steady_clock::now();

		m_current_interpreter = link(properties, key.compiler_opt);

		const auto end = std::chrono::steady_clock::now();

		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
		if (duration > std::chrono::milliseconds(1000))
		{
			rsx_log.error("Cache miss");
		}

		pipeline_cache_entry_t cache_entry
		{
			.flags = 0,
			.program = m_current_interpreter
		};

		std::lock_guard lock(m_program_cache_lock);
		m_program_cache[key] = cache_entry;
		return m_current_interpreter.get();
#endif
	}

#ifdef RPCS3_PS4
	glsl::program* shader_interpreter::ps4_get(const pipeline_key& key)
	{
		// RSX thread, the variant changed.
		if (m_ps4_retired_count.load(std::memory_order_acquire))
		{
			ps4_release_retired();
		}

		if (m_ps4_lru)
		{
			ps4_evict(rp::interpreter_cap(s_ps4_cap_request.load(std::memory_order_relaxed)));
		}

		const u64 seq = m_ps4_use_seq.fetch_add(1, std::memory_order_relaxed) + 1;
		const pipeline_key generic{ rp::compatible_opts(key.compiler_opt, ps4_base_vs_mask, ps4_base_fs_mask), key.properties };

		{
			reader_lock lock(m_program_cache_lock);

			if (auto found = m_program_cache.find(key); found != m_program_cache.end()) [[likely]]
			{
				found->second.ps4_last_use = seq;

				// 0.39: an entry of the variant preload (CACHED_PIPE_UNOPTIMIZED) holds the generic program until the exact
				// variant replaces it: bind it with the options its shaders were built with.
				const bool aliased = (found->second.flags & CACHED_PIPE_UNOPTIMIZED) != 0;
				ps4_bind(key, aliased ? generic.compiler_opt : key.compiler_opt, found->second.program);

				if ((found->second.flags & (CACHED_PIPE_UNOPTIMIZED | CACHED_PIPE_RECOMPILING)) == CACHED_PIPE_UNOPTIMIZED)
				{
					// Entries of the variant preload only: the exact variant replaces the generic one when linked.
					found->second.flags |= CACHED_PIPE_RECOMPILING;
					link(key.properties, key.compiler_opt, true, {});
				}

				return m_current_interpreter.get();
			}

			if (m_ps4_async_links)
			{
				// The generic pipeline draws every variant with the same exclusive options (as the preload's entries).
				if (auto found = m_program_cache.find(generic); found != m_program_cache.end())
				{
					found->second.ps4_last_use = seq;
					ps4_bind(key, generic.compiler_opt, found->second.program);
					return m_current_interpreter.get();
				}
			}
		}

		if (m_ps4_async_links)
		{
			// Not linked yet: queued for a pipe compiler thread. This draw is skipped and counted (VKDraw.cpp), like a
			// draw whose own pipeline is still compiling; the bound program stays as it is.
			ps4_queue_link(generic);
			return nullptr;
		}

		// interpreter_only mode or interpreter-sync-link.flag: the exact variant, linked here (0.37).
		const auto start = std::chrono::steady_clock::now();
		std::shared_ptr<glsl::program> program = link(key.properties, key.compiler_opt);
		const u64 us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();

		ps4_note_first_use_link(key.compiler_opt, us, false, !program);

		if (!program)
		{
			// Refused by the driver after a memory reclaim and one retry: this draw is skipped, the next one tries again.
			return nullptr;
		}

		{
			std::lock_guard lock(m_program_cache_lock);
			auto& entry = m_program_cache[key];
			if (entry.program)
			{
				m_ps4_retired.push_back(std::move(entry.program));
				m_ps4_retired_count.store(::size32(m_ps4_retired), std::memory_order_release);
			}

			entry.flags = 0;
			entry.program = program;
			entry.ps4_last_use = seq;
			s_ps4_live.store(::size32(m_program_cache), std::memory_order_relaxed);
		}

		ps4_bind(key, key.compiler_opt, program);
		return m_current_interpreter.get();
	}

	void shader_interpreter::ps4_bind(const pipeline_key& key, u64 shader_opt, const std::shared_ptr<glsl::program>& program)
	{
		// The previous program stays referenced by the cache or the retired list, so it is never destroyed here.
		m_current_pipeline_info_ex = *get_pipeline_info_ex(shader_opt);
		m_current_key = key;
		m_ps4_bound_opt = shader_opt;
		m_ps4_bound_textures = (shader_opt & COMPILER_OPT_ENABLE_TEXTURES) != 0;
		m_current_interpreter = program;
	}

	void shader_interpreter::ps4_queue_link(const pipeline_key& key)
	{
		const u64 now = get_system_time();
		{
			std::lock_guard lock(m_program_cache_lock);

			if (m_program_cache.find(key) != m_program_cache.end() || m_ps4_pending.find(key) != m_ps4_pending.end())
			{
				// Finished or queued meanwhile: a later draw of the variant finds it.
				return;
			}

			if (const auto refused = m_ps4_refused.find(key); refused != m_ps4_refused.end())
			{
				if (now < refused->second)
				{
					return;
				}

				m_ps4_refused.erase(refused);
			}

			if (m_ps4_pending.size() >= rp::interpreter_pending_max)
			{
				// Bounded backlog: the variant is queued by a later draw once a link finished.
				s_ps4_deferred.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			m_ps4_pending.insert(key);
		}

		// The shaders are built here (once per option set, cached), the pipeline on a pipe compiler thread.
		link(key.properties, key.compiler_opt, true, [opt = key.compiler_opt, now](std::shared_ptr<glsl::program>& program)
		{
			ps4_note_first_use_link(opt, get_system_time() - now, true, !program);
		});
	}

	void shader_interpreter::ps4_release_retired()
	{
		std::vector<std::shared_ptr<glsl::program>> release;
		{
			std::lock_guard lock(m_program_cache_lock);

			for (auto& program : m_ps4_retired)
			{
				if (program != m_current_interpreter)
				{
					release.push_back(std::move(program));
				}
			}

			std::erase_if(m_ps4_retired, [](const std::shared_ptr<glsl::program>& program) { return !program; });
			m_ps4_retired_count.store(::size32(m_ps4_retired), std::memory_order_release);
		}

		for (auto& program : release)
		{
			ps4_dispose_program(std::move(program));
		}
	}

	void shader_interpreter::ps4_evict(u32 cap)
	{
		{
			reader_lock lock(m_program_cache_lock);
			if (m_program_cache.size() <= cap)
			{
				return;
			}
		}

		std::vector<std::shared_ptr<glsl::program>> victims;
		u32 live = 0;
		{
			std::lock_guard lock(m_program_cache_lock);

			std::vector<decltype(m_program_cache)::iterator> entries;
			std::vector<u64> stamps;
			std::vector<bool> pinned;
			entries.reserve(m_program_cache.size());
			stamps.reserve(m_program_cache.size());
			pinned.reserve(m_program_cache.size());

			for (auto it = m_program_cache.begin(); it != m_program_cache.end(); ++it)
			{
				entries.push_back(it);
				stamps.push_back(it->second.ps4_last_use);
				pinned.push_back(it->second.program == m_current_interpreter);
			}

			for (const usz index : rp::lru_victims(stamps, pinned, cap))
			{
				victims.push_back(std::move(entries[index]->second.program));
				m_program_cache.erase(entries[index]);
			}

			live = ::size32(m_program_cache);
			s_ps4_live.store(live, std::memory_order_relaxed);
		}

		if (victims.empty())
		{
			return;
		}

		const u64 total = s_ps4_evicted.fetch_add(victims.size(), std::memory_order_relaxed) + victims.size();
		for (auto& program : victims)
		{
			ps4_dispose_program(std::move(program));
		}

		// RPCS3.log and startup.log: the 1st, 2nd, 4th, 8th... eviction pass of the process.
		if (const u32 pass = s_ps4_eviction_passes.fetch_add(1, std::memory_order_relaxed) + 1; !(pass & (pass - 1)))
		{
			const std::string line = fmt::format("Vulkan: interpreter pipelines live %u (cap %u), released %u least recently used (evicted %llu since boot)",
				live, cap, ::size32(victims), total);
			rsx_log.notice("PS4 %s", line);
			ps4_startup_checkpoint(line.c_str());
		}
	}
#endif

	bool shader_interpreter::is_interpreter(const glsl::program* prog) const
	{
		return prog == m_current_interpreter.get();
	}

	u32 shader_interpreter::get_vertex_instruction_location() const
	{
		return m_current_pipeline_info_ex.vertex_instruction_location;
	}

	u32 shader_interpreter::get_fragment_instruction_location() const
	{
		return m_current_pipeline_info_ex.fragment_instruction_location;
	}

	std::pair<std::shared_ptr<VKVertexProgram>, std::shared_ptr<VKFragmentProgram>> shader_interpreter::get_shaders() const
	{
#ifdef RPCS3_PS4
		// 0.38: the shaders the bound program was built from (its generic form when the variant is drawn with it).
		const auto vs_opt = m_ps4_bound_opt & COMPILER_OPT_ALL_VS_MASK;
		const auto fs_opt = m_ps4_bound_opt & COMPILER_OPT_ALL_FS_MASK;
#else
		const auto vs_opt = m_current_key.compiler_opt & COMPILER_OPT_ALL_VS_MASK;
		const auto fs_opt = m_current_key.compiler_opt & COMPILER_OPT_ALL_FS_MASK;
#endif

		std::shared_ptr<VKVertexProgram> vs;
		std::shared_ptr<VKFragmentProgram> fs;

		{
			reader_lock lock(m_vs_shader_cache_lock);
			if (auto found = m_vs_shader_cache.find(vs_opt);
				found != m_vs_shader_cache.end())
			{
				vs = found->second;
			}
		}

		{
			reader_lock lock(m_fs_shader_cache_lock);
			if (auto found = m_fs_shader_cache.find(fs_opt);
				found != m_fs_shader_cache.end())
			{
				fs = found->second;
			}
		}

		return { vs, fs };
	}

	const shader_interpreter::pipeline_info_ex_t* shader_interpreter::get_pipeline_info_ex(u64 compiler_opt)
	{
		if (auto found = m_pipeline_info_cache.find(compiler_opt); found != m_pipeline_info_cache.end())
		{
			return &found->second;
		}

		auto vs_stub = std::make_shared<VKVertexProgram>();
		auto fs_stub = std::make_shared<VKFragmentProgram>();
		const auto vi_location = init(vs_stub, compiler_opt);
		const auto fi_location = init(fs_stub, compiler_opt);

		pipeline_info_ex_t result
		{
			.vertex_instruction_location = vi_location,
			.fragment_instruction_location = fi_location,
			.fragment_textures_location = fi_location + 1
		};

		auto it = m_pipeline_info_cache.insert_or_assign(compiler_opt, result);
		return &it.first->second;
	}

	void shader_interpreter::preload(rsx::shader_loading_dialog* dlg)
	{
		dlg->create(get_localized_string(localized_string_id::RSX_OVERLAYS_COMPILING_SHADERS), get_localized_string(localized_string_id::RSX_OVERLAYS_COMPILING_SHADERS_TITLE));

		// Create some basic pipelines that we'll use to seed the base pipeline queue
		std::vector<vk::pipeline_props> pipe_properties;
		auto pdev = vk::get_current_renderer();

		// Collect format information we intend to use.
		// All formats to cover as many variants as possible
		const auto depth_format = pdev->get_formats_support().d24_unorm_s8 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
		std::vector<VkFormat> possible_color_formats =
		{
			VK_FORMAT_B8G8R8A8_UNORM,
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_FORMAT_R8G8_UNORM,
			VK_FORMAT_R32_SFLOAT,
			VK_FORMAT_R8_UNORM,

			// These formats are so rarely used, we can skip them and not stutter so badly
			// VK_FORMAT_R5G6B5_UNORM_PACK16,
			// VK_FORMAT_A1R5G5B5_UNORM_PACK16,
			// VK_FORMAT_R32G32B32A32_SFLOAT,
		};

		for (const auto surface_format : possible_color_formats)
		{
			// Base pipeline - simple color
			vk::pipeline_props base_props{};
			base_props.state.set_attachment_count(1);
			base_props.state.enable_cull_face(VK_CULL_MODE_BACK_BIT);
			base_props.state.set_primitive_type(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
			base_props.state.set_color_mask(0, true, true, true, true);
			base_props.state.set_attachment_count(1);
			base_props.state.enable_depth_bias(true);
			base_props.state.enable_depth_clamp(true);
			base_props.state.enable_depth_bounds_test(pdev->get_depth_bounds_support());
			base_props.renderpass_key = vk::get_renderpass_key(surface_format);
			pipe_properties.push_back(base_props);

			// Add a depth buffer
			base_props.renderpass_key = vk::get_renderpass_key(surface_format, depth_format);
			base_props.state.enable_depth_test(VK_COMPARE_OP_LESS);
			base_props.state.set_depth_mask(true);
			pipe_properties.push_back(base_props);

			// Add more targets. Typical G-buffers are 1-4 color targets + depth.
			for (u8 index = 1; index < 4; ++index)
			{
				base_props.renderpass_key = vk::get_renderpass_key(surface_format, depth_format, index + 1);
				base_props.state.set_attachment_count(index + 1);
				base_props.state.set_color_mask(index, true, true, true, true);
				pipe_properties.push_back(base_props);
			}

			// Add in some blending
			if (surface_format != VK_FORMAT_R32G32B32A32_SFLOAT &&
				surface_format != VK_FORMAT_R32_SFLOAT)
			{
				// Blending not supported for some F32 formats.
				base_props.renderpass_key = vk::get_renderpass_key(surface_format, depth_format);
				base_props.state.set_attachment_count(1);
				base_props.state.enable_blend(0,
					VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
					VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
					VK_BLEND_OP_ADD, VK_BLEND_OP_ADD);
				pipe_properties.push_back(base_props);
			}
		}

		const auto variants = program_common::interpreter::get_interpreter_variants();
		const u32 limit1 = ::size32(variants.base_pipelines) * ::size32(pipe_properties);
		const u32 limit2 = ::size32(variants.pipelines) * ::size32(pipe_properties);
		dlg->set_limit(0, limit1);
		dlg->set_limit(1, limit2);
#ifdef RPCS3_PS4
		// 0.37: pipelines linked below, for the 'interpreter variants ready in N ms (M pipelines)' line.
		s_ps4_preload_pipelines.store(limit1, std::memory_order_relaxed);

		// 0.38: the preload's entries (and the variants aliased to them) are meant to stay: no cap in this mode.
		m_ps4_lru = false;
#endif

		atomic_t<u32> ctr = 0;
		for (const auto& props : pipe_properties)
		{
			for (auto& variant : variants.base_pipelines)
			{
				pipeline_key key{};
				key.properties = props;
				key.compiler_opt = variant.first | variant.second;

				link(props, variant.first | variant.second, true, [&](std::shared_ptr<glsl::program>&) { ctr++; });
			}
		}

		// Drain the queue.
		// FIXME: Since the queue is executing from the context of the pipe compiler, we cannot properly stop this process.
		do
		{
			std::this_thread::sleep_for(16ms);

			const auto completed = ctr.load();
			dlg->update_msg(0, get_localized_string(localized_string_id::RSX_OVERLAYS_COMPILING_SHADERS_VULKAN, "%u %s %u", completed, get_localized_string(localized_string_id::PROGRESS_DIALOG_OF), limit1));
			dlg->set_value(0, completed);
		}
		while (ctr.load() < limit1);

		ctr = 0;

		for (const auto& props : pipe_properties)
		{
			{
				std::lock_guard lock(m_program_cache_lock);
				pipeline_key base_key;
				base_key.properties = props;

				for (auto& variant : variants.pipelines)
				{
					// Check if we have an exact match
					base_key.compiler_opt = variant.vs_opts.shader_opt | variant.fs_opts.shader_opt;
					if (auto found = m_program_cache.find(base_key);
						found != m_program_cache.end())
					{
						// We have a perfect match, no propagation required
						continue;
					}

					// Find a compatible pipeline
					auto compat_key = base_key;
					compat_key.compiler_opt = variant.vs_opts.compatible_shader_opts | variant.fs_opts.compatible_shader_opts;
					auto found = m_program_cache.find(compat_key);

#ifdef RPCS3_PS4
					if (found == m_program_cache.end())
					{
						// 0.38: the driver refused that generic pipeline: the variant links on first use.
						continue;
					}
#else
					ensure(found != m_program_cache.end(), "Invalid interpreter configuration.");
#endif

					pipeline_cache_entry_t cache_entry
					{
						.flags = CACHED_PIPE_UNOPTIMIZED,
						.program = found->second.program
					};
					m_program_cache[base_key] = cache_entry;
				}
			}
#ifdef RPCS3_PS4
            ctr += ::size32(variants.pipelines);
            dlg->update_msg(1, fmt::format("Preparando variantes %u de %u", ctr.load(), limit2));
            dlg->set_value(1, ctr.load());
#endif
		}

		dlg->set_value(1, limit2);
		dlg->refresh();
	}
};
