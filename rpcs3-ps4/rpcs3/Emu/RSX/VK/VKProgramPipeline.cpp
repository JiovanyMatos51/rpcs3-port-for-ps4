#include "stdafx.h"
#include "VKProgramPipeline.h"
#include "VKResourceManager.h"
#include "vkutils/descriptors.h"
#include "vkutils/device.h"
#include "vkutils/image.h"
#include "vkutils/sampler.h"

#include "../Program/SPIRVCommon.h"

#ifdef RPCS3_PS4
#include "VKPipelineCompiler.h"
#include "vkutils/scratch.h"
#include "Emu/system_config.h"
#include "ps4_fatal.h"
#include "ps4_launcher.h"
#include "util/ps4_rsx_programs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>
#endif

namespace vk
{
#ifdef RPCS3_PS4
	namespace
	{
		std::atomic<u32> s_ps4_shader_text_released{ 0 };
		std::atomic<u64> s_ps4_shader_text_released_bytes{ 0 };

		// 0.38: set by the pipe compiler around draw pipeline links (ps4_set_link_nullable).
		thread_local bool t_ps4_link_nullable = false;
		std::atomic<u32> s_ps4_links_refused{ 0 };

		// 0.38: program::bind recorded nothing because a descriptor set was refused (ps4_take_bind_refused).
		thread_local bool t_ps4_bind_refused = false;

		// A driver call of a link (shader module, set layout, pipeline layout, pipeline) that can fail when RADV runs out
		// of CPU heap or GPU window: memory is reclaimed and the call retried once. A refusal that survives the retry
		// ends the process as before unless the caller takes an unlinked program (draw pipelines).
		template <typename Call>
		bool ps4_link_call(const char* what, Call&& call)
		{
			VkResult result = call();
			if (result == VK_SUCCESS)
			{
				return true;
			}

			vk::ps4_reclaim_for_driver(result, what, true);
			result = call();
			if (result == VK_SUCCESS)
			{
				return true;
			}

			if (!t_ps4_link_nullable)
			{
				vk::die_with_error(result);
			}

			// Allocation-free: the CPU heap may be exhausted. First 16 refusals and then every power of two.
			const u32 count = s_ps4_links_refused.fetch_add(1, std::memory_order_relaxed) + 1;
			if (count <= 16 || !(count & (count - 1)))
			{
				char detail[192];
				std::snprintf(detail, sizeof(detail), "%s still failed after the retry (VkResult %d): the pipeline is not ready, "
					"its draws use the shader interpreter or are skipped (%u refused links so far)", what, static_cast<int>(result), count);
				ps4_fatal_note("GPU: ", detail);
			}

			return false;
		}

		// 0.36: every program kept its descriptor pools until the game stopped; in Wolverine 0.35 they reached 2466
		// pools (at least 48 KiB of GPU window each) by the 23-min fatal. Tables of draw programs are registered here;
		// the mutex orders a table's destruction (any thread) against the sweep (RSX thread).
		std::mutex s_ps4_table_mutex;
		std::vector<glsl::descriptor_table_t*> s_ps4_tables;

		// Sweep epochs: a commit stamps the current one, each sweep starts the next one and records when.
		std::atomic<u64> s_ps4_pool_epoch{ 0 };
		constexpr u64 ps4_epoch_history = 64;
		std::array<u64, ps4_epoch_history> s_ps4_epoch_start_us{};
		std::atomic<u32> s_ps4_tables_released{ 0 };

		// 0.38: a memory request for the next sweep (ps4_request_descriptor_pool_release), sweeps that released tables
		// for the window cap.
		std::atomic<bool> s_ps4_pool_release_request{ false };
		std::atomic<u32> s_ps4_window_sweeps{ 0 };

		// 0.39: RADV reads the sampler of every combined image sampler write (radv_UpdateDescriptorSets) and faults on
		// VK_NULL_HANDLE; a null image view is legal. Such a slot is written with the null sampler instead, counted for
		// the backend line (unbound_sampler_slots, 0 expected) and the first one is named in startup.log.
		void ps4_note_unbound_sampler(const glsl::descriptor_table_t& table, u32 binding, const char* where)
		{
			const u64 count = utils::ps4::rsx_programs::g_pipeline_stats.unbound_sampler_slots.fetch_add(1, std::memory_order_relaxed);
			if (count != 0)
			{
				return;
			}

			// The first one: the shader input of the slot names the program kind (interpreter arrays: 'sampler2D_array[16]').
			std::string name = "?";
			u32 set = umax;
			for (const auto& input : table.m_inputs[glsl::input_type_texture])
			{
				if (input.location == binding)
				{
					name = input.name;
					set = input.set;
					break;
				}
			}

			const std::string line = fmt::format("Vulkan: combined image sampler slot without a sampler (binding %u '%s' of set %u, %s): "
				"written with the null sampler; later ones are counted in the Vulkan backend line (unbound sampler slots)",
				binding, name, set, where);
			rsx_log.warning("PS4 %s", line);
			ps4_startup_checkpoint(line.c_str());
		}

		bool ps4_fix_null_sampler(VkDescriptorImageInfo& info, VkDescriptorType type) noexcept
		{
			if (type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || info.sampler != VK_NULL_HANDLE)
			{
				return false;
			}

			info.sampler = vk::null_sampler();
			return true;
		}
	}

	// vkutils/descriptors.cpp (0.38): window bytes of pools already handed to the garbage collector.
	u64 ps4_descriptor_pool_window_releasing() noexcept;

	u32 ps4_shader_text_released_count() noexcept
	{
		return s_ps4_shader_text_released.load(std::memory_order_relaxed);
	}

	u64 ps4_shader_text_released_bytes() noexcept
	{
		return s_ps4_shader_text_released_bytes.load(std::memory_order_relaxed);
	}

	bool ps4_set_link_nullable(bool nullable) noexcept
	{
		return std::exchange(t_ps4_link_nullable, nullable);
	}

	bool ps4_link_nullable() noexcept
	{
		return t_ps4_link_nullable;
	}

	namespace glsl
	{
		void ps4_dispose_pipeline(std::unique_ptr<program>&& pipeline)
		{
			if (pipeline)
			{
				vk::get_gc()->dispose(pipeline);
			}
		}

		u32 ps4_descriptor_pools_released() noexcept
		{
			return s_ps4_tables_released.load(std::memory_order_relaxed);
		}

		u32 ps4_sweep_descriptor_pools(u64 now_us)
		{
			constexpr u64 idle_us = 20'000'000;
			constexpr u32 live_pool_cap = 1024;

			const u64 epoch = s_ps4_pool_epoch.load(std::memory_order_relaxed);

			// Newest epoch that started at least 20 s ago: a table last committed before it has been idle that long.
			u64 idle_epoch = 0;
			for (u64 e = epoch; e > 0 && epoch - e < ps4_epoch_history; --e)
			{
				const u64 start = s_ps4_epoch_start_us[e % ps4_epoch_history];
				if (start && start + idle_us <= now_us)
				{
					idle_epoch = e;
					break;
				}
			}

			u32 released = 0;
			u32 pools_released = 0;
			u32 window_released = 0;
			u32 live = 0;
			{
				std::lock_guard lock(s_ps4_table_mutex);

				if (idle_epoch)
				{
					for (descriptor_table_t* table : s_ps4_tables)
					{
						if (table->m_descriptor_pool && table->m_ps4_last_use < idle_epoch)
						{
							pools_released += table->ps4_release_pool();
							released++;
						}
					}
				}

				// Released pools are destroyed later by the garbage collector: count them as gone already.
				live = vk::ps4_descriptor_pools_live();

				// 0.38: also while the live pools' estimated GPU window exceeds 48 MiB, and all of them (not committed
				// since the previous sweep) after a memory request (ps4_request_descriptor_pool_release).
				const bool forced = s_ps4_pool_release_request.exchange(false, std::memory_order_relaxed);
				const auto over_window = []()
				{
					const u64 total = vk::ps4_descriptor_pool_window_bytes();
					const u64 releasing = std::min(total, vk::ps4_descriptor_pool_window_releasing());
					return total - releasing > utils::ps4::rsx_programs::pool_window_cap;
				};

				if (live > live_pool_cap + pools_released || forced || over_window())
				{
					u32 excess = live > live_pool_cap + pools_released ? live - pools_released - live_pool_cap : 0;

					// Least recently used first, never a table committed since the previous sweep.
					std::vector<descriptor_table_t*> candidates;
					for (descriptor_table_t* table : s_ps4_tables)
					{
						if (table->m_descriptor_pool && table->m_ps4_last_use < epoch)
						{
							candidates.push_back(table);
						}
					}

					std::sort(candidates.begin(), candidates.end(), [](const descriptor_table_t* a, const descriptor_table_t* b)
					{
						return a->m_ps4_last_use < b->m_ps4_last_use;
					});

					for (descriptor_table_t* table : candidates)
					{
						const bool window_release = !excess && !forced && over_window();
						if (!excess && !forced && !window_release)
						{
							break;
						}

						const u32 pools = table->ps4_release_pool();
						pools_released += pools;
						released++;
						excess -= std::min(excess, pools);
						window_released += window_release ? 1 : 0;
					}
				}
			}

			s_ps4_epoch_start_us[(epoch + 1) % ps4_epoch_history] = now_us;
			s_ps4_pool_epoch.store(epoch + 1, std::memory_order_relaxed);

			if (released)
			{
				const u32 total = s_ps4_tables_released.fetch_add(released, std::memory_order_relaxed) + released;
				rsx_log.notice("PS4 VRAM: released the descriptor pools of %u idle program table(s), %u VkDescriptorPool(s); %u live before (%u tables in this process)",
					released, pools_released, live, total);

				if (total == released)
				{
					ps4_startup_checkpoint(fmt::format("VRAM: released the descriptor pools of %u program table(s) (%u VkDescriptorPool(s)) not used for 20 s, "
						"or least recently used above %u live pools; later releases are counted in the PS4 VRAM line (no-pool-release.flag disables this)",
						released, pools_released, live_pool_cap).c_str());
				}

				// 0.38: the 1st, 2nd, 4th... sweep that released tables for the window cap.
				if (window_released)
				{
					if (const u32 n = s_ps4_window_sweeps.fetch_add(1, std::memory_order_relaxed) + 1; !(n & (n - 1)))
					{
						ps4_startup_checkpoint(fmt::format("VRAM: descriptor pools above %llu MiB of estimated GPU window: released %u least recently used "
							"program table(s) (sweep %u of this kind)", utils::ps4::rsx_programs::pool_window_cap >> 20, window_released, n).c_str());
					}
				}
			}

			return released;
		}

		void ps4_request_descriptor_pool_release() noexcept
		{
			s_ps4_pool_release_request.store(true, std::memory_order_relaxed);
		}

		bool ps4_take_bind_refused() noexcept
		{
			return std::exchange(t_ps4_bind_refused, false);
		}
	}
#endif

	namespace glsl
	{
		using namespace ::glsl;

		bool operator == (const descriptor_slot_t& a, const VkDescriptorImageInfoEx& b)
		{
			const auto ptr = std::get_if<VkDescriptorImageInfoEx>(&a);
			return !!ptr &&
				ptr->resourceId == b.resourceId &&
				ptr->imageView == b.imageView &&
				ptr->sampler == b.sampler &&
				ptr->imageLayout == b.imageLayout;
		}

		bool operator == (const descriptor_slot_t& a, const VkDescriptorBufferInfoEx& b)
		{
			const auto ptr = std::get_if<VkDescriptorBufferInfoEx>(&a);
			return !!ptr &&
				ptr->resourceId == b.resourceId &&
				ptr->buffer == b.buffer &&
				ptr->offset == b.offset &&
				ptr->range == b.range;
		}

		bool operator == (const descriptor_slot_t& a, const VkDescriptorBufferViewEx& b)
		{
			const auto ptr = std::get_if<VkDescriptorBufferViewEx>(&a);
			return !!ptr && ptr->resourceId == b.resourceId;
		}

		bool operator == (const descriptor_slot_t& a, const std::span<const VkDescriptorImageInfoEx>& b)
		{
			const auto ptr = std::get_if<descriptor_image_array_t>(&a);
			return !!ptr && ptr->size() == b.size() && !std::memcmp(ptr->data(), b.data(), b.size_bytes());
		}

		VkDescriptorType to_descriptor_type(program_input_type type)
		{
			switch (type)
			{
			case input_type_uniform_buffer:
				return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			case input_type_texel_buffer:
				return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
			case input_type_texture:
				return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			case input_type_storage_buffer:
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case input_type_storage_texture:
				return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			case input_type_attachment:
				return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
			default:
				fmt::throw_exception("Unexpected program input type %d", static_cast<int>(type));
			}
		}

		VkShaderStageFlags to_shader_stage_flags(::glsl::program_domain domain)
		{
			switch (domain)
			{
			case glsl_vertex_program:
				return VK_SHADER_STAGE_VERTEX_BIT;
			case glsl_fragment_program:
				return VK_SHADER_STAGE_FRAGMENT_BIT;
			case glsl_compute_program:
				return VK_SHADER_STAGE_COMPUTE_BIT;
			default:
				fmt::throw_exception("Unexpected domain %d", static_cast<int>(domain));
			}
		}

		const char* to_string(::glsl::program_domain domain)
		{
			switch (domain)
			{
			case glsl_vertex_program:
				return "vertex";
			case glsl_fragment_program:
				return "fragment";
			case glsl_compute_program:
				return "compute";
			default:
				fmt::throw_exception("Unexpected domain %d", static_cast<int>(domain));
			}
		}

		void shader::create(::glsl::program_domain domain, const std::string& source)
		{
			type     = domain;
			m_source = source;
		}

		VkShaderModule shader::compile()
		{
			ensure(m_handle == VK_NULL_HANDLE);

#ifdef RPCS3_PS4
			// 0.38: a module the driver refused keeps its SPIR-V (released only on success below); a later retry of the
			// module (program_state_cache) reuses it instead of compiling the GLSL again.
			if (m_compiled.empty() && !spirv::compile_glsl_to_spv(m_compiled, m_source, type, ::glsl::glsl_rules_vulkan))
#else
			if (!spirv::compile_glsl_to_spv(m_compiled, m_source, type, ::glsl::glsl_rules_vulkan))
#endif
			{
				rsx_log.notice("%s", m_source);
				fmt::throw_exception("Failed to compile %s shader", to_string(type));
			}

			VkShaderModuleCreateInfo vs_info;
			vs_info.codeSize = m_compiled.size() * sizeof(u32);
			vs_info.pNext    = nullptr;
			vs_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			vs_info.pCode    = m_compiled.data();
			vs_info.flags    = 0;

#ifdef RPCS3_PS4
			// 0.38: the result was ignored: a module RADV refused (CPU heap) left a null handle that crashed in the driver
			// later. Now memory is reclaimed and the call retried once; a module still refused stays null with its SPIR-V
			// kept, its pipelines are not ready (program::link refuses a null module), and the shader caches create it
			// again later (program_state_cache: at most every 30 s; shader interpreter: such shaders are not cached).
			VkResult module_result = vkCreateShaderModule(*g_render_device, &vs_info, nullptr, &m_handle);
			if (module_result != VK_SUCCESS)
			{
				m_handle = VK_NULL_HANDLE;
				vk::ps4_reclaim_for_driver(module_result, "vkCreateShaderModule", true);
				module_result = vkCreateShaderModule(*g_render_device, &vs_info, nullptr, &m_handle);

				if (module_result != VK_SUCCESS)
				{
					m_handle = VK_NULL_HANDLE;
					utils::ps4::rsx_programs::g_pipeline_stats.modules_refused.fetch_add(1, std::memory_order_relaxed);

					// Allocation-free; the first 16 and then every power of two.
					if (const u32 count = s_ps4_links_refused.fetch_add(1, std::memory_order_relaxed) + 1; count <= 16 || !(count & (count - 1)))
					{
						char detail[160];
						std::snprintf(detail, sizeof(detail), "vkCreateShaderModule still failed after the retry (VkResult %d): "
							"pipelines of this %s shader are not ready (%u refusals so far)", static_cast<int>(module_result), to_string(type), count);
						ps4_fatal_note("GPU: ", detail);
					}
				}
			}

			if (module_result == VK_SUCCESS && !g_cfg.video.log_programs)
			{
				// Shaders live until the game stops, and nothing reads the GLSL text or the SPIR-V words once the
				// module exists (the shader log dump runs before compile()). Release both: tens of KiB per shader
				// of the 2 GiB CPU heap.
				const u64 bytes = m_source.capacity() + m_compiled.capacity() * sizeof(u32);
				std::string().swap(m_source);
				std::vector<u32>().swap(m_compiled);
				s_ps4_shader_text_released.fetch_add(1, std::memory_order_relaxed);
				s_ps4_shader_text_released_bytes.fetch_add(bytes, std::memory_order_relaxed);
			}
#else
			vkCreateShaderModule(*g_render_device, &vs_info, nullptr, &m_handle);
#endif

			return m_handle;
		}

		void shader::destroy()
		{
			m_source.clear();
			m_compiled.clear();

			if (m_handle)
			{
				vkDestroyShaderModule(*g_render_device, m_handle, nullptr);
				m_handle = nullptr;
			}
		}

		const std::string& shader::get_source() const
		{
			return m_source;
		}

		const std::vector<u32> shader::get_compiled() const
		{
			return m_compiled;
		}

		VkShaderModule shader::get_handle() const
		{
			return m_handle;
		}

		void program::init()
		{
			m_linked = false;
		}

		program::program(VkDevice dev, const VkGraphicsPipelineCreateInfo& create_info, const std::vector<program_input> &vertex_inputs, const std::vector<program_input>& fragment_inputs)
			: m_device(dev), m_info(create_info)
		{
			init();

			load_uniforms(vertex_inputs);
			load_uniforms(fragment_inputs);
		}

		program::program(VkDevice dev, const VkComputePipelineCreateInfo& create_info, const std::vector<program_input>& compute_inputs)
			: m_device(dev), m_info(create_info)
		{
			init();

			load_uniforms(compute_inputs);
		}

		program::~program()
		{
			vkDestroyPipeline(m_device, m_pipeline, nullptr);

			if (m_pipeline_layout)
			{
				vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);

				for (auto& set : m_sets)
				{
					set.destroy();
				}
			}
		}

		program& program::load_uniforms(const std::vector<program_input>& inputs)
		{
			ensure(!m_linked); // "Cannot change uniforms in already linked program!"

			for (auto &item : inputs)
			{
				ensure(item.set < binding_set_index_max_enum);                         // Ensure we have a valid set id
				ensure(item.location < 128u || item.type == input_type_push_constant); // Arbitrary limit but useful to catch possibly uninitialized values
				m_sets[item.set].m_inputs[item.type].push_back(item);
			}

			return *this;
		}

		program& program::link(VkPipelineCache pipeline_cache, bool separate_objects)
		{
			auto p_graphics_info = std::get_if<VkGraphicsPipelineCreateInfo>(&m_info);
			auto p_compute_info = !p_graphics_info ? std::get_if<VkComputePipelineCreateInfo>(&m_info) : nullptr;
			const bool is_graphics_pipe = p_graphics_info != nullptr;

			if (!is_graphics_pipe) [[ likely ]]
			{
				// We only support compute and graphics, so disable this for compute
				separate_objects = false;
			}

			if (!separate_objects)
			{
				// Collapse all sets into set 0 if validation passed
				auto& sink = m_sets[0];
				for (auto& set : m_sets)
				{
					if (&set == &sink)
					{
						continue;
					}

					for (auto& type_arr : set.m_inputs)
					{
						if (type_arr.empty())
						{
							continue;
						}

						auto type = type_arr.front().type;
						auto& dst = sink.m_inputs[type];
						dst.insert(dst.end(), type_arr.begin(), type_arr.end());

						// Clear
						type_arr.clear();
					}
				}

				sink.validate();
				sink.init(m_device);
			}
			else
			{
				for (auto& set : m_sets)
				{
					for (auto& type_arr : set.m_inputs)
					{
						if (type_arr.empty())
						{
							continue;
						}

						// Real set
						set.validate();
						set.init(m_device);
						break;
					}
				}
			}

			create_pipeline_layout();
#ifdef RPCS3_PS4
			// 0.38: every driver call of a link reclaims memory and retries once (ps4_link_call); a draw pipeline the
			// driver still refuses stays unlinked (value() == VK_NULL_HANDLE) and its caller treats it as not ready.
			m_linked = true;

			if (!m_pipeline_layout)
			{
				return *this;
			}

			if (is_graphics_pipe)
			{
				VkGraphicsPipelineCreateInfo create_info = *p_graphics_info;
				create_info.layout = m_pipeline_layout;

				for (u32 stage = 0; stage < create_info.stageCount; ++stage)
				{
					if (!create_info.pStages[stage].module)
					{
						// The shader module was refused (shader::compile): the driver would crash on it.
						if (!t_ps4_link_nullable)
						{
							vk::die_with_error(VK_ERROR_OUT_OF_HOST_MEMORY);
						}

						s_ps4_links_refused.fetch_add(1, std::memory_order_relaxed);
						return *this;
					}
				}

				ps4_link_call("vkCreateGraphicsPipelines", [&]()
				{
					m_pipeline = VK_NULL_HANDLE;
					return vkCreateGraphicsPipelines(m_device, pipeline_cache, 1, &create_info, nullptr, &m_pipeline);
				});
			}
			else
			{
				VkComputePipelineCreateInfo create_info = *p_compute_info;
				create_info.layout = m_pipeline_layout;

				ps4_link_call("vkCreateComputePipelines", [&]()
				{
					m_pipeline = VK_NULL_HANDLE;
					return vkCreateComputePipelines(m_device, pipeline_cache, 1, &create_info, nullptr, &m_pipeline);
				});
			}
#else
			ensure(m_pipeline_layout);

			if (is_graphics_pipe)
			{
				VkGraphicsPipelineCreateInfo create_info = *p_graphics_info;
				create_info.layout = m_pipeline_layout;
				CHECK_RESULT(vkCreateGraphicsPipelines(m_device, pipeline_cache, 1, &create_info, nullptr, &m_pipeline));
			}
			else
			{
				VkComputePipelineCreateInfo create_info = *p_compute_info;
				create_info.layout = m_pipeline_layout;
				CHECK_RESULT(vkCreateComputePipelines(m_device, pipeline_cache, 1, &create_info, nullptr, &m_pipeline));
			}

			m_linked = true;
#endif
			return *this;
		}

		bool program::has_uniform(program_input_type type, std::string_view uniform_name)
		{
			for (auto& set : m_sets)
			{
				const auto& uniform = set.m_inputs[type];
				return std::any_of(uniform.cbegin(), uniform.cend(), [&uniform_name](const auto& u)
				{
					return u.name == uniform_name;
				});
			}

			return false;
		}

		std::pair<u32, u32> program::get_uniform_location(::glsl::program_domain domain, program_input_type type, std::string_view uniform_name)
		{
			for (unsigned i = 0; i < ::size32(m_sets); ++i)
			{
				const auto& type_arr = m_sets[i].m_inputs[type];
				const auto result = std::find_if(type_arr.cbegin(), type_arr.cend(), [&](const auto& u)
				{
					return u.domain == domain && u.name == uniform_name;
				});

				if (result != type_arr.end())
				{
					return { i, result->location };
				}
			}

			return { umax, umax };
		}

		void program::bind_uniform(const VkDescriptorImageInfoEx& image_descriptor, u32 set_id, u32 binding_point)
		{
			if (m_sets[set_id].m_descriptor_slots[binding_point] == image_descriptor)
			{
				return;
			}

			m_sets[set_id].notify_descriptor_slot_updated(binding_point, image_descriptor);
		}

		void program::bind_uniform(const VkDescriptorBufferInfoEx &buffer_descriptor, u32 set_id, u32 binding_point)
		{
			if (m_sets[set_id].m_descriptor_slots[binding_point] == buffer_descriptor)
			{
				return;
			}

			m_sets[set_id].notify_descriptor_slot_updated(binding_point, buffer_descriptor);
		}

		void program::bind_uniform(const VkDescriptorBufferViewEx& buffer_view, u32 set_id, u32 binding_point)
		{
			if (m_sets[set_id].m_descriptor_slots[binding_point] == buffer_view)
			{
				return;
			}

			m_sets[set_id].notify_descriptor_slot_updated(binding_point, buffer_view);
		}

		void program::bind_uniform_array(const std::span<const VkDescriptorImageInfoEx>& image_descriptors, u32 set_id, u32 binding_point)
		{
			if (m_sets[set_id].m_descriptor_slots[binding_point] == image_descriptors)
			{
				return;
			}

			m_sets[set_id].notify_descriptor_slot_updated(binding_point, image_descriptors);
		}

		void program::create_pipeline_layout()
		{
			ensure(!m_linked);
			ensure(m_pipeline_layout == VK_NULL_HANDLE);

			rsx::simple_array<VkPushConstantRange> push_constants{};
			rsx::simple_array<VkDescriptorSetLayout> set_layouts{};

			for (auto& set : m_sets)
			{
				if (!set.m_device)
				{
					continue;
				}

				set.create_descriptor_set_layout();
#ifdef RPCS3_PS4
				if (!set.m_descriptor_set_layout)
				{
					// 0.38: refused after a reclaim and one retry (draw pipelines only, descriptors::create_layout): the
					// program stays unlinked.
					s_ps4_links_refused.fetch_add(1, std::memory_order_relaxed);
					for (auto& table : m_sets)
					{
						table.destroy();
					}

					return;
				}
#endif
				set_layouts.push_back(set.m_descriptor_set_layout);

				for (const auto& input : set.m_inputs[input_type_push_constant])
				{
					const auto& range = input.as_push_constant();
					push_constants.push_back({
						.stageFlags = to_shader_stage_flags(input.domain),
						.offset = range.offset,
						.size = range.size
						});
				}
			}

			VkPipelineLayoutCreateInfo create_info
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.flags = 0,
				.setLayoutCount = set_layouts.size(),
				.pSetLayouts = set_layouts.data(),
				.pushConstantRangeCount = push_constants.size(),
				.pPushConstantRanges = push_constants.data()
			};
#ifdef RPCS3_PS4
			if (!ps4_link_call("vkCreatePipelineLayout", [&]()
			{
				m_pipeline_layout = VK_NULL_HANDLE;
				return vkCreatePipelineLayout(m_device, &create_info, nullptr, &m_pipeline_layout);
			}))
			{
				// Refused (draw pipelines only): no layout, so the destructor would not release the set layouts.
				m_pipeline_layout = VK_NULL_HANDLE;
				for (auto& table : m_sets)
				{
					table.destroy();
				}
			}
#else
			CHECK_RESULT(vkCreatePipelineLayout(m_device, &create_info, nullptr, &m_pipeline_layout));
#endif
		}

		program& program::bind(const vk::command_buffer& cmd, VkPipelineBindPoint bind_point)
		{
			VkDescriptorSet bind_sets[binding_set_index_max_enum];
			unsigned count = 0;

			for (auto& set : m_sets)
			{
				if (!set.m_device)
				{
					continue;
				}

				bind_sets[count++] = set.commit();   // Commit variable changes and return handle to the new set
			}

#ifdef RPCS3_PS4
			// 0.38: a draw's set the driver refused (ps4_set_descriptor_nullable): nothing is recorded, the caller skips
			// the draw (ps4_take_bind_refused).
			for (unsigned i = 0; i < count; ++i)
			{
				if (!bind_sets[i])
				{
					t_ps4_bind_refused = true;
					return *this;
				}
			}
#endif

			cmd.bind_pipeline(m_pipeline, bind_point);
			cmd.bind_descriptor_sets({ bind_sets, count }, bind_point, m_pipeline_layout);
			return *this;
		}

		void descriptor_table_t::destroy()
		{
#ifdef RPCS3_PS4
			{
				// Leave the registry before the pool goes away, so a sweep never sees a destroyed table.
				std::lock_guard lock(s_ps4_table_mutex);
				if (m_ps4_registry_index != umax)
				{
					descriptor_table_t* const last = s_ps4_tables.back();
					s_ps4_tables[m_ps4_registry_index] = last;
					last->m_ps4_registry_index = m_ps4_registry_index;
					s_ps4_tables.pop_back();
					m_ps4_registry_index = umax;
				}
			}
#endif
			if (!m_device)
			{
				return;
			}

			if (m_descriptor_set_layout)
			{
				vkDestroyDescriptorSetLayout(m_device, m_descriptor_set_layout, nullptr);
			}

			if (m_descriptor_pool)
			{
				m_descriptor_pool->destroy();
				m_descriptor_pool.reset();
			}

			m_device = VK_NULL_HANDLE;
		}

		void descriptor_table_t::init(VkDevice dev)
		{
			m_device = dev;

			size_t bind_slots_count = 0;
			for (auto& type_arr : m_inputs)
			{
				if (type_arr.empty() || type_arr.front().type == input_type_push_constant)
				{
					continue;
				}

				bind_slots_count += type_arr.size();
			}

			m_descriptor_slots.resize(bind_slots_count);
			std::fill(m_descriptor_slots.begin(), m_descriptor_slots.end(), descriptor_slot_t{});

			m_descriptors_dirty.resize(bind_slots_count);
			std::fill(m_descriptors_dirty.begin(), m_descriptors_dirty.end(), false);
		}

		VkDescriptorSet descriptor_table_t::allocate_descriptor_set()
		{
			if (!m_descriptor_pool)
			{
				create_descriptor_pool();
			}

			return m_descriptor_pool->allocate(m_descriptor_set_layout);
		}

		void descriptor_table_t::create_descriptor_template()
		{
			auto push_descriptor_slot = [this](unsigned idx, VkWriteDescriptorSet& writer)
			{
				const auto& slot = m_descriptor_slots[idx];
				const VkDescriptorType type = m_descriptor_types[idx];

				writer.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writer.dstBinding = idx;
				writer.descriptorCount = 1;
				writer.descriptorType = type;

				if (auto ptr = std::get_if<VkDescriptorImageInfoEx>(&slot))
				{
#ifdef RPCS3_PS4
					VkDescriptorImageInfo info = *ptr;
					if (ps4_fix_null_sampler(info, type))
					{
						ps4_note_unbound_sampler(*this, idx, "first commit");
					}

					m_descriptor_set.push(info, type, idx);
#else
					m_descriptor_set.push(*ptr, type, idx);
#endif
					return;
				}

				if (auto ptr = std::get_if<VkDescriptorBufferInfoEx>(&slot))
				{
					m_descriptor_set.push(*ptr, type, idx);
					return;
				}

				if (auto ptr = std::get_if<VkDescriptorBufferViewEx>(&slot))
				{
					m_descriptor_set.push(ptr->view, type, idx);
					return;
				}

				if (auto ptr = std::get_if<descriptor_image_array_t>(&slot))
				{
					// We need to convert the VkDescriptorImageInfoEx entries back to the native vulkan variants since we're going to be flushing an array with no stride check
					auto vk_data = ptr->map(FN(static_cast<VkDescriptorImageInfo>(x)));

#ifdef RPCS3_PS4
					// 0.39: never more entries than the binding declares (16), never a null sampler.
					const u32 count = utils::ps4::rsx_programs::image_array_write_count(vk_data.size());
					for (u32 i = 0; i < count; ++i)
					{
						if (ps4_fix_null_sampler(vk_data[i], type))
						{
							ps4_note_unbound_sampler(*this, idx, "first commit, array");
						}
					}

					writer.descriptorCount = count;
					m_descriptor_set.push(vk_data.data(), count, type, idx);
#else
					writer.descriptorCount = ptr->size();
					m_descriptor_set.push(vk_data.data(), vk_data.size(), type, idx);
#endif
					return;
				}

				fmt::throw_exception("Unexpected descriptor structure at index %u", idx);
			};

			m_descriptor_template_typemask = 0u;
			m_descriptor_template.clear();

			for (unsigned i = 0; i < m_descriptor_slots.size(); ++i)
			{
				m_descriptor_template_typemask |= (1u << static_cast<u32>(m_descriptor_types[i]));
				VkWriteDescriptorSet _template{};

				push_descriptor_slot(i, _template);
				m_descriptors_dirty[i] = false;
				m_descriptor_template.push_back(_template);
			}

			m_descriptor_template_cache_id = umax;
			ensure(m_descriptor_template.size() == m_descriptor_slots.size());
		}

		void descriptor_table_t::update_descriptor_template()
		{
			auto update_descriptor_slot = [this](unsigned idx)
			{
				const auto& slot = m_descriptor_slots[idx];

				if (auto ptr = std::get_if<VkDescriptorImageInfoEx>(&slot))
				{
#ifdef RPCS3_PS4
					VkDescriptorImageInfo info = *ptr;
					if (ps4_fix_null_sampler(info, m_descriptor_types[idx]))
					{
						ps4_note_unbound_sampler(*this, idx, "update");
					}

					// 0.39: one entry (the slot may have held an array before).
					m_descriptor_template[idx].descriptorCount = 1;
					m_descriptor_template[idx].pImageInfo = m_descriptor_set.store(info);
#else
					m_descriptor_template[idx].pImageInfo = m_descriptor_set.store(*ptr);
#endif
					return;
				}

				if (auto ptr = std::get_if<VkDescriptorBufferInfoEx>(&slot))
				{
					m_descriptor_template[idx].pBufferInfo = m_descriptor_set.store(*ptr);
					return;
				}

				if (auto ptr = std::get_if<VkDescriptorBufferViewEx>(&slot))
				{
					m_descriptor_template[idx].pTexelBufferView = m_descriptor_set.store(ptr->view);
					return;
				}

				if (auto ptr = std::get_if<descriptor_image_array_t>(&slot))
				{
					auto vk_data = ptr->map(FN(static_cast<VkDescriptorImageInfo>(x))); // This can be optimized to update only changed ids but this is an interpreter-only feature for now
#ifdef RPCS3_PS4
					// 0.39: the first commit may have written this slot as a single (unbound) image, so the write count
					// follows the array (at most the 16 entries the binding declares) instead of asserting the old count.
					const u32 count = utils::ps4::rsx_programs::image_array_write_count(vk_data.size());
					for (u32 i = 0; i < count; ++i)
					{
						if (ps4_fix_null_sampler(vk_data[i], m_descriptor_types[idx]))
						{
							ps4_note_unbound_sampler(*this, idx, "update, array");
						}
					}

					m_descriptor_template[idx].descriptorCount = count;
#else
					ensure(m_descriptor_template[idx].descriptorCount == ptr->size());
#endif
					m_descriptor_template[idx].pImageInfo = m_descriptor_set.store(vk_data);
					return;
				}

				fmt::throw_exception("Unexpected descriptor structure at index %u", idx);
			};

			{
				std::lock_guard lock(m_descriptor_set);
				const bool cache_is_valid = m_descriptor_template_cache_id == m_descriptor_set.cache_id();

				for (unsigned i = 0; i < m_descriptor_slots.size(); ++i)
				{
					m_descriptor_template[i].dstSet = m_descriptor_set.value();
					if (!m_descriptors_dirty[i] && cache_is_valid)
					{
						continue;
					}

					// Update
					update_descriptor_slot(i);
					m_descriptors_dirty[i] = false;
				}
			}

			// Push
			m_descriptor_set.push(m_descriptor_template, m_descriptor_template_typemask);
			m_descriptor_template_cache_id = m_descriptor_set.cache_id();
		}

		VkDescriptorSet descriptor_table_t::commit()
		{
#ifdef RPCS3_PS4
			// Stamped before the early return: rebinding a clean set is a use too.
			m_ps4_last_use = s_ps4_pool_epoch.load(std::memory_order_relaxed);
#endif
			if (!m_descriptor_set)
			{
				m_any_descriptors_dirty = true;
				std::fill(m_descriptors_dirty.begin(), m_descriptors_dirty.end(), false);
			}

			// Check if we need to actually open a new set
			if (!m_any_descriptors_dirty)
			{
				return m_descriptor_set.value();
			}

#ifdef RPCS3_PS4
			// 0.38: refused for a draw (only with ps4_set_descriptor_nullable): the current set and the dirty state stay
			// as they are, so the next commit allocates again.
			const VkDescriptorSet ps4_new_set = allocate_descriptor_set();
			if (!ps4_new_set)
			{
				return VK_NULL_HANDLE;
			}

			m_descriptor_set = ps4_new_set;
#else
			m_descriptor_set = allocate_descriptor_set();
#endif

			if (!m_descriptor_template.empty()) [[ likely ]]
			{
				// Run pointer updates. Optimized for cached back-to-back updates which are quite frequent.
				update_descriptor_template();
			}
			else
			{
				// Creating the template also seeds initial values
				create_descriptor_template();
			}

			m_descriptor_set.on_bind();
			m_any_descriptors_dirty = false;

			return m_descriptor_set.value();
		}

		void descriptor_table_t::create_descriptor_set_layout()
		{
			ensure(m_descriptor_set_layout == VK_NULL_HANDLE);

			rsx::simple_array<VkDescriptorSetLayoutBinding> bindings;
			bindings.reserve(16);

			m_descriptor_pool_sizes.clear();
			m_descriptor_pool_sizes.reserve(input_type_max_enum);

			std::unordered_map<u32, VkDescriptorType> descriptor_type_map;

			auto descriptor_count = [](std::string_view name) -> u32
			{
				const auto start = name.find_last_of("[");
				if (start == std::string::npos)
				{
					return 1;
				}

				const auto end = name.find_last_of("]");
				ensure(end != std::string::npos && start < end, "Invalid variable name");

				const std::string_view array_size = name.substr(start + 1, end - start - 1);
				if (const auto count = std::atoi(array_size.data());
					count > 0)
				{
					return count;
				}

				return 1;
			};

			for (const auto& type_arr : m_inputs)
			{
				if (type_arr.empty() || type_arr.front().type == input_type_push_constant)
				{
					continue;
				}

				VkDescriptorType type = to_descriptor_type(type_arr.front().type);
				m_descriptor_pool_sizes.push_back({ .type = type });

				for (const auto& input : type_arr)
				{
					VkDescriptorSetLayoutBinding binding
					{
						.binding = input.location,
						.descriptorType = type,
						.descriptorCount = descriptor_count(input.name),
						.stageFlags = to_shader_stage_flags(input.domain) | input.ex_stages
					};
					bindings.push_back(binding);

					descriptor_type_map[input.location] = type;
					m_descriptor_pool_sizes.back().descriptorCount += binding.descriptorCount;
				}
			}

			m_descriptor_types.resize(::size32(m_descriptors_dirty));

			for (u32 i = 0; i < ::size32(m_descriptors_dirty); ++i)
			{
				if (descriptor_type_map.find(i) == descriptor_type_map.end())
				{
					fmt::throw_exception("Invalid input structure. Some input bindings were not declared!");
				}
				m_descriptor_types[i] = descriptor_type_map[i];
			}

			m_descriptor_set_layout = vk::descriptors::create_layout(bindings);
		}

		void descriptor_table_t::create_descriptor_pool()
		{
			m_descriptor_pool = std::make_unique<descriptor_pool>();
#ifdef RPCS3_PS4
			// Pools autoscale by doubling up to this many sets. RADV backs each pool with CPU heap (set objects)
			// and a descriptor buffer in the shared GPU window, and every program keeps its pools until the game
			// stops: the console caps them at 1024 sets (hot programs cycle subpools a little more often).
			// 0.38: a set of the shader interpreter's fragment table holds 64 combined image samplers (~6 KiB of window per
			// set, ~6 MiB per 1024-set subpool): those pools stop growing at 64 sets.
			u32 image_samplers = 0;
			for (const auto& size : m_descriptor_pool_sizes)
			{
				image_samplers += size.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? size.descriptorCount : 0;
			}

			m_descriptor_pool->create(*vk::g_render_device, m_descriptor_pool_sizes, 16u, utils::ps4::rsx_programs::pool_max_sets(image_samplers, 1024u));
#else
			m_descriptor_pool->create(*vk::g_render_device, m_descriptor_pool_sizes, 16u, 4096u);
#endif
		}

#ifdef RPCS3_PS4
		u32 descriptor_table_t::ps4_release_pool()
		{
			if (!m_descriptor_pool)
			{
				return 0;
			}

			const u32 pools = m_descriptor_pool->ps4_subpool_count();
			m_descriptor_set.ps4_release();

			// The template's pointers led into the freed scratch arrays: the next commit stores every slot again.
			m_descriptor_template_cache_id = umax;
			descriptor_pool::ps4_dispose(m_descriptor_pool);
			return pools;
		}

		void program::ps4_note_draw_program()
		{
			if (m_ps4_draw_program)
			{
				return;
			}

			m_ps4_draw_program = true;
			std::lock_guard lock(s_ps4_table_mutex);

			for (auto& set : m_sets)
			{
				if (set.m_device && set.m_ps4_registry_index == umax)
				{
					set.m_ps4_registry_index = ::size32(s_ps4_tables);
					s_ps4_tables.push_back(&set);
				}
			}
		}
#endif

		void descriptor_table_t::validate() const
		{
			// Check for overlapping locations
			std::set<u32> taken_locations;

			for (auto& type_arr : m_inputs)
			{
				if (type_arr.empty() ||
					type_arr.front().type == input_type_push_constant)
				{
					continue;
				}

				for (const auto& input : type_arr)
				{
					ensure(taken_locations.find(input.location) == taken_locations.end(), "Overlapping input locations found.");
					taken_locations.insert(input.location);
				}
			}
		}
	}
}
