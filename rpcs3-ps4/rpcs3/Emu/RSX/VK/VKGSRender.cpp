#include "Emu/RSX/VK/vkutils/descriptors.h"
#include "stdafx.h"
#ifdef RPCS3_PS4
#include "ps4_fatal.h"
#include "ps4_launcher.h"
#include "VKDMA.h"
#include "util/ps4_adapt.hpp"
#include "util/ps4_gpu_health.hpp"
#include "util/ps4_memory_governor.hpp"
#include "util/ps4_policy.hpp"
#include "util/ps4_rsx_programs.hpp"
#include "util/ps4_safe_mode.hpp"
#include "util/ps4_vram_policy.hpp"
#include "Emu/System.h"
#include "Emu/Cell/SPUThread.h"
#include "Emu/RSX/ps4_readback_state.h"
#include "vkutils/memory.h"

#include <optional>
#include <thread>
#endif
#include "../Overlays/overlay_compile_notification.h"
#include "../Overlays/Shaders/shader_loading_dialog_native.h"

#include "VKAsyncScheduler.h"
#include "VKCommandStream.h"
#include "VKCommonPipelineLayout.h"
#include "VKCompute.h"
#include "VKGSRender.h"
#include "VKHelpers.h"
#include "VKRenderPass.h"
#include "VKResourceManager.h"

#include "vkutils/buffer_object.h"
#include "vkutils/scratch.h"

#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/Host/MM.h"
#include "Emu/RSX/Host/RSXDMAWriter.h"
#include "Emu/RSX/NV47/HW/context_accessors.define.h"
#include "Emu/Memory/vm_locking.h"

#include "../Program/SPIRVCommon.h"

#include "util/asm.hpp"
#include <vulkan/vulkan_core.h>

namespace vk
{
	VkCompareOp get_compare_func(rsx::comparison_function op, bool reverse_direction = false);

	std::pair<VkFormat, VkComponentMapping> get_compatible_surface_format(rsx::surface_color_format color_format)
	{
		const VkComponentMapping o_rgb = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ONE };
		const VkComponentMapping z_rgb = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ZERO };

		switch (color_format)
		{
#if !defined(__APPLE__) || !defined(ARCH_X64)
		case rsx::surface_color_format::r5g6b5:
			return std::make_pair(VK_FORMAT_R5G6B5_UNORM_PACK16, vk::default_component_map);

		case rsx::surface_color_format::x1r5g5b5_o1r5g5b5:
			return std::make_pair(VK_FORMAT_A1R5G5B5_UNORM_PACK16, o_rgb);

		case rsx::surface_color_format::x1r5g5b5_z1r5g5b5:
			return std::make_pair(VK_FORMAT_A1R5G5B5_UNORM_PACK16, z_rgb);
#else
		// assign B8G8R8A8_UNORM to formats that are not supported by Metal on non-Apple GPUs
		case rsx::surface_color_format::r5g6b5:
			return std::make_pair(VK_FORMAT_B8G8R8A8_UNORM, vk::default_component_map);

		case rsx::surface_color_format::x1r5g5b5_o1r5g5b5:
			return std::make_pair(VK_FORMAT_B8G8R8A8_UNORM, o_rgb);

		case rsx::surface_color_format::x1r5g5b5_z1r5g5b5:
			return std::make_pair(VK_FORMAT_B8G8R8A8_UNORM, z_rgb);
#endif
		case rsx::surface_color_format::a8r8g8b8:
			return std::make_pair(VK_FORMAT_B8G8R8A8_UNORM, vk::default_component_map);

		case rsx::surface_color_format::a8b8g8r8:
			return std::make_pair(VK_FORMAT_R8G8B8A8_UNORM, vk::default_component_map);

		case rsx::surface_color_format::x8b8g8r8_o8b8g8r8:
			return std::make_pair(VK_FORMAT_R8G8B8A8_UNORM, o_rgb);

		case rsx::surface_color_format::x8b8g8r8_z8b8g8r8:
			return std::make_pair(VK_FORMAT_R8G8B8A8_UNORM, z_rgb);

		case rsx::surface_color_format::x8r8g8b8_z8r8g8b8:
			return std::make_pair(VK_FORMAT_B8G8R8A8_UNORM, z_rgb);

		case rsx::surface_color_format::x8r8g8b8_o8r8g8b8:
			return std::make_pair(VK_FORMAT_B8G8R8A8_UNORM, o_rgb);

		case rsx::surface_color_format::w16z16y16x16:
			return std::make_pair(VK_FORMAT_R16G16B16A16_SFLOAT, vk::default_component_map);

		case rsx::surface_color_format::w32z32y32x32:
			return std::make_pair(VK_FORMAT_R32G32B32A32_SFLOAT, vk::default_component_map);

		case rsx::surface_color_format::b8:
		{
			const VkComponentMapping no_alpha = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
			return std::make_pair(VK_FORMAT_R8_UNORM, no_alpha);
		}

		case rsx::surface_color_format::g8b8:
		{
			const VkComponentMapping gb_rg = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G };
			return std::make_pair(VK_FORMAT_R8G8_UNORM, gb_rg);
		}

		case rsx::surface_color_format::x32:
		{
			const VkComponentMapping rrrr = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R };
			return std::make_pair(VK_FORMAT_R32_SFLOAT, rrrr);
		}

		default:
			rsx_log.error("Surface color buffer: Unsupported surface color format (0x%x)", static_cast<u32>(color_format));
			return std::make_pair(VK_FORMAT_B8G8R8A8_UNORM, vk::default_component_map);
		}
	}

	VkLogicOp get_logic_op(rsx::logic_op op)
	{
		switch (op)
		{
		case rsx::logic_op::logic_clear: return VK_LOGIC_OP_CLEAR;
		case rsx::logic_op::logic_and: return VK_LOGIC_OP_AND;
		case rsx::logic_op::logic_and_reverse: return VK_LOGIC_OP_AND_REVERSE;
		case rsx::logic_op::logic_copy: return VK_LOGIC_OP_COPY;
		case rsx::logic_op::logic_and_inverted: return VK_LOGIC_OP_AND_INVERTED;
		case rsx::logic_op::logic_noop: return VK_LOGIC_OP_NO_OP;
		case rsx::logic_op::logic_xor: return VK_LOGIC_OP_XOR;
		case rsx::logic_op::logic_or : return VK_LOGIC_OP_OR;
		case rsx::logic_op::logic_nor: return VK_LOGIC_OP_NOR;
		case rsx::logic_op::logic_equiv: return VK_LOGIC_OP_EQUIVALENT;
		case rsx::logic_op::logic_invert: return VK_LOGIC_OP_INVERT;
		case rsx::logic_op::logic_or_reverse: return VK_LOGIC_OP_OR_REVERSE;
		case rsx::logic_op::logic_copy_inverted: return VK_LOGIC_OP_COPY_INVERTED;
		case rsx::logic_op::logic_or_inverted: return VK_LOGIC_OP_OR_INVERTED;
		case rsx::logic_op::logic_nand: return VK_LOGIC_OP_NAND;
		case rsx::logic_op::logic_set: return VK_LOGIC_OP_SET;
		default:
			fmt::throw_exception("Unknown logic op 0x%x", static_cast<u32>(op));
		}
	}

	VkBlendFactor get_blend_factor(rsx::blend_factor factor)
	{
		switch (factor)
		{
		case rsx::blend_factor::one: return VK_BLEND_FACTOR_ONE;
		case rsx::blend_factor::zero: return VK_BLEND_FACTOR_ZERO;
		case rsx::blend_factor::src_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
		case rsx::blend_factor::dst_alpha: return VK_BLEND_FACTOR_DST_ALPHA;
		case rsx::blend_factor::src_color: return VK_BLEND_FACTOR_SRC_COLOR;
		case rsx::blend_factor::dst_color: return VK_BLEND_FACTOR_DST_COLOR;
		case rsx::blend_factor::constant_color: return VK_BLEND_FACTOR_CONSTANT_COLOR;
		case rsx::blend_factor::constant_alpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
		case rsx::blend_factor::one_minus_src_color: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case rsx::blend_factor::one_minus_dst_color: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case rsx::blend_factor::one_minus_src_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case rsx::blend_factor::one_minus_dst_alpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case rsx::blend_factor::one_minus_constant_alpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
		case rsx::blend_factor::one_minus_constant_color: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
		case rsx::blend_factor::src_alpha_saturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		default:
			fmt::throw_exception("Unknown blend factor 0x%x", static_cast<u32>(factor));
		}
	}

	VkBlendOp get_blend_op(rsx::blend_equation op)
	{
		switch (op)
		{
		case rsx::blend_equation::add_signed:
			rsx_log.error("blend equation add_signed used. Emulating using FUNC_ADD");
			[[fallthrough]];
		case rsx::blend_equation::add: return VK_BLEND_OP_ADD;
		case rsx::blend_equation::reverse_add_signed:
			rsx_log.error("blend equation reverse_add_signed used. Emulating using FUNC_ADD");
			return VK_BLEND_OP_ADD;
		case rsx::blend_equation::subtract: return VK_BLEND_OP_SUBTRACT;
		case rsx::blend_equation::reverse_subtract_signed:
			rsx_log.error("blend equation reverse_subtract_signed used. Emulating using FUNC_REVERSE_SUBTRACT");
			[[fallthrough]];
		case rsx::blend_equation::reverse_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
		case rsx::blend_equation::min: return VK_BLEND_OP_MIN;
		case rsx::blend_equation::max: return VK_BLEND_OP_MAX;
		default:
			fmt::throw_exception("Unknown blend op: 0x%x", static_cast<u32>(op));
		}
	}

	VkStencilOp get_stencil_op(rsx::stencil_op op)
	{
		switch (op)
		{
		case rsx::stencil_op::keep: return VK_STENCIL_OP_KEEP;
		case rsx::stencil_op::zero: return VK_STENCIL_OP_ZERO;
		case rsx::stencil_op::replace: return VK_STENCIL_OP_REPLACE;
		case rsx::stencil_op::incr: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
		case rsx::stencil_op::decr: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
		case rsx::stencil_op::invert: return VK_STENCIL_OP_INVERT;
		case rsx::stencil_op::incr_wrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
		case rsx::stencil_op::decr_wrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
		default:
			fmt::throw_exception("Unknown stencil op: 0x%x", static_cast<u32>(op));
		}
	}

	VkFrontFace get_front_face(rsx::front_face ffv)
	{
		switch (ffv)
		{
		case rsx::front_face::cw: return VK_FRONT_FACE_CLOCKWISE;
		case rsx::front_face::ccw: return VK_FRONT_FACE_COUNTER_CLOCKWISE;
		default:
			fmt::throw_exception("Unknown front face value: 0x%x", static_cast<u32>(ffv));
		}
	}

	VkCullModeFlags get_cull_face(rsx::cull_face cfv)
	{
		switch (cfv)
		{
		case rsx::cull_face::back: return VK_CULL_MODE_BACK_BIT;
		case rsx::cull_face::front: return VK_CULL_MODE_FRONT_BIT;
		case rsx::cull_face::front_and_back: return VK_CULL_MODE_FRONT_AND_BACK;
		default:
			fmt::throw_exception("Unknown cull face value: 0x%x", static_cast<u32>(cfv));
		}
	}

	struct vertex_input_assembly_state
	{
		VkPrimitiveTopology primitive;
		VkBool32 restart_index_enabled;
	};

	vertex_input_assembly_state decode_vertex_input_assembly_state()
	{
		vertex_input_assembly_state state{};
		const auto& current_draw = rsx::method_registers.current_draw_clause;
		const auto [primitive, emulated_primitive] = vk::get_appropriate_topology(current_draw.primitive);

		if (rsx::method_registers.restart_index_enabled() &&
			!current_draw.is_disjoint_primitive &&
			current_draw.command == rsx::draw_command::indexed &&
			!emulated_primitive &&
			!vk::emulate_primitive_restart(current_draw.primitive))
		{
			state.restart_index_enabled = VK_TRUE;
		}

		state.primitive = primitive;
		return state;
	}

	// TODO: This should be deprecated soon (kd)
	vk::pipeline_props decode_rsx_state(
		const rsx::context* ctx,
		const vertex_input_assembly_state& vertex_input,
		vk::render_target* ds,
		const rsx::backend_configuration& backend_config,
		u8 num_draw_buffers,
		u8 num_rasterization_samples,
		bool depth_bounds_support,
		bool force_disable_blending)
	{
		vk::pipeline_props properties{};

		// Input assembly
		properties.state.set_primitive_type(vertex_input.primitive);
		properties.state.enable_primitive_restart(vertex_input.restart_index_enabled);

		// Rasterizer state
		properties.state.set_attachment_count(num_draw_buffers);
		properties.state.set_front_face(vk::get_front_face(REGS(ctx)->front_face_mode()));
		properties.state.enable_depth_clamp(REGS(ctx)->depth_clamp_enabled() || !REGS(ctx)->depth_clip_enabled());
		properties.state.enable_depth_bias(true);
		properties.state.enable_depth_bounds_test(depth_bounds_support);

		if (REGS(ctx)->depth_test_enabled())
		{
			//NOTE: Like stencil, depth write is meaningless without depth test
			properties.state.set_depth_mask(REGS(ctx)->depth_write_enabled());
			properties.state.enable_depth_test(vk::get_compare_func(REGS(ctx)->depth_func()));
		}

		if (REGS(ctx)->cull_face_enabled())
		{
			properties.state.enable_cull_face(vk::get_cull_face(REGS(ctx)->cull_face_mode()));
		}

		const auto host_write_mask = rsx::get_write_output_mask(REGS(ctx)->surface_color());
		for (uint index = 0; index < num_draw_buffers; ++index)
		{
			bool color_mask_b = REGS(ctx)->color_mask_b(index);
			bool color_mask_g = REGS(ctx)->color_mask_g(index);
			bool color_mask_r = REGS(ctx)->color_mask_r(index);
			bool color_mask_a = REGS(ctx)->color_mask_a(index);

			switch (REGS(ctx)->surface_color())
			{
			case rsx::surface_color_format::b8:
				rsx::get_b8_colormask(color_mask_r, color_mask_g, color_mask_b, color_mask_a);
				break;
			case rsx::surface_color_format::g8b8:
				rsx::get_g8b8_r8g8_colormask(color_mask_r, color_mask_g, color_mask_b, color_mask_a);
				break;
			default:
				break;
			}

			properties.state.set_color_mask(
				index,
				color_mask_r && host_write_mask[0],
				color_mask_g && host_write_mask[1],
				color_mask_b && host_write_mask[2],
				color_mask_a && host_write_mask[3]);
		}

		// LogicOp and Blend are mutually exclusive. If both are enabled, LogicOp takes precedence.
		if (REGS(ctx)->logic_op_enabled())
		{
			properties.state.enable_logic_op(vk::get_logic_op(REGS(ctx)->logic_operation()));
		}
		else if (!force_disable_blending)
		{
			VkBlendFactor sfactor_rgb, sfactor_a, dfactor_rgb, dfactor_a;
			VkBlendOp equation_rgb, equation_a;

			if (const auto blend_enabled = REGS(ctx)->blend_enabled_mask())
			{
				sfactor_rgb = vk::get_blend_factor(REGS(ctx)->blend_func_sfactor_rgb());
				sfactor_a = vk::get_blend_factor(REGS(ctx)->blend_func_sfactor_a());
				dfactor_rgb = vk::get_blend_factor(REGS(ctx)->blend_func_dfactor_rgb());
				dfactor_a = vk::get_blend_factor(REGS(ctx)->blend_func_dfactor_a());
				equation_rgb = vk::get_blend_op(REGS(ctx)->blend_equation_rgb());
				equation_a = vk::get_blend_op(REGS(ctx)->blend_equation_a());

				for (u8 idx = 0; idx < num_draw_buffers; ++idx)
				{
					if (blend_enabled & (1u << idx))
					{
						properties.state.enable_blend(idx, sfactor_rgb, sfactor_a, dfactor_rgb, dfactor_a, equation_rgb, equation_a);
					}
				}
			}
		}

		if (REGS(ctx)->stencil_test_enabled())
		{
			if (!REGS(ctx)->two_sided_stencil_test_enabled())
			{
				properties.state.enable_stencil_test(
					vk::get_stencil_op(REGS(ctx)->stencil_op_fail()),
					vk::get_stencil_op(REGS(ctx)->stencil_op_zfail()),
					vk::get_stencil_op(REGS(ctx)->stencil_op_zpass()),
					vk::get_compare_func(REGS(ctx)->stencil_func()),
					0xFF, 0xFF); //write mask, func_mask, ref are dynamic
			}
			else
			{
				properties.state.enable_stencil_test_separate(0,
					vk::get_stencil_op(REGS(ctx)->stencil_op_fail()),
					vk::get_stencil_op(REGS(ctx)->stencil_op_zfail()),
					vk::get_stencil_op(REGS(ctx)->stencil_op_zpass()),
					vk::get_compare_func(REGS(ctx)->stencil_func()),
					0xFF, 0xFF); //write mask, func_mask, ref are dynamic

				properties.state.enable_stencil_test_separate(1,
					vk::get_stencil_op(REGS(ctx)->back_stencil_op_fail()),
					vk::get_stencil_op(REGS(ctx)->back_stencil_op_zfail()),
					vk::get_stencil_op(REGS(ctx)->back_stencil_op_zpass()),
					vk::get_compare_func(REGS(ctx)->back_stencil_func()),
					0xFF, 0xFF); //write mask, func_mask, ref are dynamic
			}

			if (ds && ds->samples() > 1 && !(ds->stencil_init_flags & 0xFF00))
			{
				if (properties.state.ds.front.failOp != VK_STENCIL_OP_KEEP ||
					properties.state.ds.front.depthFailOp != VK_STENCIL_OP_KEEP ||
					properties.state.ds.front.passOp != VK_STENCIL_OP_KEEP ||
					properties.state.ds.back.failOp != VK_STENCIL_OP_KEEP ||
					properties.state.ds.back.depthFailOp != VK_STENCIL_OP_KEEP ||
					properties.state.ds.back.passOp != VK_STENCIL_OP_KEEP)
				{
					// Toggle bit 9 to signal require full bit-wise transfer
					ds->stencil_init_flags |= (1 << 8);
				}
			}
		}

		if (backend_config.supports_hw_a2c || num_rasterization_samples > 1)
		{
			const bool alpha_to_one_enable = REGS(ctx)->msaa_alpha_to_one_enabled() && backend_config.supports_hw_a2one;

			properties.state.set_multisample_state(
				num_rasterization_samples,
				REGS(ctx)->msaa_sample_mask(),
				REGS(ctx)->msaa_enabled(),
				REGS(ctx)->msaa_alpha_to_coverage_enabled(),
				alpha_to_one_enable);

			// A problem observed on multiple GPUs is that interior geometry edges can resolve 0 samples unless we force shading rate of 1.
			// For whatever reason, the way MSAA images are 'resolved' on PS3 bypasses this issue.
			// NOTE: We do not do image resolve at all, the output is merely 'exploded' and the guest application is responsible for doing the resolve in software as it is on real hardware.
			properties.state.set_multisample_shading_rate(1.f);
		}

		return properties;
	}
}

#ifdef RPCS3_PS4
namespace
{
	// rsx::data_heap::s_ps4_make_room while ring reclaim is on.
	bool ps4_ring_make_room_hook(rsx::data_heap& heap, usz size)
	{
		auto renderer = dynamic_cast<VKGSRender*>(rsx::get_current_renderer());
		return renderer && renderer->ps4_make_ring_room(heap, size);
	}

	// VKGSRender instances created in this process: a second game in one process runs on a second VkInstance.
	std::atomic<u32> s_ps4_instances_created{ 0 };

	// 0.37: access-violation flushes that recorded at least one readback. The faulting threads add, the 10 s 'Vulkan
	// backend' line (RSX thread) reads and resets.
	struct ps4_av_readback_stats_t
	{
		std::atomic<u64> flushes{ 0 };
		std::atomic<u64> total_us{ 0 };
		std::atomic<u64> max_us{ 0 };
		std::atomic<u64> request_us{ 0 };
		std::atomic<u64> drain_us{ 0 };
		std::atomic<u64> copy_us{ 0 };
		std::atomic<u64> swap_us{ 0 };
	};

	ps4_av_readback_stats_t s_ps4_av_readbacks;
	std::atomic<u32> s_ps4_av_readbacks_timed{ 0 }; // per renderer: numbers the timing lines
	std::atomic<u32> s_ps4_av_readbacks_slow{ 0 };  // ... of 50 ms or more

	// End of an access-violation flush, on the faulting thread: one timing line for the 1st-16th flush with a readback
	// and the first 16 slow ones (startup.log), more in RPCS3.log, and the sums for the 10 s line. 'swap' is the CPU
	// part after the GPU wait: the DMA mirror written into guest memory (byteswapped there with dma.flag cpu_swap).
	void ps4_note_av_readback(const spu_thread* spu, u64 start, u64 requested, u64 drained, u64 end)
	{
		const vk::ps4_readback_tls_t& rb = vk::g_ps4_readback_tls;
		if (!rb.count)
		{
			// Every section was synchronized already: no GPU readback in this flush.
			return;
		}

		// The GPU part ends with this thread's newest DMA submit wait (the drain's, if nothing was recorded after it).
		const u64 copied = std::clamp(rb.submit_done_us, drained, end);
		const u64 request_us = requested - start;
		const u64 drain_us = drained - requested;
		const u64 copy_us = copied - drained;
		const u64 swap_us = end - copied;
		const u64 total_us = end - start;

		auto& st = s_ps4_av_readbacks;
		st.flushes.fetch_add(1, std::memory_order_relaxed);
		st.total_us.fetch_add(total_us, std::memory_order_relaxed);
		st.request_us.fetch_add(request_us, std::memory_order_relaxed);
		st.drain_us.fetch_add(drain_us, std::memory_order_relaxed);
		st.copy_us.fetch_add(copy_us, std::memory_order_relaxed);
		st.swap_us.fetch_add(swap_us, std::memory_order_relaxed);

		for (u64 max = st.max_us.load(std::memory_order_relaxed); total_us > max && !st.max_us.compare_exchange_weak(max, total_us, std::memory_order_relaxed);)
		{
		}

		const u32 number = s_ps4_av_readbacks_timed.fetch_add(1, std::memory_order_relaxed) + 1;
		const bool slow = total_us >= 50'000;
		const u32 slow_number = slow ? s_ps4_av_readbacks_slow.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
		const bool to_rpcs3_log = number <= 64 || !(number & (number - 1)) || (slow && slow_number <= 256);
		const bool to_startup_log = number <= 16 || (slow && slow_number <= 16);

		if (!to_rpcs3_log && !to_startup_log)
		{
			return;
		}

		char who[160];
		if (spu)
		{
			std::snprintf(who, sizeof(who), "SPU%u ls=0x%x", spu->lv2_id >> 24, spu->pc);
		}
		else if (rsx::ps4_is_rsx_thread())
		{
			std::snprintf(who, sizeof(who), "RSX");
		}
		else if (const cpu_thread* cpu = cpu_thread::get_current(); cpu && cpu->get_class() == thread_class::ppu)
		{
			std::snprintf(who, sizeof(who), "PPU '%s' cia=0x%x", cpu->get_name().c_str(), cpu->get_pc());
		}
		else
		{
			std::snprintf(who, sizeof(who), "thread '%s'", thread_ctrl::get_name().c_str());
		}

		char numbers[32];
		if (rb.count > 1)
		{
			std::snprintf(numbers, sizeof(numbers), "#%u-#%u", rb.first, rb.last);
		}
		else
		{
			std::snprintf(numbers, sizeof(numbers), "#%u", rb.last);
		}

		char detail[320];
		std::snprintf(detail, sizeof(detail), "readback %s total %.1f ms (request %.1f, drain %.1f, copy %.1f, swap %.1f) %s",
			numbers, total_us / 1000., request_us / 1000., drain_us / 1000., copy_us / 1000., swap_us / 1000., who);

		if (to_rpcs3_log)
		{
			rsx_log.notice("PS4 DMA: %s", detail);
		}

		if (to_startup_log)
		{
			ps4_fatal_note("DMA: ", detail);
		}
	}
}

namespace vk
{
	ps4_vk_draw_counters_t g_ps4_vk_draw_counters;
	u64 g_ps4_interpreter_draws_total = 0;

	namespace
	{
		// RSX thread only (ps4_radv_scope_begin admits no other thread).
		std::array<u64, ps4_radv_site_count> s_ps4_radv_ticks{};
		bool s_ps4_radv_open = false; // a timed scope is open: nested scopes are not timed again
	}

	bool ps4_radv_scope_begin(u64& t0, u64& w0) noexcept
	{
		if (!rsx::ps4_is_rsx_thread() || s_ps4_radv_open)
		{
			return false;
		}

		s_ps4_radv_open = true;
		w0 = rsx::g_ps4_rsx_phase.wait_ticks.observe();
		t0 = utils::get_tsc();
		return true;
	}

	void ps4_radv_scope_end(ps4_radv_site site, u64 t0, u64 w0) noexcept
	{
		const u64 spent = utils::get_tsc() - t0;
		const u64 waited = rsx::g_ps4_rsx_phase.wait_ticks.observe() - w0;
		s_ps4_radv_ticks[site] += spent > waited ? spent - waited : 0;
		s_ps4_radv_open = false;
	}
}
#endif

u64 VKGSRender::get_cycles()
{
	return thread_ctrl::get_cycles(static_cast<named_thread<VKGSRender>&>(*this));
}

VKGSRender::VKGSRender(utils::serial* ar) noexcept : GSRender(ar)
{
#ifdef RPCS3_PS4
	ps4_startup_checkpoint("Vulkan: GSRender constructed; initialize dependencies");
	// 0.32: readback (DMA) switches from /data/rpcs3/dma.flag, read once per renderer and logged to startup.log.
	vk::ps4_load_dma_flags();

	// 0.37: the readback timing lines and the 10 s 'Vulkan backend' line start over with each renderer.
	s_ps4_av_readbacks_timed = 0;
	s_ps4_av_readbacks_slow = 0;
	vk::g_ps4_vk_draw_counters = {};
	vk::s_ps4_radv_ticks = {};

	// 0.38: a stop request of the previous game's renderer does not carry over.
	utils::ps4::vram::reset_stop_request();

	// 0.39: a GPU hang confirmed for 10 s stops the game (vkutils/sync.cpp ps4_request_gpu_hang_stop). Feature
	// hang_stop: off with no-hang-stop.flag or from safe-mode level 2.
	utils::ps4::gpu::set_hang_stop(utils::ps4::safe_mode::enabled(utils::ps4::safe_mode::feature::hang_stop));
	ps4_startup_checkpoint(utils::ps4::gpu::hang_stop()
		? "GPU: a hang confirmed for 10 s (a fence wait, or the oldest submission unfinished with the game in the foreground) stops the game "
		  "and returns to the XMB; the title's next boots get the gpu_hang adapt key and one safe-mode level more (no-hang-stop.flag keeps waiting)"
		: "GPU: a hang only waits and reports (hang_stop off: no-hang-stop.flag or safe-mode level 2)");

	// A device loss of this game in an earlier boot (its cb_split adapt key, decided by the boot policy): command
	// buffers are submitted in parts, so no single submission grows past what the driver accepts.
	m_ps4_cb_split_draws = utils::ps4::policy::cb_split_for_title() ? utils::ps4::rsx_programs::cb_split_draws : 0;
	if (m_ps4_cb_split_draws)
	{
		ps4_startup_checkpoint(fmt::format("PS4 RSX: command buffers split every %u draws (adapt cb_split after a device loss of this game; cb-split-off.flag turns it off)",
			m_ps4_cb_split_draws).c_str());
	}
#endif
	// Initialize dependencies
	g_fxo->need<rsx::dma_manager>();
	g_fxo->need<vk::driver_manager_thread>();

#ifdef RPCS3_PS4
	ps4_startup_checkpoint(fmt::format("Vulkan: create RADV instance (#%u in this process)", ++s_ps4_instances_created).c_str());
#endif
	if (!m_instance.create("RPCS3"))
	{
		rsx_log.fatal("Could not find a Vulkan compatible GPU driver. Your GPU(s) may not support Vulkan, or you need to install the Vulkan runtime and drivers");
		m_device = VK_NULL_HANDLE;
		return;
	}

	m_instance.bind();

#ifdef RPCS3_PS4
	ps4_startup_checkpoint("Vulkan: instance ready; enumerate GPUs");
#endif
	std::vector<vk::physical_device>& gpus = m_instance.enumerate_devices();

	// Actually confirm  that the loader found at least one compatible device
	// This should not happen unless something is wrong with the driver setup on the target system
	if (gpus.empty())
	{
		//We can't throw in Emulator::Load, so we show error and return
		rsx_log.fatal("No compatible GPU devices found");
		m_device = VK_NULL_HANDLE;
		return;
	}

#ifdef RPCS3_PS4
	ps4_startup_checkpoint("Vulkan: GPU properties ready; create device and swapchain");
#endif
	bool gpu_found = false;
	std::string adapter_name = g_cfg.video.vk.adapter;

	display_handle_t display = m_frame->handle();

#ifdef HAVE_X11
	std::visit([this](auto&& p) {
		using T = std::decay_t<decltype(p)>;
		if constexpr (std::is_same_v<T, std::pair<Display*, Window>>)
		{
			m_display_handle = p.first; XFlush(m_display_handle);
		}
	}, display);
#endif

	for (auto &gpu : gpus)
	{
		if (gpu.get_name() == adapter_name)
		{
			m_swapchain.reset(m_instance.create_swapchain(display, gpu));
			gpu_found = true;
			break;
		}
	}

	if (!gpu_found || adapter_name.empty())
	{
		m_swapchain.reset(m_instance.create_swapchain(display, gpus[0]));
	}

	if (!m_swapchain)
	{
		m_device = VK_NULL_HANDLE;
		rsx_log.fatal("Could not successfully initialize a swapchain");
		return;
	}

	m_device = const_cast<vk::render_device*>(&m_swapchain->get_device());
	vk::set_current_renderer(m_swapchain->get_device());
	vk::init();

	m_swapchain_dims.width = m_frame->client_width();
	m_swapchain_dims.height = m_frame->client_height();

	if (!m_swapchain->init(m_swapchain_dims.width, m_swapchain_dims.height))
	{
		swapchain_unavailable = true;
	}

	// create command buffer...
	m_command_buffer_pool.create((*m_device), m_device->get_graphics_queue_family());
	m_primary_cb_list.create(m_command_buffer_pool, vk::command_buffer::access_type_hint::flush_only);
	m_current_command_buffer = m_primary_cb_list.get();
	m_current_command_buffer->begin();

	// Create secondary command_buffer for parallel operations
	m_secondary_command_buffer_pool.create((*m_device), m_device->get_graphics_queue_family());
	m_secondary_cb_list.create(m_secondary_command_buffer_pool, vk::command_buffer::access_type_hint::all);

	//Occlusion
	m_occlusion_query_manager = std::make_unique<vk::query_pool_manager>(*m_device, VK_QUERY_TYPE_OCCLUSION, OCCLUSION_MAX_POOL_SIZE);
	m_occlusion_map.resize(rsx::reports::occlusion_query_count);

	for (u32 n = 0; n < rsx::reports::occlusion_query_count; ++n)
		m_occlusion_query_data[n].driver_handle = n;

	if (g_cfg.video.precise_zpass_count)
	{
		m_occlusion_query_manager->set_control_flags(VK_QUERY_CONTROL_PRECISE_BIT, 0);
	}

	// VRAM allocation
#ifdef RPCS3_PS4
	// 0.38: the fixed rings took 452 of the 640 MiB window budget in 0.37 (70% load before the first texture).
	// Since 0.36 a ring within its guard reclaims space before a draw instead of growing, so they shrink: attrib
	// 64 -> 32 MiB, texture upload 64 -> 48 MiB (three frames of 16 MiB streaming uploads before a reclaim waits for a
	// queued frame), the 16 MiB rings 8 MiB, the auxiliary upload heap 64 -> 16 MiB (about -180 MiB).
	// rings-full.flag restores the 0.37 sizes.
	const bool ps4_full_rings = fs::is_file("/data/rpcs3/rings-full.flag");
	vk::ps4_set_full_rings(ps4_full_rings);
	vk::ps4_set_aux_upload_heap_record(0);
	const usz ps4_attrib_ring = (ps4_full_rings ? VK_ATTRIB_RING_BUFFER_SIZE_M : VK_PS4_ATTRIB_RING_BUFFER_SIZE_M) * 0x100000;
	const usz ps4_texture_upload_ring = (ps4_full_rings ? VK_TEXTURE_UPLOAD_RING_BUFFER_SIZE_M : VK_PS4_TEXTURE_UPLOAD_RING_BUFFER_SIZE_M) * 0x100000;
	const usz ps4_ubo_ring = (ps4_full_rings ? VK_UBO_RING_BUFFER_SIZE_M : VK_PS4_UBO_RING_BUFFER_SIZE_M) * 0x100000;
	const usz ps4_transform_ring = (ps4_full_rings ? VK_TRANSFORM_CONSTANTS_BUFFER_SIZE_M : VK_PS4_TRANSFORM_CONSTANTS_BUFFER_SIZE_M) * 0x100000;
	const usz ps4_index_ring = (ps4_full_rings ? VK_INDEX_RING_BUFFER_SIZE_M : VK_PS4_INDEX_RING_BUFFER_SIZE_M) * 0x100000;
	// 0.38: a texture upload larger than the free part of its ring gets its own staging buffer (vk::upload_image)
	// instead of growing the ring; upload-staging-off.flag leaves those to the ring.
	m_ps4_upload_staging = !fs::is_file("/data/rpcs3/upload-staging-off.flag");
	ps4_startup_checkpoint(fmt::format("PS4 RSX: rings attrib %u, texture upload %u, uniform %u, transform/instancing %u, index %u MiB, auxiliary upload %u MiB%s; "
		"uploads larger than a ring's free part %s",
		ps4_attrib_ring >> 20, ps4_texture_upload_ring >> 20, ps4_ubo_ring >> 20, ps4_transform_ring >> 20, ps4_index_ring >> 20,
		vk::ps4_aux_upload_heap_size() >> 20, ps4_full_rings ? " (rings-full.flag: 0.37 sizes)" : " (rings-full.flag restores the 0.37 sizes)",
		m_ps4_upload_staging ? "use their own staging buffer (upload-staging-off.flag: the ring grows)" : "grow the ring (upload-staging-off.flag)").c_str());

	// 0.39: adaptive ring growth (ps4_check_ring_heaps): a ring that grew in an earlier session of this title starts at
	// that size (adapt record "<title>.rings"), never above its maximum: the 0.37 size, 32 MiB for the transform
	// constants and instancing rings. Feature ring_growth (rings-grow.flag or features.txt); off by default, as 0.38:
	// boot sizes for the whole session and the record neither read nor written.
	m_ps4_ring_growth = utils::ps4::safe_mode::enabled(utils::ps4::safe_mode::feature::ring_growth);
	m_ps4_ring_title = Emu.GetTitleID();
	const std::string ps4_ring_record = m_ps4_ring_growth && !m_ps4_ring_title.empty()
		? utils::ps4::adapt::read_record(m_ps4_ring_title, utils::ps4::adapt::rings_suffix) : std::string();
	std::string ps4_rings_from_record;
	const auto ps4_ring_size = [&](const char* name, usz base, usz max_mib) -> usz
	{
		const usz recorded = usz{utils::ps4::vram::ring_record_mib(ps4_ring_record, name)} * 0x100000;
		const usz size = std::clamp<usz>(recorded, base, std::max<usz>(base, max_mib * 0x100000));
		if (size != base)
		{
			fmt::append(ps4_rings_from_record, " '%s' %u MiB;", name, static_cast<u32>(size >> 20));
		}
		return size;
	};

	// The auxiliary upload heap is created on first use (the first texture upload of this renderer) at this size.
	vk::ps4_set_aux_upload_heap_record(ps4_ring_size("auxilliary upload heap", vk::ps4_aux_upload_heap_size(), 64));
	m_ps4_aux_staging_seen = vk::ps4_aux_staging_uploads();

	// This first set is bound persistently, so grow notifications are enabled.
	m_attrib_ring_info.create(VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, ps4_ring_size("attrib buffer", ps4_attrib_ring, 64), vk::heap_pool_default, "attrib buffer", 0x400000, VK_TRUE);
	m_fragment_env_ring_info.create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ps4_ring_size("fragment env buffer", ps4_ubo_ring, 16), vk::heap_pool_low_latency, "fragment env buffer", 0x10000, VK_TRUE);
	m_vertex_env_ring_info.create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ps4_ring_size("vertex env buffer", ps4_ubo_ring, 16), vk::heap_pool_default, "vertex env buffer", 0x10000, VK_TRUE);
	m_fragment_texture_params_ring_info.create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ps4_ring_size("fragment texture params buffer", ps4_ubo_ring, 16), vk::heap_pool_low_latency, "fragment texture params buffer", 0x10000, VK_TRUE);
	m_vertex_layout_ring_info.create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ps4_ring_size("vertex layout buffer", ps4_ubo_ring, 16), vk::heap_pool_low_latency, "vertex layout buffer", 0x10000, VK_TRUE);
	m_fragment_constants_ring_info.create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ps4_ring_size("fragment constants buffer", ps4_ubo_ring, 16), vk::heap_pool_low_latency, "fragment constants buffer", 0x10000, VK_TRUE);
	m_transform_constants_ring_info.create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ps4_ring_size("transform constants buffer", ps4_transform_ring, 32), vk::heap_pool_default, "transform constants buffer", 0x10000, VK_TRUE);
	m_raster_env_ring_info.create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ps4_ring_size("raster env buffer", ps4_ubo_ring, 16), vk::heap_pool_low_latency, "raster env buffer", 0x10000, VK_TRUE);
	// Below here, we do not bind these persistently. Each draw call specifies the range manually so we do not need heap_grow notifications.
	m_instancing_buffer_ring_info.create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ps4_ring_size("instancing data buffer", ps4_transform_ring, 32), vk::heap_pool_default, "instancing data buffer");
	m_index_buffer_ring_info.create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, ps4_ring_size("index buffer", ps4_index_ring, 16), vk::heap_pool_default, "index buffer");
	m_texture_upload_buffer_ring_info.create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, ps4_ring_size("texture upload buffer", ps4_texture_upload_ring, 64), vk::heap_pool_default, "texture upload buffer", 32 * 0x100000);
#else
	// This first set is bound persistently, so grow notifications are enabled.
	m_attrib_ring_info.create(VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, VK_ATTRIB_RING_BUFFER_SIZE_M * 0x100000, vk::heap_pool_default, "attrib buffer", 0x400000, VK_TRUE);
	m_fragment_env_ring_info.create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_UBO_RING_BUFFER_SIZE_M * 0x100000, vk::heap_pool_low_latency, "fragment env buffer", 0x10000, VK_TRUE);
	m_vertex_env_ring_info.create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_UBO_RING_BUFFER_SIZE_M * 0x100000, vk::heap_pool_default, "vertex env buffer", 0x10000, VK_TRUE);
	m_fragment_texture_params_ring_info.create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_UBO_RING_BUFFER_SIZE_M * 0x100000, vk::heap_pool_low_latency, "fragment texture params buffer", 0x10000, VK_TRUE);
	m_vertex_layout_ring_info.create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_UBO_RING_BUFFER_SIZE_M * 0x100000, vk::heap_pool_low_latency, "vertex layout buffer", 0x10000, VK_TRUE);
	m_fragment_constants_ring_info.create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_UBO_RING_BUFFER_SIZE_M * 0x100000, vk::heap_pool_low_latency, "fragment constants buffer", 0x10000, VK_TRUE);
	m_transform_constants_ring_info.create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_TRANSFORM_CONSTANTS_BUFFER_SIZE_M * 0x100000, vk::heap_pool_default, "transform constants buffer", 0x10000, VK_TRUE);
	m_raster_env_ring_info.create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_UBO_RING_BUFFER_SIZE_M * 0x100000, vk::heap_pool_low_latency, "raster env buffer", 0x10000, VK_TRUE);
	// Below here, we do not bind these persistently. Each draw call specifies the range manually so we do not need heap_grow notifications.
	m_instancing_buffer_ring_info.create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_TRANSFORM_CONSTANTS_BUFFER_SIZE_M * 0x100000, vk::heap_pool_default, "instancing data buffer");
	m_index_buffer_ring_info.create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_INDEX_RING_BUFFER_SIZE_M * 0x100000, vk::heap_pool_default, "index buffer");
	m_texture_upload_buffer_ring_info.create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_TEXTURE_UPLOAD_RING_BUFFER_SIZE_M * 0x100000, vk::heap_pool_default, "texture upload buffer", 32 * 0x100000);
#endif

	vk::data_heap_manager::register_ring_buffers
	({
		std::ref(m_attrib_ring_info),
		std::ref(m_fragment_env_ring_info),
		std::ref(m_vertex_env_ring_info),
		std::ref(m_fragment_texture_params_ring_info),
		std::ref(m_vertex_layout_ring_info),
		std::ref(m_fragment_constants_ring_info),
		std::ref(m_transform_constants_ring_info),
		std::ref(m_index_buffer_ring_info),
		std::ref(m_texture_upload_buffer_ring_info),
		std::ref(m_raster_env_ring_info),
		std::ref(m_instancing_buffer_ring_info)
	});

	m_flushable_data_heaps = vk::data_heap_manager::to_list().filter(FN(x->has_shadow()));

	const auto shadermode = g_cfg.video.shadermode.get();

	if (shadermode == shader_mode::async_with_interpreter || shadermode == shader_mode::interpreter_only)
	{
#ifdef RPCS3_PS4
		// 0.37: the interpreter is on by default now. Two 64 MiB blocks would add 128 MiB of fixed rings to the one GPU
		// window, where Wolverine already trims textures continuously in 0.36. They get the size of the other per-draw
		// rings instead; a block within its guard is reclaimed before a draw like those (ps4_check_ring_heaps).
		// 0.38: that size is 8 MiB now (16 with rings-full.flag).
		const usz ps4_instruction_ring_size = ps4_ubo_ring;
		m_vertex_instructions_buffer.create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ps4_ring_size("vertex instructions buffer", ps4_instruction_ring_size, 16), vk::heap_pool_low_latency, "vertex instructions buffer", 512 * 16);
		m_fragment_instructions_buffer.create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ps4_ring_size("fragment instructions buffer", ps4_instruction_ring_size, 16), vk::heap_pool_low_latency, "fragment instructions buffer", 2048);
#else
		m_vertex_instructions_buffer.create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 64 * 0x100000, vk::heap_pool_low_latency, "vertex instructions buffer", 512 * 16);
		m_fragment_instructions_buffer.create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 64 * 0x100000, vk::heap_pool_low_latency, "fragment instructions buffer", 2048);
#endif

		vk::data_heap_manager::register_ring_buffers
		({
			std::ref(m_vertex_instructions_buffer),
			std::ref(m_fragment_instructions_buffer)
		});
	}

#ifdef RPCS3_PS4
	// 0.36: the 0.35 Wolverine fatal was the attrib ring growing 64 -> 128 MiB after 23 min, when the GPU window had no
	// free 128 MiB range left. Rings now reclaim space before a draw instead (VKDraw.cpp), and grow() asks this
	// renderer for room before it falls back to growing (vkutils/data_heap.cpp).
	m_ps4_rings =
	{
		&m_attrib_ring_info, &m_fragment_env_ring_info, &m_vertex_env_ring_info, &m_fragment_texture_params_ring_info,
		&m_vertex_layout_ring_info, &m_fragment_constants_ring_info, &m_transform_constants_ring_info, &m_index_buffer_ring_info,
		&m_texture_upload_buffer_ring_info, &m_raster_env_ring_info, &m_instancing_buffer_ring_info,
		m_vertex_instructions_buffer.heap ? &m_vertex_instructions_buffer : nullptr,
		m_fragment_instructions_buffer.heap ? &m_fragment_instructions_buffer : nullptr
	};

	m_ps4_ring_reclaim = !fs::is_file("/data/rpcs3/ring-reclaim-off.flag");
	rsx::data_heap::s_ps4_make_room = m_ps4_ring_reclaim ? &ps4_ring_make_room_hook : nullptr;
	ps4_startup_checkpoint(m_ps4_ring_reclaim
		? "PS4 RSX: rings reclaim space before a draw (queued frames, then a hard sync) instead of growing; growth only for an allocation larger than the guard, 16 MiB steps up to 2x (ring-reclaim-off.flag restores 0.35 growth)"
		: "PS4 RSX: ring reclaim off (ring-reclaim-off.flag); rings grow as in 0.35");

	// Largest size of each managed ring (m_ps4_rings order) for the adaptive growth.
	m_ps4_ring_max_mib = { 64, 16, 16, 16, 16, 16, 32, 16, 64, 16, 32, 16, 16 };
	ps4_startup_checkpoint(m_ps4_ring_growth
		? fmt::format("PS4 RSX: adaptive ring growth on: a ring doubles after a ring hard sync or more than %u reclaims/s for %u s, up to the 0.37 size "
			"(transform constants and instancing 32 MiB), when the window's largest free range holds it + 16 MiB and the load is below 85%%; "
			"sizes kept per title (ring_growth on: rings-grow.flag or features.txt); the auxiliary upload heap doubles up to 64 MiB after %u s of more than %u uploads per "
			"second that did not fit it; from the title record:%s",
			utils::ps4::vram::ring_busy_reclaims, utils::ps4::vram::ring_busy_seconds,
			utils::ps4::vram::ring_busy_seconds, utils::ps4::vram::ring_busy_reclaims,
			ps4_rings_from_record.empty() ? " none" : ps4_rings_from_record.c_str()).c_str()
		: "PS4 RSX: adaptive ring growth off (0.38 default; rings-grow.flag turns it on); boot sizes for the whole session");
#endif

	// Initialize optional allocation information with placeholders
	m_vertex_env_buffer_info = { *m_vertex_env_ring_info.heap, 0, VK_WHOLE_SIZE };
	m_vertex_constants_buffer_info = { *m_transform_constants_ring_info.heap, 0, VK_WHOLE_SIZE };
	m_fragment_env_buffer_info = { *m_fragment_env_ring_info.heap, 0, VK_WHOLE_SIZE };
	m_fragment_texture_params_buffer_info = { *m_fragment_texture_params_ring_info.heap, 0, VK_WHOLE_SIZE };
	m_raster_env_buffer_info = { *m_raster_env_ring_info.heap, 0, VK_WHOLE_SIZE };
	m_vertex_layout_stream_info = { *m_vertex_layout_ring_info.heap, 0, VK_WHOLE_SIZE };
	m_fragment_constants_buffer_info = { *m_fragment_constants_ring_info.heap, 0, VK_WHOLE_SIZE };

	const auto& limits = m_device->gpu().get_limits();
#ifdef RPCS3_PS4
	// 0.38: a texel buffer view never spans more than the (smaller) attrib ring.
	m_texbuffer_view_size = std::min(limits.maxTexelBufferElements, static_cast<u32>(ps4_attrib_ring));
#else
	m_texbuffer_view_size = std::min(limits.maxTexelBufferElements, VK_ATTRIB_RING_BUFFER_SIZE_M * 0x100000u);
#endif

	// Initialize bulk allocators
	m_vertex_env_allocator = std::make_unique<rsx::data_heap::bulk_allocator<256, 96>>(
		m_vertex_env_ring_info,
		std::min<u32>(limits.maxUniformBufferRange / 96u, 1024u));

	m_transform_constants_allocator = std::make_unique<rsx::data_heap::bulk_allocator<256, 16>>(
		m_transform_constants_ring_info,
		std::min<u32>(limits.maxUniformBufferRange / 16u, 8192u)
	);

	m_fragment_constants_allocator = std::make_unique<rsx::data_heap::bulk_allocator<256, 16>>(
		m_fragment_constants_ring_info,
		std::min<u32>(limits.maxUniformBufferRange / 16u, 8192u)
	);

	if (m_texbuffer_view_size < 0x800000)
	{
		// Warn, only possibly expected on macOS
		rsx_log.warning("Current driver may crash due to memory limitations (%uk)", m_texbuffer_view_size / 1024);
	}

	m_max_async_frames = m_swapchain->get_swap_image_count();
	m_frame_context_storage.resize(m_max_async_frames);
	m_current_frame = &m_frame_context_storage[0];

	for (auto& ctx : m_frame_context_storage)
	{
		ctx.init(*m_device);
	}

	const auto& memory_map = m_device->get_memory_mapping();
	null_buffer = std::make_unique<vk::buffer>(*m_device, 32, memory_map.device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, 0, VMM_ALLOCATION_POOL_UNDEFINED);
	null_buffer_view = std::make_unique<vk::buffer_view>(*m_device, null_buffer->value, VK_FORMAT_R8_UINT, 0, 32);

#ifdef RPCS3_PS4
	// 0.38: 32 MiB held in the GPU window for the last step of a failed required allocation (vkutils/memory.cpp).
	vk::ps4_vram_reserve_arm(*m_device, memory_map.device_local.first());
#endif

	// Initialize the shader compiler stack
	const int preferred_compiler_threads = g_cfg.video.shader_compiler_threads_count == 0
		? utils::get_thread_count()                   // We spawn the initial pipe compiler with all threads during the boot sequence by default.
		: g_cfg.video.shader_compiler_threads_count;  // Respect user override if set

	spirv::initialize_compiler_context();
	vk::initialize_pipe_compiler(preferred_compiler_threads);

	m_prog_buffer = std::make_unique<vk::program_cache>
	(
		[this](const vk::pipeline_props& props, const RSXVertexProgram& vp, const RSXFragmentProgram& fp)
		{
			// Program was linked or queued for linking
			m_shaders_cache->store(props, vp, fp);
		}
	);

	if (g_cfg.video.disable_vertex_cache)
		m_vertex_cache = std::make_unique<vk::null_vertex_cache>();
	else
		m_vertex_cache = std::make_unique<vk::weak_vertex_cache>();

	m_shaders_cache = std::make_unique<vk::shader_cache>(*m_prog_buffer, "vulkan", "v1.95");

	for (u32 i = 0; i < m_swapchain->get_swap_image_count(); ++i)
	{
		const auto target_layout = m_swapchain->get_optimal_present_layout();
		const auto target_image = m_swapchain->get_image(i);
		VkClearColorValue clear_color{};
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		vk::change_image_layout(*m_current_command_buffer, target_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, range);
		vkCmdClearColorImage(*m_current_command_buffer, target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &range);
		vk::change_image_layout(*m_current_command_buffer, target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target_layout, range);

	}

	m_texture_cache.initialize((*m_device), m_device->get_graphics_queue(),
			m_texture_upload_buffer_ring_info);

	vk::get_overlay_pass<vk::ui_overlay_renderer>()->init(*m_current_command_buffer, m_texture_upload_buffer_ring_info);

	if (shadermode == shader_mode::async_with_interpreter || shadermode == shader_mode::interpreter_only)
	{
		m_shader_interpreter.init(*m_device);
	}

	backend_config.supports_multidraw = true;
	backend_config.supports_hw_instanced_rendering = true;
	backend_config.supports_programmable_blending = true;

	backend_config.supports_last_provoking_vertex = m_device->get_provoking_vertex_last_support();
	if (!backend_config.supports_last_provoking_vertex)
	{
		rsx_log.warning("VK_EXT_provoking_vertex with provokingVertexLast is unavailable; RSX flat shading will fall back to smooth interpolation.");
	}

	// NVIDIA has broken attribute interpolation
	backend_config.supports_normalized_barycentrics = (
		!vk::is_NVIDIA(vk::get_driver_vendor()) ||
		!m_device->get_barycoords_support() ||
		g_cfg.video.shader_precision == gpu_preset_level::low);

	// NOTE: We do not actually need multiple sample support for A2C to work
	// This is here for visual consistency - will be removed when AA problems due to mipmaps are fixed
	if (g_cfg.video.antialiasing_level != msaa_level::none)
	{
		backend_config.supports_hw_msaa = true;
		backend_config.supports_hw_a2c = true;
		backend_config.supports_hw_a2c_1spp = true;
		backend_config.supports_hw_a2one = m_device->get_alpha_to_one_support();
	}

	// NOTE: On NVIDIA cards going back decades (including the PS3) there is a slight normalization inaccuracy in compressed formats.
	// Confirmed in BLES01916 (The Evil Within) which uses RGB565 for some virtual texturing data.
	backend_config.supports_hw_renormalization = vk::is_NVIDIA(vk::get_driver_vendor());

	// Conditional rendering support
	// Do not use on MVK due to a speedhack we rely on (streaming results without stopping the current renderpass)
	// If we break the renderpasses, MVK loses around 75% of its performance in troublesome spots compared to just doing a CPU sync
	backend_config.supports_hw_conditional_render = (vk::get_driver_vendor() != vk::driver_vendor::MVK);

	// Passthrough DMA
	backend_config.supports_passthrough_dma = m_device->get_external_memory_host_support();

	// Host sync
	backend_config.supports_host_gpu_labels = !!g_cfg.video.host_label_synchronization;

	// Async compute and related operations
	if (g_cfg.video.vk.asynchronous_texture_streaming)
	{
		// Optimistic, enable async compute
		backend_config.supports_asynchronous_compute = true;

		if (m_device->get_graphics_queue() == m_device->get_transfer_queue())
		{
			rsx_log.error("Cannot run graphics and async transfer in the same queue. Async uploads are disabled. This is a limitation of your GPU");
			backend_config.supports_asynchronous_compute = false;
		}
	}

	// Sanity checks
	switch (vk::get_driver_vendor())
	{
	case vk::driver_vendor::NVIDIA:
		if (backend_config.supports_asynchronous_compute)
		{
			if (auto chip_family = vk::get_chip_family();
				chip_family == vk::chip_class::NV_kepler || chip_family == vk::chip_class::NV_maxwell)
			{
				rsx_log.warning("Older NVIDIA cards do not meet requirements for true asynchronous compute due to some driver fakery.");
			}

			rsx_log.notice("Forcing safe async compute for NVIDIA device to avoid crashing.");
			g_cfg.video.vk.asynchronous_scheduler.set(vk_gpu_scheduler_mode::safe);
		}
		break;
	case vk::driver_vendor::NVK:
		// TODO: Verify if this driver crashes or not
		rsx_log.warning("NVK behavior with passthrough DMA is unknown. Proceed with caution.");
		break;
#if !defined(_WIN32)
		// Anything running on AMDGPU kernel driver will not work due to the check for fd-backed memory allocations
	case vk::driver_vendor::RADV:
	case vk::driver_vendor::AMD:
#if !defined(__linux__)
		// Intel chipsets would fail on BSD in most cases and DRM_IOCTL_i915_GEM_USERPTR unimplemented
	case vk::driver_vendor::ANV:
#endif
		if (backend_config.supports_passthrough_dma)
		{
			rsx_log.error("AMDGPU kernel driver on Linux and INTEL driver on some platforms cannot support passthrough DMA buffers.");
			backend_config.supports_passthrough_dma = false;
		}
		break;
#endif
	case vk::driver_vendor::INTEL:
		// As expected host allocations won't work on INTEL despite the extension being present
		if (backend_config.supports_passthrough_dma)
		{
			rsx_log.error("INTEL driver does not support passthrough DMA buffers");
			backend_config.supports_passthrough_dma = false;
		}
		break;
	default: break;
	}

	if (backend_config.supports_asynchronous_compute)
	{
		m_async_compute_memory_barrier =
		{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR,
			.pNext = nullptr,
			.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR,
			.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT_KHR,
			.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT_KHR,
			.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT_KHR
		};

		m_async_compute_dependency_info =
		{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &m_async_compute_memory_barrier
		};

		// Run only if async compute can be used.
		g_fxo->init<vk::AsyncTaskScheduler>(g_cfg.video.vk.asynchronous_scheduler, m_async_compute_dependency_info);
	}

	if (backend_config.supports_host_gpu_labels)
	{
		if (backend_config.supports_passthrough_dma)
		{
			m_host_object_data = std::make_unique<vk::buffer>(*m_device,
				0x10000,
				memory_map.host_visible_coherent, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
				VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0,
				VMM_ALLOCATION_POOL_SYSTEM);

			m_host_dma_ctrl = std::make_unique<rsx::RSXDMAWriter>(m_host_object_data->map(0, 0x10000));
		}
		else
		{
			rsx_log.error("Your GPU/driver does not support extensions required to enable passthrough DMA emulation. Host GPU labels will be disabled.");
			backend_config.supports_host_gpu_labels = false;
		}
	}

	if (!backend_config.supports_host_gpu_labels &&
		!backend_config.supports_asynchronous_compute)
	{
		// Disable passthrough DMA unless we enable a feature that requires it.
		// I'm avoiding an explicit checkbox for this until I figure out why host labels don't fix all problems with passthrough.
		backend_config.supports_passthrough_dma = false;
	}
}

VKGSRender::~VKGSRender()
{
#ifdef RPCS3_PS4
	// 0.31: the video memory snapshot describes this renderer; drop it so the health report in the XMB does not
	// keep printing a stale one.
	utils::ps4::gpu::publish({});

	// 0.36: the rings are destroyed below; growth falls back to upstream behaviour until a renderer sets it again.
	rsx::data_heap::s_ps4_make_room = nullptr;
	utils::ps4::gpu::note_unfinished_submission(0);
#endif

	// Force-release ZCULL-ctrl since it is a pointer to self.
	zcull_ctrl.release();

	if (m_device == VK_NULL_HANDLE)
	{
		//Initialization failed
		return;
	}

	// Flush DMA queue
	while (!g_fxo->get<rsx::dma_manager>().sync())
	{
		do_local_task(rsx::FIFO::state::lock_wait);
	}

	//Wait for device to finish up with resources
	vkDeviceWaitIdle(*m_device);

	// Globals. TODO: Refactor lifetime management
	if (auto async_scheduler = g_fxo->try_get<vk::AsyncTaskScheduler>())
	{
		async_scheduler->destroy();
	}

	// GC cleanup
	vk::get_resource_manager()->flush();

	// Host data
	if (m_host_object_data)
	{
		m_host_object_data->unmap();
		m_host_object_data.reset();
	}

	// Clear flush requests
	m_flush_requests.clear_pending_flag();

	// Shaders
	vk::destroy_pipe_compiler();        // Ensure no pending shaders being compiled
	spirv::finalize_compiler_context(); // Shut down the glslang compiler
	m_prog_buffer->clear();             // Delete shader objects
	m_shader_interpreter.destroy();

	m_persistent_attribute_storage.reset();
	m_volatile_attribute_storage.reset();

	// Upscaler (references some global resources)
	m_upscaler.reset();

	// Heaps
	vk::data_heap_manager::reset();

	// Fallback bindables
	null_buffer.reset();
	null_buffer_view.reset();

	if (m_current_frame == &m_aux_frame_context)
	{
		// Return resources back to the owner
		m_current_frame = &m_frame_context_storage[m_current_queue_index];
		m_current_frame->grab_resources(m_aux_frame_context);
	}

	// CPU frame contexts
	for (auto &ctx : m_frame_context_storage)
	{
		ctx.destroy(*m_device);
	}
	m_current_frame = nullptr;
	m_frame_context_storage.clear();

	// Textures
#ifdef RPCS3_PS4
	// Texture cache first: unlocking a framebuffer section calls on_unlock() on its render target, which the surface
	// cache frees in destroy(). Unlock only drops a reference; the surface cache then frees the targets.
	m_texture_cache.destroy();
	m_rtts.destroy();
#else
	m_rtts.destroy();
	m_texture_cache.destroy();
#endif

	m_overlay_recording_img.reset();
	m_stencil_mirror_sampler.reset();

	// Queries
	m_occlusion_query_manager.reset();
	m_cond_render_buffer.reset();

	// Command buffer
	m_primary_cb_list.destroy();
	m_secondary_cb_list.destroy();

	m_command_buffer_pool.destroy();
	m_secondary_command_buffer_pool.destroy();

	// Descriptors
	vk::descriptors::flush();

#ifdef RPCS3_PS4
	// 0.38: the emergency reserve goes with the device.
	vk::ps4_vram_reserve_disarm();
#endif

	// Global resources
	vk::destroy_global_resources();

	// Device handles/contexts
	m_swapchain->destroy();
	m_instance.destroy();

#if defined(HAVE_X11) && defined(HAVE_VULKAN)
	if (m_display_handle)
		XCloseDisplay(m_display_handle);
#endif
}

#ifdef RPCS3_PS4
namespace vk
{
	thread_local bool g_ps4_rsx_parked_for_flush = false;
}
#endif

#ifdef RPCS3_PS4
// 0.39: only the RSX thread clears a flush request (do_local_task, hard syncs). One that left through
// thread_ctrl::emergency_exit (thread fatal) never runs again, and ~VKGSRender clears the request only after the join
// that waits for the faulting thread itself, so an unbounded producer_wait() keeps the stop from finishing.
bool VKGSRender::ps4_wait_flush_served(u32 address)
{
	u32 spins = 0;
	u64 warn_at_us = 0;

	while (m_flush_requests.pending())
	{
		// The upstream spin first: a living RSX thread usually takes the request within it
		if (spins < 1024)
		{
			spins++;
			utils::pause();
			continue;
		}

		// errored: emergency_exit (never overwritten by the stop's 'aborting'); finished: the thread returned
		if (const thread_state state = static_cast<thread_state>(static_cast<named_thread<VKGSRender>&>(*this)); state >= thread_state::errored)
		{
			if (const u32 n = ++m_ps4_av_dead_rsx; n <= 4)
			{
				const std::string line = fmt::format("PS4 RSX: a CPU fault at 0x%x waited for the RSX thread, which %s; not served, left to the "
					"access-violation net so the stop can finish (thread '%s', #%u)", address,
					state == thread_state::errored ? "ended in a fatal error" : "has returned", thread_ctrl::get_name(), n);
				rsx_log.error("%s", line);
				ps4_startup_checkpoint(line.c_str());
			}

			return false;
		}

		// A living RSX thread that does not take the request: one line per wait, no behaviour change
		const u64 now = get_system_time();
		if (!warn_at_us)
		{
			warn_at_us = now + 2'000'000;
		}
		else if (now >= warn_at_us)
		{
			rsx_log.warning("PS4 RSX: '%s' has waited 2 s for the RSX thread to take a CPU fault at 0x%x", thread_ctrl::get_name(), address);
			warn_at_us = umax;
		}

		std::this_thread::yield();
	}

	return true;
}
#endif

bool VKGSRender::on_access_violation(u32 address, bool is_writing)
{
	rsx::mm_flush(address);

	vk::texture_cache::thrashed_set result;
	{
		const rsx::invalidation_cause cause = is_writing ? rsx::invalidation_cause::deferred_write : rsx::invalidation_cause::deferred_read;
		result = m_texture_cache.invalidate_address(*m_secondary_cb_list.get(), address, cause);
	}

	if (result.invalidate_samplers)
	{
		std::lock_guard lock(m_sampler_mutex);
		m_samplers_dirty.store(true);
	}

	if (!result.violation_handled)
	{
		return zcull_ctrl->on_access_violation(address);
	}

#ifdef RPCS3_PS4
	// 0.36: the texture cache took this fault, so ZCULL's handler (which flushes deferred reports before the CPU reads
	// them) did not run for it. Counted when ZCULL still protects the page.
	if (zcull_ctrl->ps4_page_locked(address))
	{
		const u32 claims = ++m_ps4_zcull_pages_claimed;
		if (!(claims & (claims - 1)))
		{
			const std::string line = fmt::format("RSX: the texture cache handled a fault at 0x%x on a page ZCULL still protects for deferred reports "
				"(zcull pages unlocked by texture cache=%u)", address, claims);
			rsx_log.warning("PS4 %s", line);

			if (claims == 1)
			{
				ps4_startup_checkpoint(line.c_str());
			}
		}
	}
#endif

	if (result.num_flushable > 0)
	{
		if (g_fxo->get<rsx::dma_manager>().is_current_thread())
		{
			// The offloader thread cannot handle flush requests
			ensure(!(m_queue_status & flush_queue_state::deadlock));

			m_offloader_fault_range = g_fxo->get<rsx::dma_manager>().get_fault_range(is_writing);
			m_offloader_fault_cause = (is_writing) ? rsx::invalidation_cause::write : rsx::invalidation_cause::read;

			g_fxo->get<rsx::dma_manager>().set_mem_fault_flag();
			m_queue_status |= flush_queue_state::deadlock;
			m_eng_interrupt_mask |= rsx::backend_interrupt;

			// Wait for deadlock to clear
			while (m_queue_status & flush_queue_state::deadlock)
			{
				utils::pause();
			}

			g_fxo->get<rsx::dma_manager>().clear_mem_fault_flag();
			return true;
		}

#ifdef RPCS3_PS4
		// 0.37: this flush is timed (request, drain, GPU copy, CPU write-back), and a faulting SPU is published for
		// the RSX report and label writers until it returns (ps4_readback_state.h).
		const u64 ps4_av_start = get_system_time();
		vk::g_ps4_readback_tls = {};
		const spu_thread* const ps4_spu = cpu_thread::get_current<spu_thread>();
		std::optional<rsx::ps4_spu_readback_scope> ps4_spu_readback;
		if (ps4_spu)
		{
			ps4_spu_readback.emplace(ps4_spu->lv2_id >> 24);
		}
#endif

		bool has_queue_ref = false;
		std::function<void()> data_transfer_completed_callback{};

		if (!is_current_thread()) [[likely]]
		{
#ifdef RPCS3_PS4
			// 0.39: a fault already found the RSX thread ended (ps4_wait_flush_served): no request nobody serves
			if (m_ps4_av_dead_rsx)
			{
				return false;
			}
#endif
			// Always submit primary cb to ensure state consistency (flush pending changes such as image transitions)
			vm::temporary_unlock();

			std::lock_guard lock(m_flush_queue_mutex);

			m_flush_requests.post(false);
			m_eng_interrupt_mask |= rsx::backend_interrupt;
			has_queue_ref = true;
		}
		else
		{
			if (vk::is_uninterruptible())
			{
				rsx_log.error("Fault in uninterruptible code!");
			}

			// Flush primary cb queue to sync pending changes (e.g image transitions!)
			flush_command_queue();
		}

		if (has_queue_ref)
		{
			// Wait for the RSX thread to process request if it hasn't already
#ifdef RPCS3_PS4
			if (!ps4_wait_flush_served(address))
			{
				// 0.39: not handled. handle_access_violation (Thread.cpp) retries the access, since the guest flags
				// allow it, and ps4_av_livelock_net makes the host page RW; this thread then reaches its stop check.
				// The data it reads is stale (the RSX thread is gone with the game).
				m_flush_requests.remove_one();
				return false;
			}

			// 0.32: this thread holds its queue reference until remove_one(); submissions from it are expected until
			// then. The RSX is normally parked meanwhile, but a hard sync can serve the request without parking
			// (m_ps4_rsx_parked_for_flush tells the two apart).
			vk::g_ps4_rsx_parked_for_flush = true;
#else
			m_flush_requests.producer_wait();
#endif

			data_transfer_completed_callback = [&]()
			{
#ifdef RPCS3_PS4
				vk::g_ps4_rsx_parked_for_flush = false;
#endif
				m_flush_requests.remove_one();
				has_queue_ref = false;
			};
		}

#ifdef RPCS3_PS4
		const u64 ps4_av_requested = get_system_time();

		// 0.37: vertex copies handed to the RSX copy worker land before the drain fence. A no-op today: the RSX syncs
		// them before it submits or parks. CPU threads only (RSX, PPU, SPU): the copy worker can fault into this path
		// itself and must not wait for its own job.
		if (cpu_thread::get_current())
		{
			g_fxo->get<rsx::dma_manager>().sync();
		}

		// 0.28: the RSX thread is parked here (or this is the RSX thread, after flush_command_queue), so
		// submitting is externally synchronized. Drain the queue through a fence before flushing: the DMA
		// vk::events these sections wait on are never seen as set by the host on the console's RADV.
		// 0.32: the drain only counts when its fence wait completed; after a 2 s timeout (GPU hang) the DMA it
		// was meant to cover is not known to be complete, so imp_flush must not treat it as covered.
		const bool ps4_drained = m_texture_cache.ps4_drain_queue(*m_secondary_cb_list.next());
		vk::g_ps4_flush_queue_drained = ps4_drained;
		if (ps4_drained)
		{
			// 0.29: exact count per 10 s ('av drains' in the 'RSX gameplay' line).
			rsx::g_ps4_rsx_counters.av_drains++;
		}
		else
		{
			rsx_log.error("PS4: access-violation drain did not complete; DMA data may be stale");
		}

		const u64 ps4_av_drained = get_system_time();
#endif
		m_texture_cache.flush_all(*m_secondary_cb_list.next(), result, data_transfer_completed_callback);
#ifdef RPCS3_PS4
		vk::g_ps4_flush_queue_drained = false;
		vk::g_ps4_rsx_parked_for_flush = false;
		ps4_note_av_readback(ps4_spu, ps4_av_start, ps4_av_requested, ps4_av_drained, get_system_time());
#endif

		if (has_queue_ref)
		{
			// Release RSX thread if it's still locked
			m_flush_requests.remove_one();
		}
	}

	return true;
}

void VKGSRender::on_invalidate_memory_range(const utils::address_range32 &range, rsx::invalidation_cause cause)
{
	std::lock_guard lock(m_secondary_cb_guard);

	auto data = m_texture_cache.invalidate_range(*m_secondary_cb_list.next(), range, cause);
	AUDIT(data.empty());

	if (cause == rsx::invalidation_cause::unmap)
	{
		if (data.violation_handled)
		{
			m_texture_cache.purge_unreleased_sections();
			{
				std::lock_guard lock(m_sampler_mutex);
				m_samplers_dirty.store(true);
			}
		}

		vk::unmap_dma(range.start, range.length());
	}
}

void VKGSRender::on_semaphore_acquire_wait()
{
	if (m_flush_requests.pending() ||
		(async_flip_requested & flip_request::emu_requested) ||
		(m_queue_status & flush_queue_state::deadlock))
	{
		do_local_task(rsx::FIFO::state::lock_wait);
	}
}

bool VKGSRender::on_vram_exhausted(rsx::problem_severity severity)
{
	ensure(!vk::is_uninterruptible() && rsx::get_current_renderer()->is_current_thread());

#ifdef RPCS3_PS4
	if (severity >= rsx::problem_severity::severe)
	{
		// 0.31: severe means less than about 64 MiB of the device-local cap is left (or an allocation already
		// failed). Counted for the health report.
		// 0.32: trims now also start from the budget for the whole GPU window (vmm_determine_memory_load_severity),
		// which is normal operation near that budget and must not shrink the next boots. The smaller video memory
		// budget (adapt vram=low) is only noted for an allocation that really failed (memory.cpp).
		const u32 trims = utils::ps4::gpu::note_pressure_trim();
		if (!(trims & (trims - 1)))
		{
			// The 1st, 2nd, 4th, 8th... trim: near the window budget this can run every frame.
			rsx_log.warning("PS4 VRAM: trim at severity %s (%u severe trim(s) this process)", severity == rsx::problem_severity::fatal ? "fatal" : "severe", trims);
		}
	}
#endif

	bool texture_cache_relieved = false;
	if (severity >= rsx::problem_severity::fatal)
	{
		// Hard sync before trying to evict anything. This guarantees no UAF crashes in the driver.
		// As a bonus, we also get a free gc pass
		flush_command_queue(true, true);
		g_fxo->get<vk::driver_manager_thread>().drain();

		if (m_texture_cache.is_overallocated())
		{
			// Evict some unused textures. Do not evict any active references
			std::set<u32> exclusion_list;
			auto scan_array = [&](const auto& texture_array)
			{
				for (auto i = 0ull; i < texture_array.size(); ++i)
				{
					const auto& tex = texture_array[i];
					const auto addr = rsx::get_address(tex.offset(), tex.location());
					exclusion_list.insert(addr);
				}
			};

			scan_array(rsx::method_registers.fragment_textures);
			scan_array(rsx::method_registers.vertex_textures);

			// Hold the secondary lock guard to prevent threads from trying to touch access violation handler stuff
			std::lock_guard lock(m_secondary_cb_guard);

			rsx_log.warning("Texture cache is overallocated. Will evict unnecessary textures.");
			texture_cache_relieved = m_texture_cache.evict_unused(exclusion_list);
		}
	}

	texture_cache_relieved |= m_texture_cache.handle_memory_pressure(severity);

#ifdef RPCS3_PS4
	// 0.38: below fatal, only unreleased and unlocked sections went so far; idle-but-locked textures stayed even
	// with the window full. A severe trim now also evicts textures not looked up for 300 frames.
	if (severity >= rsx::problem_severity::severe)
	{
		texture_cache_relieved |= ps4_evict_textures_by_age() > 0;
	}
#endif

	if (severity == rsx::problem_severity::low)
	{
		// Low severity only handles invalidating unused textures
		return texture_cache_relieved;
	}

	bool surface_cache_relieved = false;

#ifdef RPCS3_PS4
	// 0.32: device-local and host-visible memory share one orbis-drm GPU window here. A spill allocates the host copy
	// in that same window before the device copy is released, so it frees nothing when memory is short (like an IGP).
	constexpr bool can_spill = false;
#else
	const auto mem_info = m_device->get_memory_mapping();
	const bool can_spill = mem_info.device_local != mem_info.host_visible_coherent; // Do not spill if it is an IGP, there is nowhere to spill to
#endif

	// Check if we need to spill
	if (severity >= rsx::problem_severity::fatal &&                // Only spill for fatal errors
		can_spill &&
		m_rtts.is_overallocated())                                 // Surface cache must be over-allocated by the design quota
	{
		// Queue a VRAM spill operation.
		m_rtts.spill_unused_memory();
	}

	// Moderate severity and higher also starts removing stale render target objects
	if (m_rtts.handle_memory_pressure(*m_current_command_buffer, severity))
	{
		surface_cache_relieved = true;
		m_rtts.trim(*m_current_command_buffer, severity);
	}

	const bool any_cache_relieved = (texture_cache_relieved || surface_cache_relieved);
	if (severity < rsx::problem_severity::fatal)
	{
		return any_cache_relieved;
	}

	if (surface_cache_relieved && !m_samplers_dirty)
	{
		// If surface cache was modified destructively, then we must reload samplers touching the surface cache.
		bool invalidate_samplers = false;
		auto scan_array = [&](const auto& texture_array, const auto& sampler_states)
		{
			if (invalidate_samplers)
			{
				return;
			}

			for (auto i = 0ull; i < texture_array.size(); ++i)
			{
				if (texture_array[i].enabled() &&
					sampler_states[i] &&
					sampler_states[i]->upload_context == rsx::texture_upload_context::framebuffer_storage)
				{
					invalidate_samplers = true;
					break;
				}
			}
		};

		scan_array(rsx::method_registers.fragment_textures, fs_sampler_state);
		scan_array(rsx::method_registers.vertex_textures, vs_sampler_state);

		if (invalidate_samplers)
		{
			m_samplers_dirty.store(true);
		}
	}

	// Imminent crash, full GPU sync is the least of our problems
	flush_command_queue(true, true);

	return any_cache_relieved;
}

#ifdef RPCS3_PS4
void VKGSRender::ps4_submit_frame_before_dma(vk::command_buffer& cmd)
{
	if (!is_current_thread() ||
		&cmd != static_cast<vk::command_buffer*>(m_current_command_buffer) ||
		!cmd.is_recording() ||
		(m_queue_status & flush_queue_state::flushing) ||
		!vk::ps4_dma_isolate())
	{
		return;
	}

	// The regular submit path: dirty heaps are synced, the render pass and open queries are closed, the buffer is
	// tagged for the garbage collector.
	close_and_submit_command_buffer();

	// The readback goes into this same command buffer (the texture cache holds a reference to it), so wait until the
	// GPU is done with the frame before it is recorded again. Untimed: the fence watchdog reports a hang after 10 s.
	m_current_command_buffer->wait();

	// Advances reset_id, so occlusion bookkeeping no longer treats this buffer as holding the queries just submitted.
	m_current_command_buffer->reset();

	if (m_occlusion_query_active)
	{
		// Same as flush_command_queue: the query that was closed above restarts at the next draw.
		m_current_command_buffer->flags |= vk::command_buffer::cb_load_occluson_task;
	}

	// prepare_for_dma_transfers begins the buffer again.
}

bool VKGSRender::ps4_retire_completed_work()
{
	const auto& memory_map = m_device->get_memory_mapping();
	const auto app_memory_usage = [&]()
	{
		return vk::vmm_get_application_memory_usage(memory_map.device_local) + vk::vmm_get_application_memory_usage(memory_map.host_visible_coherent);
	};

	const u64 completed_before = vk::last_completed_event_id();
	const u64 usage_before = app_memory_usage();

	if (is_current_thread())
	{
		// The buffer being recorded is not pending, so this only waits for submitted work (as in
		// on_descriptor_pool_fragmentation). May stall for up to a frame of GPU work.
		m_primary_cb_list.wait_all();
	}
	else if (vk::g_ps4_rsx_parked_for_flush && m_ps4_rsx_parked_for_flush.load())
	{
		// Off the RSX thread: only collect what already finished, and only inside an access-violation flush that
		// still holds its queue reference while the RSX thread waits in do_local_task (it cannot leave that wait
		// before this thread drops the reference). Anywhere else the RSX thread may be in the unlocked paths of
		// these buffers (submit, begin, ps4_wait_pending), and a fence reset from here can make its wait miss the
		// signal: a 2 s timeout reported as a GPU hang, or an untimed wait in begin() that never returns.
		m_primary_cb_list.poke_all();
	}

	// The secondary chain is not polled: its buffers are never tagged with an event id, so completing them frees
	// nothing, and other faulting threads wait on and reset their fences without the chunk lock.

	// Frees what the completed submissions released.
	g_fxo->get<vk::driver_manager_thread>().drain();

	return vk::last_completed_event_id() != completed_before || app_memory_usage() < usage_before;
}

bool VKGSRender::ps4_make_ring_room(rsx::data_heap& heap, usz /*size*/)
{
	// Same conditions as a fatal on_vram_exhausted (which also hard-syncs mid-draw), and only for this renderer's rings.
	if (!m_ps4_ring_reclaim || !is_current_thread() || vk::is_uninterruptible() || (m_queue_status & flush_queue_state::flushing))
	{
		return false;
	}

	if (std::none_of(m_ps4_rings.begin(), m_ps4_rings.end(), [&](vk::data_heap* ring) { return ring && static_cast<rsx::data_heap*>(ring) == &heap; }))
	{
		return false;
	}

	// Submits and waits for everything recorded so far; the drain retires every queued frame and restores its heap
	// snapshot. The command buffer is kept because the caller may hold it. get is not moved past the current frame:
	// the draw in progress may use vertex-cache hits of earlier draws in this frame, which only the frame keeps alive.
	flush_command_queue(true, true);
	return true;
}

std::set<u32> VKGSRender::ps4_bound_texture_addresses() const
{
	// The exclusion list of the fatal trim in on_vram_exhausted.
	std::set<u32> addresses;
	auto scan_array = [&](const auto& texture_array)
	{
		for (auto i = 0ull; i < texture_array.size(); ++i)
		{
			const auto& tex = texture_array[i];
			addresses.insert(rsx::get_address(tex.offset(), tex.location()));
		}
	};

	scan_array(rsx::method_registers.fragment_textures);
	scan_array(rsx::method_registers.vertex_textures);
	return addresses;
}

u64 VKGSRender::ps4_evict_textures_by_age()
{
	namespace vram = utils::ps4::vram;

	if (!m_ps4_age_evict_state)
	{
		m_ps4_age_evict_state = fs::is_file("/data/rpcs3/texture-age-evict-off.flag") ? 2 : 1;
		ps4_startup_checkpoint(m_ps4_age_evict_state == 1
			? "VRAM: severe and fatal trims with the GPU window at 90% or more (or the textures over their quota) evict textures not used for 300 frames, "
			  "least recently used first, down to 85% (texture-age-evict-off.flag disables it)"
			: "VRAM: age-based texture eviction off (texture-age-evict-off.flag)");
	}

	if (m_ps4_age_evict_state != 1)
	{
		return 0;
	}

	const u64 now = get_system_time();
	if (m_ps4_last_age_eviction_us && now - m_ps4_last_age_eviction_us < vram::evict_period_us)
	{
		return 0;
	}

	const f32 load = vk::ps4_window_load_percent();
	const bool over_quota = m_texture_cache.ps4_over_quota();
	if (load < vram::evict_load_percent && !over_quota)
	{
		return 0;
	}

	m_ps4_last_age_eviction_us = now;

	// Bytes to free so that the load falls to 85%: from the window meter, or from the allocator's blocks against
	// their budget without it.
	u64 size = 0, used = 0, hole = 0;
	if (!vk::ps4_window_meter(size, used, hole))
	{
		size = used = 0;
	}

	utils::ps4::gpu::vram_snapshot heaps{};
	vk::ps4_query_vram_heaps(heaps);
	u64 vma = 0;
	for (u32 i = 0; i < heaps.heap_count; ++i)
	{
		vma += heaps.heap_usage[i];
	}

	const u64 target = vram::eviction_target(vma, u64{ vk::ps4_window_budget_mib() } << 20, used, size, vk::ps4_vram_reserve_held());
	const u64 textures = vk::vmm_get_application_pool_usage(VMM_ALLOCATION_POOL_TEXTURE_CACHE);

	u64 evicted = 0;
	if (m_secondary_cb_guard.try_lock())
	{
		// As the fatal trim: faulting threads stay out of the access-violation paths meanwhile.
		evicted = m_texture_cache.ps4_evict_by_age(ps4_bound_texture_addresses(), vk::get_current_frame_id(), vram::evict_min_age_frames, target);
		m_secondary_cb_guard.unlock();
	}

	const u32 runs = ++m_ps4_age_evictions;
	if (!(runs & (runs - 1)))
	{
		// The 1st, 2nd, 4th... run of this renderer.
		const std::string line = fmt::format("VRAM: age-based eviction #%u: window %u%%, textures %llu MiB%s: %llu MiB of textures unused for %llu+ frames evicted (target %llu MiB)",
			runs, static_cast<u32>(load), textures >> 20, over_quota ? " (over the quota)" : "", evicted >> 20, vram::evict_min_age_frames, target >> 20);
		rsx_log.warning("PS4 %s", line);
		ps4_startup_checkpoint(line.c_str());
	}

	return evicted;
}

u64 VKGSRender::ps4_sweep_dma(bool deep)
{
	namespace vram = utils::ps4::vram;

	if (!m_ps4_dma_age_out_state)
	{
		m_ps4_dma_age_out_state = fs::is_file("/data/rpcs3/dma-age-out-off.flag") ? 2 : 1;
		ps4_startup_checkpoint(m_ps4_dma_age_out_state == 1
			? "VRAM: DMA mirror blocks unused for 30 s, or above 64 MiB the least recently used ones, are released; never one holding a readback "
			  "the CPU has not read or backing a DMA section (dma-age-out-off.flag keeps them)"
			: "VRAM: DMA mirror blocks are kept until the game stops (dma-age-out-off.flag)");
	}

	if (m_ps4_dma_age_out_state != 1)
	{
		return 0;
	}

	// Blocks that hold a readback the CPU has not flushed yet, or that back a DMA section, stay. The texture cache is
	// asked without the DMA pool lock (the cache takes that lock under its own).
	return vk::ps4_dma_release_idle(get_system_time(), deep ? vram::dma_min_age_us : vram::dma_idle_us, vram::dma_pool_cap, vram::dma_min_age_us,
		[this](const utils::address_range32& range) { return m_texture_cache.ps4_dma_range_pinned(range); });
}

bool VKGSRender::ps4_deep_reclaim(const char* why)
{
	// Same conditions as a fatal on_vram_exhausted (which also hard-syncs mid-draw).
	if (!is_current_thread() || vk::is_uninterruptible() || (m_queue_status & flush_queue_state::flushing) || m_ps4_in_deep_reclaim)
	{
		return false;
	}

	m_ps4_in_deep_reclaim = true;
	const u64 start = get_system_time();

	u64 size = 0, used_before = 0, hole_before = 0;
	const bool metered = vk::ps4_window_meter(size, used_before, hole_before, true);
	const u64 textures_before = vk::vmm_get_application_pool_usage(VMM_ALLOCATION_POOL_TEXTURE_CACHE);
	const u64 surfaces_before = vk::vmm_get_application_pool_usage(VMM_ALLOCATION_POOL_SURFACE_CACHE);

	// Everything recorded so far completes; the garbage collector frees what it held.
	flush_command_queue(true, true);
	g_fxo->get<vk::driver_manager_thread>().drain();

	// Every texture the current draw does not bind, whatever the quota (the fatal trim only evicts above it). Nothing
	// when this thread is inside the texture cache (evict_unused only try-locks it).
	if (m_secondary_cb_guard.try_lock())
	{
		m_texture_cache.evict_unused(ps4_bound_texture_addresses());
		m_secondary_cb_guard.unlock();
	}

	// Unreleased and unlocked sections, the reusable image pool and temporary subresources.
	m_texture_cache.handle_memory_pressure(rsx::problem_severity::fatal);

	// Stale and invalidated surfaces.
	if (m_rtts.handle_memory_pressure(*m_current_command_buffer, rsx::problem_severity::fatal))
	{
		m_rtts.trim(*m_current_command_buffer, rsx::problem_severity::fatal);
	}

	// DMA mirrors not used for 2 s that hold nothing the CPU still has to read.
	const u64 dma_freed = ps4_sweep_dma(true);

	// Caches whose owners registered a reclaimer (shader interpreter, pipelines, descriptor pools).
	const u64 registered = utils::ps4::vram::run_reclaimers(3);

	// Bound textures or surfaces may have been replaced: samplers are looked up again.
	m_samplers_dirty.store(true);

	// The evicted resources went to the garbage collector: a second hard sync frees them now. Evicted textures pass
	// through the reusable image pool on the way; their images are idle there and destroyed at once.
	flush_command_queue(true, true);
	g_fxo->get<vk::driver_manager_thread>().drain();
	const u64 pool_freed = m_texture_cache.ps4_drop_image_pool();

	u64 used_after = used_before, hole_after = hole_before;
	vk::ps4_window_meter(size, used_after, hole_after, true);

	const u32 runs = ++m_ps4_deep_reclaims;
	if (!(runs & (runs - 1)))
	{
		// The 1st, 2nd, 4th... run of this renderer.
		const std::string line = fmt::format("VRAM: deep reclaim #%u for %s in %llu ms: textures %llu -> %llu MiB, surfaces %llu -> %llu MiB, "
			"image pool %llu MiB, DMA %llu MiB, registered %llu MiB; window %s",
			runs, why ? why : "an allocation", (get_system_time() - start) / 1000,
			textures_before >> 20, vk::vmm_get_application_pool_usage(VMM_ALLOCATION_POOL_TEXTURE_CACHE) >> 20,
			surfaces_before >> 20, vk::vmm_get_application_pool_usage(VMM_ALLOCATION_POOL_SURFACE_CACHE) >> 20,
			pool_freed >> 20, dma_freed >> 20, registered >> 20,
			metered ? fmt::format("mapped %llu -> %llu MiB, largest free range %llu -> %llu MiB", used_before >> 20, used_after >> 20, hole_before >> 20, hole_after >> 20)
				: std::string("not metered"));
		rsx_log.error("PS4 %s", line);
		ps4_startup_checkpoint(line.c_str());
	}

	m_ps4_in_deep_reclaim = false;
	return true;
}

namespace vk
{
	// vkutils/descriptors.cpp (0.38): window bytes of descriptor pools already handed to the garbage collector.
	u64 ps4_descriptor_pool_window_releasing() noexcept;
}

u64 VKGSRender::ps4_program_reclaimer(int level) noexcept
{
	auto renderer = dynamic_cast<VKGSRender*>(rsx::get_current_renderer());
	if (!renderer || !renderer->is_current_thread() || level < 2)
	{
		return 0;
	}

	// The interpreter cache shrinks to the shedding cap at its next variant change; ps4_shed_programs restores it once
	// the pressure is gone.
	vk::ps4_set_interpreter_cap(utils::ps4::rsx_programs::shed_interpreter_cap);
	renderer->m_ps4_shedding = true;

	if (!renderer->m_ps4_pool_release_state)
	{
		renderer->m_ps4_pool_release_state = fs::is_file("/data/rpcs3/no-pool-release.flag") ? 2 : 1;
	}

	if (renderer->m_ps4_pool_release_state != 1)
	{
		return 0;
	}

	// Pools of every draw program table not committed since the previous sweep go to the garbage collector now (the
	// deep reclaim's second hard sync frees them). Never a table of the current draw: draw commits run uninterruptible
	// (VKDraw.cpp), where no deep reclaim runs, and a table committed since the last sweep is never released.
	const u64 releasing_before = vk::ps4_descriptor_pool_window_releasing();
	vk::glsl::ps4_request_descriptor_pool_release();
	vk::glsl::ps4_sweep_descriptor_pools(get_system_time());
	renderer->m_ps4_last_shed_release_us = get_system_time();

	const u64 releasing_after = vk::ps4_descriptor_pool_window_releasing();
	return releasing_after > releasing_before ? releasing_after - releasing_before : 0;
}

void VKGSRender::ps4_shed_programs()
{
	namespace rp = utils::ps4::rsx_programs;

	const u64 now = get_system_time();
	if (now < m_ps4_next_shed_check_us)
	{
		return;
	}

	m_ps4_next_shed_check_us = now + 1'000'000;

	const int heap = utils::ps4::governor::heap_pressure();
	const f32 load = vk::ps4_window_load_percent();

	if (!rp::shed_programs(heap, load, m_ps4_shedding))
	{
		if (m_ps4_shedding)
		{
			m_ps4_shedding = false;
			vk::ps4_set_interpreter_cap(0);
		}

		return;
	}

	if (!m_ps4_shedding)
	{
		m_ps4_shedding = true;
		vk::ps4_set_interpreter_cap(rp::shed_interpreter_cap);

		const u32 runs = ++m_ps4_shed_runs;
		if (!(runs & (runs - 1)))
		{
			// The 1st, 2nd, 4th... time of this renderer.
			const std::string line = fmt::format("RSX: memory pressure (CPU heap level %d, GPU window %u%%): shader interpreter cache capped at %u pipelines, "
				"descriptor pools of program tables not used since the last sweep released (shedding #%u)", heap, static_cast<u32>(load), rp::shed_interpreter_cap, runs);
			rsx_log.warning("PS4 %s", line);
			ps4_startup_checkpoint(line.c_str());
		}
	}

	if (!m_ps4_last_shed_release_us || now - m_ps4_last_shed_release_us >= rp::shed_repeat_us)
	{
		// Served by the next descriptor pool sweep (VKPresent.cpp, every 120 frames).
		m_ps4_last_shed_release_us = now;
		vk::glsl::ps4_request_descriptor_pool_release();
	}
}

bool VKGSRender::ps4_is_current_primary_cb(const vk::command_buffer& cmd) const
{
	return m_ps4_upload_staging && is_current_thread() && m_current_command_buffer &&
		&cmd == static_cast<const vk::command_buffer*>(m_current_command_buffer);
}

void VKGSRender::ps4_restart_rings()
{
	// Waits for everything and starts the rings over. Nothing of this draw is allocated yet. Constants and environment
	// blocks that later draws would reuse from the freed space are uploaded again.
	flush_command_queue(true);
	m_vertex_cache->purge();
	vk::data_heap_manager::reset_heap_allocations();
	m_current_frame->reset_heap_ptrs();
	m_last_heap_sync_time = rsx::get_shared_tag();
	m_graphics_state |= rsx::fragment_state_dirty | rsx::vertex_state_dirty | rsx::transform_constants_dirty |
		rsx::fragment_constants_dirty | rsx::fragment_texture_state_dirty | rsx::polygon_stipple_pattern_dirty;
	m_interpreter_state = rsx::invalidate_pipeline_bits;
}

void VKGSRender::ps4_check_aux_upload_heap()
{
	// Uploads that did not fit the auxiliary upload heap since the last draw (each got its own staging buffer).
	const u32 seen = vk::ps4_aux_staging_uploads();
	if (seen == m_ps4_aux_staging_seen) [[likely]]
	{
		return;
	}

	const u32 fresh = seen - m_ps4_aux_staging_seen;
	m_ps4_aux_staging_seen = seen;
	if (!m_ps4_ring_growth)
	{
		return;
	}

	const u64 now = get_system_time();
	bool busy = false;
	for (u32 i = 0; i < std::min<u32>(fresh, 8); i++)
	{
		busy = utils::ps4::vram::ring_rate_note(m_ps4_aux_rate, now) || busy;
	}

	if (!busy || now < m_ps4_aux_retry_us)
	{
		return;
	}

	vk::data_heap& aux = *vk::get_upload_heap();
	const usz old_size = aux.size();
	const usz target = utils::ps4::vram::ring_growth_target(old_size, usz{64} << 20);
	if (!aux.heap || !target)
	{
		return;
	}

	const std::string name = aux.ps4_name();
	u64 window_size = 0, window_used = 0, window_hole = 0;
	const bool meter = vk::ps4_window_meter(window_size, window_used, window_hole, true);
	const f32 load = vk::ps4_window_load_percent();
	// For this heap a reclaim is an upload that did not fit it (it got its own staging buffer).
	const char* const reason = "after 3 s of more than 2 reclaims per second";

	if (!meter || !utils::ps4::vram::ring_growth_admitted(target, window_hole, load))
	{
		m_ps4_aux_retry_us = now + utils::ps4::vram::ring_growth_retry_us;
		const u32 skips = ++m_ps4_ring_growth_skips;
		const std::string line = fmt::format("PS4 RSX: ring '%s' (%u MiB) not grown %s: window %s, largest free range %u MiB, load %u%% (skipped growths %u)",
			name, static_cast<u32>(old_size >> 20), reason, meter ? "too full" : "meter off", static_cast<u32>(window_hole >> 20), static_cast<u32>(load), skips);
		rsx_log.warning("%s", line);
		if (!(skips & (skips - 1)))
		{
			ps4_startup_checkpoint(line.c_str());
		}
		return;
	}

	// The heap is reset at every frame cleanup while queued frames may still read it, so its size changes only after
	// a hard sync. Called before the draw allocates anything, like the ring growth.
	g_fxo->get<rsx::dma_manager>().sync();
	ps4_restart_rings();

	if (!vk::ps4_resize_ring(aux, target))
	{
		m_ps4_aux_retry_us = now + utils::ps4::vram::ring_growth_retry_us;
		const u32 skips = ++m_ps4_ring_growth_skips;
		const std::string line = fmt::format("PS4 RSX: ring '%s' growth %u -> %u MiB refused by the allocator (%s); retried in 30 s at the earliest (skipped growths %u)",
			name, static_cast<u32>(old_size >> 20), static_cast<u32>(target >> 20), reason, skips);
		rsx_log.warning("%s", line);
		if (!(skips & (skips - 1)))
		{
			ps4_startup_checkpoint(line.c_str());
		}
		return;
	}

	m_ps4_ring_growths++;
	m_ps4_aux_rate = {};

	bool recorded = false;
	if (!m_ps4_ring_title.empty())
	{
		const std::string record = utils::ps4::vram::ring_record_set(
			utils::ps4::adapt::read_record(m_ps4_ring_title, utils::ps4::adapt::rings_suffix), name, static_cast<u32>(target >> 20));
		recorded = utils::ps4::adapt::write_record(m_ps4_ring_title, utils::ps4::adapt::rings_suffix, record);
	}

	const std::string line = fmt::format("PS4 RSX: ring '%s' grew %u -> %u MiB %s (window largest free range %u MiB, load %u%%; growth %u of this renderer; %s)",
		name, static_cast<u32>(old_size >> 20), static_cast<u32>(target >> 20), reason, static_cast<u32>(window_hole >> 20), static_cast<u32>(load),
		m_ps4_ring_growths, recorded ? "kept for the next boot of this title" : "not recorded");
	rsx_log.warning("%s", line);
	ps4_startup_checkpoint(line.c_str());
}

void VKGSRender::ps4_check_ring_heaps()
{
	// 0.39: the auxiliary upload heap first (it may hard-sync, which also empties every ring).
	ps4_check_aux_upload_heap();

	const auto critical = [this]() -> const vk::data_heap*
	{
		for (const vk::data_heap* ring : m_ps4_rings)
		{
			if (ring && ring->heap && ring->ps4_contiguous_free() < ring->ps4_guard())
			{
				return ring;
			}
		}

		return nullptr;
	};

	const vk::data_heap* ring = critical();
	if (!ring) [[likely]]
	{
		return;
	}

	// 0.37: vertex copies handed to the RSX copy worker write ring space of this frame; they land before any reclaim
	// below resets the rings (flush_command_queue syncs too). Only here, not before every draw: the worker would
	// lose its overlap with the draws.
	g_fxo->get<rsx::dma_manager>().sync();

	const std::string name = ring->ps4_name();
	const u32 ring_mib = static_cast<u32>(ring->size() >> 20);
	const u32 guard_mib = static_cast<u32>(ring->ps4_guard() >> 20);

	// Oldest queued frames first: each cleanup waits for that frame and returns the ring space it held. With the
	// auxiliary frame context in use the queue belongs to the borrowed slot, so this goes straight to the hard sync
	// (upstream's check_heap_status did the same).
	const usz queued_before = m_queued_frames.size();
	check_present_status();

	while (critical() && !m_queued_frames.empty() && m_current_frame != &m_aux_frame_context)
	{
		frame_context_cleanup(m_queued_frames.front());
	}

	const u32 retired = static_cast<u32>(queued_before - m_queued_frames.size());
	bool hard_sync = false;

	if (critical())
	{
		// The current frame alone fills the ring.
		ps4_restart_rings();
		hard_sync = true;
		m_ps4_ring_hard_syncs++;
	}

	const u32 reclaims = ++m_ps4_ring_reclaims;
	if (!(reclaims & (reclaims - 1)) || (hard_sync && !(m_ps4_ring_hard_syncs & (m_ps4_ring_hard_syncs - 1))))
	{
		// The 1st, 2nd, 4th... reclaim and hard sync of this renderer. Without ring_growth the 0.38 text, verbatim.
		const std::string line = m_ps4_ring_growth
			? fmt::format("PS4 RSX: ring '%s' (%u MiB) within its %u MiB guard: %u queued frame(s) retired%s; adaptive growth on (reclaims %u, hard syncs %u)",
				name, ring_mib, guard_mib, retired, hard_sync ? ", hard sync" : "", reclaims, m_ps4_ring_hard_syncs)
			: fmt::format("PS4 RSX: ring '%s' (%u MiB) within its %u MiB guard: %u queued frame(s) retired%s; no growth (reclaims %u, hard syncs %u)",
				name, ring_mib, guard_mib, retired, hard_sync ? ", hard sync" : "", reclaims, m_ps4_ring_hard_syncs);
		rsx_log.warning("%s", line);
		ps4_startup_checkpoint(line.c_str());
	}

	// 0.39: adaptive growth of this ring after a ring hard sync, or after more than ring_busy_reclaims reclaims per
	// second for ring_busy_seconds seconds. It grows right after a hard sync (the one above, or one made for it), when
	// nothing of any ring is in use or referenced by a queued frame's heap snapshot.
	const usz index = static_cast<usz>(std::find(m_ps4_rings.begin(), m_ps4_rings.end(), ring) - m_ps4_rings.begin());
	if (!m_ps4_ring_growth || index >= m_ps4_rings.size())
	{
		return;
	}

	const u64 now = get_system_time();
	const bool busy = utils::ps4::vram::ring_rate_note(m_ps4_ring_rate[index], now);
	if ((!hard_sync && !busy) || now < m_ps4_ring_retry_us[index])
	{
		return;
	}

	vk::data_heap& grow_ring = *m_ps4_rings[index];
	const usz old_size = grow_ring.size();
	const usz target = utils::ps4::vram::ring_growth_target(old_size, usz{m_ps4_ring_max_mib[index]} << 20);
	if (!target)
	{
		return;
	}

	u64 window_size = 0, window_used = 0, window_hole = 0;
	const bool meter = vk::ps4_window_meter(window_size, window_used, window_hole, true);
	const f32 load = vk::ps4_window_load_percent();
	const char* const reason = hard_sync ? "after a hard sync" : "after 3 s of more than 2 reclaims per second";

	if (!meter || !utils::ps4::vram::ring_growth_admitted(target, window_hole, load))
	{
		// Not now: the window has no room for it. Looked at again after the retry period.
		m_ps4_ring_retry_us[index] = now + utils::ps4::vram::ring_growth_retry_us;
		const u32 skips = ++m_ps4_ring_growth_skips;
		const std::string line = fmt::format("PS4 RSX: ring '%s' (%u MiB) not grown %s: window %s, largest free range %u MiB, load %u%% (skipped growths %u)",
			name, static_cast<u32>(old_size >> 20), reason, meter ? "too full" : "meter off", static_cast<u32>(window_hole >> 20), static_cast<u32>(load), skips);
		rsx_log.warning("%s", line);
		if (!(skips & (skips - 1)))
		{
			ps4_startup_checkpoint(line.c_str());
		}
		return;
	}

	if (!hard_sync)
	{
		ps4_restart_rings();
	}

	if (!vk::ps4_resize_ring(grow_ring, target))
	{
		m_ps4_ring_retry_us[index] = now + utils::ps4::vram::ring_growth_retry_us;
		const u32 skips = ++m_ps4_ring_growth_skips;
		const std::string line = fmt::format("PS4 RSX: ring '%s' growth %u -> %u MiB refused by the allocator (%s); retried in 30 s at the earliest (skipped growths %u)",
			name, static_cast<u32>(old_size >> 20), static_cast<u32>(target >> 20), reason, skips);
		rsx_log.warning("%s", line);
		if (!(skips & (skips - 1)))
		{
			ps4_startup_checkpoint(line.c_str());
		}
		return;
	}

	m_ps4_ring_growths++;
	m_ps4_ring_rate[index] = {};

	// The next boot of this title starts with the grown size.
	bool recorded = false;
	if (!m_ps4_ring_title.empty())
	{
		const std::string record = utils::ps4::vram::ring_record_set(
			utils::ps4::adapt::read_record(m_ps4_ring_title, utils::ps4::adapt::rings_suffix), name, static_cast<u32>(target >> 20));
		recorded = utils::ps4::adapt::write_record(m_ps4_ring_title, utils::ps4::adapt::rings_suffix, record);
	}

	const std::string line = fmt::format("PS4 RSX: ring '%s' grew %u -> %u MiB %s (window largest free range %u MiB, load %u%%; growth %u of this renderer; %s)",
		name, static_cast<u32>(old_size >> 20), static_cast<u32>(target >> 20), reason, static_cast<u32>(window_hole >> 20), static_cast<u32>(load),
		m_ps4_ring_growths, recorded ? "kept for the next boot of this title" : "not recorded");
	rsx_log.warning("%s", line);
	ps4_startup_checkpoint(line.c_str());
}

void VKGSRender::ps4_check_gpu_liveness()
{
	const u64 now = get_system_time();
	if (now < m_ps4_next_liveness_scan_us)
	{
		return;
	}

	// The pending table and the foreground stretch start over only when the process really was away (home screen,
	// overlay, rest mode: the focus epoch of ps4_fatal.cpp changed), since the system may hold its GPU work meanwhile.
	// A gap between two scans alone is this thread blocked in a wait on the hung GPU: GoW Ascension 0.39 reset the
	// table on every such gap and never confirmed its hang.
	if (const u32 epoch = ps4_focus_epoch(); epoch != m_ps4_liveness_epoch)
	{
		m_ps4_liveness_epoch = epoch;
		m_ps4_pending_cbs.fill(ps4_pending_cb_t{});
		m_ps4_foreground_since_us = 0;
	}

	m_ps4_next_liveness_scan_us = now + 1'000'000;

	// GoW 0.35 froze on a submission the GPU never finished, and nothing on the RPCS3 side named it: the FIFO kept
	// spinning on a jump-to-self and no fence was waited on. Each submission is timed from the first scan that saw it
	// pending (so the age is up to 1 s short); a reset id change means the buffer was reused for a new one.
	u64 oldest_us = 0;
	m_primary_cb_list.ps4_for_each([&](u32 index, vk::command_buffer_chunk& cb)
	{
		ps4_pending_cb_t& seen = m_ps4_pending_cbs[index];
		if (!cb.ps4_submitted_unsignaled())
		{
			seen = {};
			return;
		}

		if (!seen.first_seen_us || seen.reset_id != cb.reset_id)
		{
			seen = { cb.reset_id, now };
			return;
		}

		oldest_us = std::max(oldest_us, now - seen.first_seen_us);
	});

	const u32 age_ms = static_cast<u32>(std::min<u64>(oldest_us / 1000, u32{umax}));
	utils::ps4::gpu::note_unfinished_submission(age_ms);

	if (age_ms >= utils::ps4::gpu::unfinished_report_ms && !m_ps4_unfinished_logged)
	{
		m_ps4_unfinished_logged = true;
		const std::string line = fmt::format("GPU: a command buffer submitted %u ms ago has not finished (GPU hang? mesa.log names the submit)", age_ms);
		rsx_log.error("PS4 %s", line);
		ps4_startup_checkpoint(line.c_str());
	}

	// 0.39: confirmed hang: the submission stayed unfinished for 10 s while the game was in the foreground the whole
	// time (the home screen pauses the game, and the system may hold its GPU work meanwhile).
	if (ps4_in_background())
	{
		m_ps4_foreground_since_us = 0;
	}
	else if (!m_ps4_foreground_since_us)
	{
		m_ps4_foreground_since_us = now;
	}

	if (m_ps4_foreground_since_us && utils::ps4::gpu::hang_confirmed(oldest_us, now - m_ps4_foreground_since_us))
	{
		vk::ps4_request_gpu_hang_stop(fmt::format("a command buffer submitted %u ms ago has not finished", age_ms).c_str());
	}
}

namespace vk
{
	void ps4_gpu_liveness_in_wait() noexcept
	{
		// The table belongs to the RSX thread: never scanned from another one.
		if (!rsx::ps4_is_rsx_thread())
		{
			return;
		}

		if (auto* const renderer = dynamic_cast<VKGSRender*>(rsx::get_current_renderer()); renderer && renderer->is_current_thread())
		{
			renderer->ps4_liveness_scan_in_wait();
		}
	}
}

void VKGSRender::ps4_report_vk_backend()
{
	const u64 now = get_system_time();
	if (!m_ps4_vk_report_start_us)
	{
		// First call of this renderer: the window starts here.
		m_ps4_vk_report_start_us = now;
		m_ps4_vk_report_links = vk::ps4_interpreter_first_use_links(m_ps4_vk_report_link_us);
		m_ps4_vk_report_spills = vk::ps4_descriptor_spills();

		auto& pst = utils::ps4::rsx_programs::g_pipeline_stats;
		m_ps4_vk_report_translate = pst.rsx_translate.load(std::memory_order_relaxed);
		m_ps4_vk_report_translate_us = pst.rsx_translate_us.load(std::memory_order_relaxed);
		m_ps4_vk_report_translate_async = pst.translate_async.load(std::memory_order_relaxed);
		m_ps4_vk_report_translate_back = pst.translate_taken_back.load(std::memory_order_relaxed);
		pst.rsx_translate_max_us.store(0, std::memory_order_relaxed);
		return;
	}

	if (now - m_ps4_vk_report_start_us < 10'000'000)
	{
		return;
	}

	const u64 window_us = now - m_ps4_vk_report_start_us;
	m_ps4_vk_report_start_us = now;

	// Everything is read and reset each window, also when nothing is printed.
	const vk::ps4_vk_draw_counters_t dc = std::exchange(vk::g_ps4_vk_draw_counters, vk::ps4_vk_draw_counters_t{});
	const std::array<u64, vk::ps4_radv_site_count> radv = std::exchange(vk::s_ps4_radv_ticks, std::array<u64, vk::ps4_radv_site_count>{});

	u64 link_us = 0;
	const u64 links = vk::ps4_interpreter_first_use_links(link_us);
	const u64 window_links = links - std::min(m_ps4_vk_report_links, links);
	const u64 window_link_us = link_us - std::min(m_ps4_vk_report_link_us, link_us);
	m_ps4_vk_report_links = links;
	m_ps4_vk_report_link_us = link_us;

	const u64 spills = vk::ps4_descriptor_spills();
	const u64 window_spills = spills - std::min(m_ps4_vk_report_spills, spills);
	m_ps4_vk_report_spills = spills;

	// 0.39: new shader programs translated in this window on the RSX thread (count, time, longest one), on the
	// pipeline workers, and taken back from the workers. The 10 s line goes to startup.log only, so a window with a
	// quarter second or more of RSX-thread translation is also written to RPCS3.log.
	auto& pst = utils::ps4::rsx_programs::g_pipeline_stats;
	const u64 translate = pst.rsx_translate.load(std::memory_order_relaxed);
	const u64 translate_us = pst.rsx_translate_us.load(std::memory_order_relaxed);
	const u64 translate_async = pst.translate_async.load(std::memory_order_relaxed);
	const u64 translate_back = pst.translate_taken_back.load(std::memory_order_relaxed);
	const u64 window_translate = translate - std::min(m_ps4_vk_report_translate, translate);
	const u64 window_translate_us = translate_us - std::min(m_ps4_vk_report_translate_us, translate_us);
	const u64 window_translate_async = translate_async - std::min(m_ps4_vk_report_translate_async, translate_async);
	const u64 window_translate_back = translate_back - std::min(m_ps4_vk_report_translate_back, translate_back);
	const u64 window_translate_max_us = pst.rsx_translate_max_us.exchange(0, std::memory_order_relaxed);
	m_ps4_vk_report_translate = translate;
	m_ps4_vk_report_translate_us = translate_us;
	m_ps4_vk_report_translate_async = translate_async;
	m_ps4_vk_report_translate_back = translate_back;

	if (window_translate_us >= 250'000)
	{
		rsx_log.warning("PS4 RSX: %llu new shader programs translated on the RSX thread in the last %llu s (%llu ms, max %llu ms)",
			window_translate, window_us / 1'000'000, window_translate_us / 1000, window_translate_max_us / 1000);
	}

	auto& av = s_ps4_av_readbacks;
	const u64 av_flushes = av.flushes.exchange(0);
	const u64 av_total = av.total_us.exchange(0), av_max = av.max_us.exchange(0);
	const u64 av_request = av.request_us.exchange(0), av_drain = av.drain_us.exchange(0);
	const u64 av_copy = av.copy_us.exchange(0), av_swap = av.swap_us.exchange(0);

	// Game pipeline waits (queue to ready) and the occlusion answers forced visible for skipped draws, per window.
	u64 wait_counts[utils::ps4::rsx_programs::wait_bucket_count]{};
	u64 wait_max_us = 0;
	utils::ps4::rsx_programs::g_pipeline_waits.take(wait_counts, wait_max_us);
	const u64 forced_queries = pst.queries_forced_visible.exchange(0, std::memory_order_relaxed);
	const u64 forced_cond = pst.cond_render_forced_visible.exchange(0, std::memory_order_relaxed);

	if (!dc.frames)
	{
		// No guest frame drew in this window (loading screens without draws, a stalled game).
		return;
	}

	const double frames = static_cast<double>(dc.frames);
	std::string line = fmt::format("Vulkan backend: %llu frames with draws in %llu ms", dc.frames, window_us / 1000);

	// RADV time of the RSX thread, sampled frames only (as the RSX phase timers).
	const double ticks_per_us = static_cast<double>(utils::get_tsc_freq()) / 1'000'000.;
	if (!rsx::g_ps4_rsx_phase.on.observe())
	{
		line += "; radv off (rsx-profiler-off.flag)";
	}
	else if (dc.sampled_frames && ticks_per_us > 0)
	{
		const double per_frame = static_cast<double>(dc.sampled_frames) * ticks_per_us;
		u64 total = 0;
		for (const u64 ticks : radv)
		{
			total += ticks;
		}

		fmt::append(line, "; radv us/frame=%.0f (sampled %llu frames: draw %.0f, bind %.0f, render pass %.0f, barriers %.0f, "
			"descriptor writes at submit %.0f, texture copies %.0f)",
			total / per_frame, dc.sampled_frames, radv[vk::ps4_radv_draw] / per_frame, radv[vk::ps4_radv_bind] / per_frame,
			radv[vk::ps4_radv_render_pass] / per_frame, radv[vk::ps4_radv_barrier] / per_frame,
			radv[vk::ps4_radv_descriptors] / per_frame, radv[vk::ps4_radv_texture_copy] / per_frame);
	}
	else
	{
		line += "; radv us/frame=n/a (no sampled frame)";
	}

	fmt::append(line, "; xform KiB/frame=full %.1f reloc %.1f (uploads/frame %.1f)",
		dc.xform_full_bytes / 1024. / frames, dc.xform_reloc_bytes / 1024. / frames, dc.xform_uploads / frames);

	fmt::append(line, "; interpreter draws/frame=%.1f, variants linked on first use %llu (%llu ms)",
		dc.interpreter_draws / frames, window_links, window_link_us / 1000);

	fmt::append(line, "; submits/frame=%.1f", dc.submits / frames);
	if (m_ps4_cb_split_draws)
	{
		fmt::append(line, " (%llu split after %u draws)", dc.cb_splits, m_ps4_cb_split_draws);
	}

	if (av_flushes)
	{
		const double n = static_cast<double>(av_flushes) * 1000.;
		fmt::append(line, "; av readbacks=%llu (avg %.1f ms, max %.1f ms; avg request %.1f, drain %.1f, copy %.1f, swap %.1f)",
			av_flushes, av_total / n, av_max / 1000., av_request / n, av_drain / n, av_copy / n, av_swap / n);
	}

	if (window_spills)
	{
		fmt::append(line, "; descriptor spills=%llu", window_spills);
	}

	// 0.38: dropped draws, and the program caches' memory state.
	if (dc.skipped_not_ready || dc.skipped_descriptors)
	{
		fmt::append(line, "; draws skipped=%llu not ready, %llu descriptor set refused (%llu sets refused since boot)",
			dc.skipped_not_ready, dc.skipped_descriptors, vk::ps4_descriptor_sets_refused());
	}

	u64 waits = 0;
	for (const u64 count : wait_counts)
	{
		waits += count;
	}

	if (waits)
	{
		fmt::append(line, "; pipeline wait p90/max %llu/%llu ms (%llu pipelines)",
			utils::ps4::rsx_programs::wait_percentile_ms(wait_counts, 90, wait_max_us / 1000), wait_max_us / 1000, waits);
	}

	if (forced_queries || forced_cond || dc.skipped_not_ready)
	{
		fmt::append(line, "; queries forced visible %llu, cond-render forced visible %llu", forced_queries, forced_cond);
	}

	const auto& ps = utils::ps4::rsx_programs::g_pipeline_stats;
	u64 interp_evicted = 0, interp_refused = 0, interp_deferred = 0;
	const u32 interp_live = vk::ps4_interpreter_pipelines_live(interp_evicted, interp_refused, interp_deferred);
	fmt::append(line, "; pipelines live %u (evicted %llu, refused %llu, modules refused %llu, preload skipped %u); "
		"interpreter pipelines live %u (evicted %llu, refused %llu, deferred %llu)%s",
		ps.live.load(std::memory_order_relaxed), ps.evicted.load(std::memory_order_relaxed), ps.refused.load(std::memory_order_relaxed),
		ps.modules_refused.load(std::memory_order_relaxed), ps.preload_skipped.load(std::memory_order_relaxed),
		interp_live, interp_evicted, interp_refused, interp_deferred, m_ps4_shedding ? "; shedding" : "");

	// 0.39 (process totals): texture-less interpreter draws bound to the null-texture environment, sampler slots
	// committed without a sampler (0 expected) and interpreter links run on the priority lane.
	fmt::append(line, "; interp fallback env %llu, unbound sampler slots %llu, lane links %llu",
		ps.interp_fallback_env.load(std::memory_order_relaxed), ps.unbound_sampler_slots.load(std::memory_order_relaxed),
		ps.lane_links.load(std::memory_order_relaxed));

	fmt::append(line, "; programs translated on the RSX thread %llu (%llu ms, max %llu ms), by workers %llu (taken back %llu)",
		window_translate, window_translate_us / 1000, window_translate_max_us / 1000, window_translate_async, window_translate_back);

	ps4_startup_checkpoint(line.c_str());
}
#endif

void VKGSRender::on_descriptor_pool_fragmentation(bool is_fatal)
{
	if (!is_fatal)
	{
		// It is very likely that the release is simply in progress (enqueued)
		m_primary_cb_list.wait_all();
		return;
	}

	// Just flush everything. Unless the hardware is very deficient, this should happen very rarely.
	flush_command_queue(true, true);
}

void VKGSRender::notify_tile_unbound(u32 tile)
{
	//TODO: Handle texture writeback
	if (false)
	{
		u32 addr = rsx::get_address(tiles[tile].offset, tiles[tile].location);
		on_notify_pre_memory_unmapped(addr, tiles[tile].size, *std::make_unique<std::vector<std::pair<u64, u64>>>());
		m_rtts.invalidate_surface_address(addr, false);
	}

	{
		std::lock_guard lock(m_sampler_mutex);
		m_samplers_dirty.store(true);
	}
}

void VKGSRender::check_present_status()
{
	while (!m_queued_frames.empty())
	{
		auto ctx = m_queued_frames.front();
		if (!ctx->swap_command_buffer->poke())
		{
			return;
		}

		frame_context_cleanup(ctx);
	}
}

void VKGSRender::set_viewport()
{
	const auto [clip_width, clip_height] = rsx::apply_resolution_scale<true>(
		resolution_scaling_config,
		rsx::method_registers.surface_clip_width(), rsx::method_registers.surface_clip_height());

	const auto zclip_near = rsx::method_registers.clip_min();
	const auto zclip_far = rsx::method_registers.clip_max();

	//NOTE: The scale_offset matrix already has viewport matrix factored in
	m_viewport.x = 0;
	m_viewport.y = 0;
	m_viewport.width = clip_width;
	m_viewport.height = clip_height;

	if (m_device->get_unrestricted_depth_range_support())
	{
		m_viewport.minDepth = zclip_near;
		m_viewport.maxDepth = zclip_far;
	}
	else
	{
		m_viewport.minDepth = 0.f;
		m_viewport.maxDepth = 1.f;
	}

	m_current_command_buffer->flags |= vk::command_buffer::cb_reload_dynamic_state;
	m_graphics_state.clear(rsx::pipeline_state::zclip_config_state_dirty);
}

void VKGSRender::set_scissor(bool clip_viewport)
{
	areau scissor;
	if (get_scissor(scissor, clip_viewport))
	{
		m_scissor.extent.height = scissor.height();
		m_scissor.extent.width = scissor.width();
		m_scissor.offset.x = scissor.x1;
		m_scissor.offset.y = scissor.y1;

		m_current_command_buffer->flags |= vk::command_buffer::cb_reload_dynamic_state;
	}
}

void VKGSRender::bind_viewport()
{
	if (m_graphics_state & rsx::pipeline_state::zclip_config_state_dirty)
	{
		if (m_device->get_unrestricted_depth_range_support())
		{
			m_viewport.minDepth = rsx::method_registers.clip_min();
			m_viewport.maxDepth = rsx::method_registers.clip_max();
		}

		m_graphics_state.clear(rsx::pipeline_state::zclip_config_state_dirty);
	}

	vkCmdSetViewport(*m_current_command_buffer, 0, 1, &m_viewport);
	vkCmdSetScissor(*m_current_command_buffer, 0, 1, &m_scissor);
}

void VKGSRender::on_init_thread()
{
	if (m_device == VK_NULL_HANDLE)
	{
		fmt::throw_exception("No Vulkan device was created");
	}

	GSRender::on_init_thread();
	zcull_ctrl.reset(static_cast<::rsx::reports::ZCULL_control*>(this));

#ifdef RPCS3_PS4
	// 0.38: the program caches answer the deep reclaim of a required allocation (interpreter cap, descriptor pools);
	// a cap left by the previous game's shedding does not carry over.
	vk::ps4_set_interpreter_cap(0);
	utils::ps4::vram::register_reclaimer(&VKGSRender::ps4_program_reclaimer);

	// 0.37: draws whose pipeline still compiles go to the shader interpreter (PC default) instead of being dropped.
	// The variant preload below is only a warm-up: shader_interpreter::get() links a missing variant synchronously
	// on first use. shader-interpreter-lazy.flag skips it (boot time against first-use hitches; both are timed).
	// 0.38: lazy by default. The preload linked 6650 pipelines before every Vulkan game (FIFA 14: 87 min, +64 MiB
	// of CPU heap, and every one of them pinned in the GPU window until the stop). Variants link on first use (the
	// interpreter's first-use path and its cache bound are the shader interpreter's). shader-interpreter-preload.flag
	// restores the preload.
	constexpr bool ps4_interpreter_lazy_default = true;
	const bool ps4_interpreter_mode =
		g_cfg.video.shadermode == shader_mode::async_with_interpreter || g_cfg.video.shadermode == shader_mode::interpreter_only;
	const bool ps4_interpreter_preload_flag = ps4_interpreter_mode && fs::is_file("/data/rpcs3/shader-interpreter-preload.flag");
	const bool ps4_interpreter_lazy = ps4_interpreter_mode &&
		((ps4_interpreter_lazy_default && !ps4_interpreter_preload_flag) || fs::is_file("/data/rpcs3/shader-interpreter-lazy.flag"));

	if (ps4_interpreter_lazy)
	{
		ps4_startup_checkpoint(ps4_interpreter_lazy_default && !ps4_interpreter_preload_flag
			? "Vulkan: interpreter preload skipped (default); variants link on first use (shader-interpreter-preload.flag restores the preload)"
			: "Vulkan: interpreter preload skipped (shader-interpreter-lazy.flag); variants link on first use");
	}
	else if (ps4_interpreter_preload_flag)
	{
		ps4_startup_checkpoint("Vulkan: interpreter preload on (shader-interpreter-preload.flag)");
	}
#endif

	if ((g_cfg.video.shadermode == shader_mode::async_with_interpreter ||
		g_cfg.video.shadermode == shader_mode::interpreter_only)
#ifdef RPCS3_PS4
		&& !ps4_interpreter_lazy
#endif
		)
	{
		std::unique_ptr<rsx::shader_loading_dialog> dlg = m_overlay_manager
			? std::make_unique<rsx::shader_loading_dialog_native>(this)
			: std::make_unique<rsx::shader_loading_dialog>();
#ifdef RPCS3_PS4
        ps4_startup_checkpoint("Vulkan: interpreter variant preparation started");
        const u64 ps4_preload_start = get_system_time();
#endif
		m_shader_interpreter.preload(dlg.get());
#ifdef RPCS3_PS4
        ps4_startup_checkpoint(fmt::format("Vulkan: interpreter variants ready in %llu ms (%u pipelines)",
            (get_system_time() - ps4_preload_start) / 1000, vk::ps4_interpreter_preload_pipelines()).c_str());
#endif
		dlg->close();
	}

	if (g_cfg.video.shadermode != shader_mode::interpreter_only)
	{
		if (!m_overlay_manager)
		{
			m_frame->hide();
			m_shaders_cache->load(nullptr);
			m_frame->show();
		}
		else
		{
			rsx::shader_loading_dialog_native dlg(this);

			// TODO: Handle window resize messages during loading on GPUs without OUT_OF_DATE_KHR support
#ifdef RPCS3_PS4
            ps4_startup_checkpoint("Vulkan: loading game shader cache");
#endif
			m_shaders_cache->load(&dlg);
#ifdef RPCS3_PS4
            ps4_startup_checkpoint("Vulkan: game shader cache ready");
#endif
		}
	}

	// Now we properly initialize the pipe compiler as per the user's configuration
	if (!g_cfg.video.shader_compiler_threads_count)
	{
		vk::resize_pipe_compiler(g_cfg.video.shader_compiler_threads_count);
	}
}

void VKGSRender::on_exit()
{
	GSRender::on_exit();
	vk::destroy_pipe_compiler(); // Ensure no pending shaders being compiled
	zcull_ctrl.release();
}

void VKGSRender::clear_surface(u32 mask)
{
	if (skip_current_frame || swapchain_unavailable) return;

	// If stencil write mask is disabled, remove clear_stencil bit
	if (!rsx::method_registers.stencil_mask()) mask &= ~RSX_GCM_CLEAR_STENCIL_BIT;

	// Ignore invalid clear flags
	if (!(mask & RSX_GCM_CLEAR_ANY_MASK)) return;

	u8 ctx = rsx::framebuffer_creation_context::context_draw;
	if (mask & RSX_GCM_CLEAR_COLOR_RGBA_MASK) ctx |= rsx::framebuffer_creation_context::context_clear_color;
	if (mask & RSX_GCM_CLEAR_DEPTH_STENCIL_MASK) ctx |= rsx::framebuffer_creation_context::context_clear_depth;
	init_buffers(rsx::framebuffer_creation_context{ctx});

	if (!m_graphics_state.test(rsx::rtt_config_valid))
	{
		return;
	}

	//float depth_clear = 1.f;
	u32   stencil_clear = 0;
	u32   depth_stencil_mask = 0;

	std::vector<VkClearAttachment> clear_descriptors;
	VkClearValue depth_stencil_clear_values = {}, color_clear_values = {};

	u16 scissor_x = static_cast<u16>(m_scissor.offset.x);
	u16 scissor_w = static_cast<u16>(m_scissor.extent.width);
	u16 scissor_y = static_cast<u16>(m_scissor.offset.y);
	u16 scissor_h = static_cast<u16>(m_scissor.extent.height);

	const u16 fb_width = m_draw_fbo->width();
	const u16 fb_height = m_draw_fbo->height();

	//clip region
	std::tie(scissor_x, scissor_y, scissor_w, scissor_h) = rsx::clip_region<u16>(fb_width, fb_height, scissor_x, scissor_y, scissor_w, scissor_h, true);
	VkClearRect region = { { { scissor_x, scissor_y }, { scissor_w, scissor_h } }, 0, 1 };

	const bool full_frame = (scissor_w == fb_width && scissor_h == fb_height);
	bool update_color = false, update_z = false;
	auto surface_depth_format = rsx::method_registers.surface_depth_fmt();

	if (auto ds = std::get<1>(m_rtts.m_bound_depth_stencil); mask & RSX_GCM_CLEAR_DEPTH_STENCIL_MASK)
	{
		if (mask & RSX_GCM_CLEAR_DEPTH_BIT)
		{
			u32 max_depth_value = get_max_depth_value(surface_depth_format);

			u32 clear_depth = rsx::method_registers.z_clear_value(is_depth_stencil_format(surface_depth_format));
			float depth_clear = static_cast<float>(clear_depth) / max_depth_value;

			depth_stencil_clear_values.depthStencil.depth = depth_clear;
			depth_stencil_clear_values.depthStencil.stencil = stencil_clear;

			depth_stencil_mask |= VK_IMAGE_ASPECT_DEPTH_BIT;
		}

		if (is_depth_stencil_format(surface_depth_format))
		{
			if (mask & RSX_GCM_CLEAR_STENCIL_BIT)
			{
				u8 clear_stencil = rsx::method_registers.stencil_clear_value();
				depth_stencil_clear_values.depthStencil.stencil = clear_stencil;

				depth_stencil_mask |= VK_IMAGE_ASPECT_STENCIL_BIT;

				if (ds->samples() > 1)
				{
					if (full_frame) ds->stencil_init_flags &= 0xFF;
					ds->stencil_init_flags |= clear_stencil;
				}
			}
		}

		if ((depth_stencil_mask && depth_stencil_mask != ds->aspect()) || !full_frame)
		{
			// At least one aspect is not being cleared or the clear does not cover the full frame
			// Steps to initialize memory are required

			if (ds->state_flags & rsx::surface_state_flags::erase_bkgnd &&  // Needs initialization
				ds->old_contents.empty() && !g_cfg.video.read_depth_buffer) // No way to load data from memory, so no initialization given
			{
				// Only one aspect was cleared. Make sure to memory initialize the other before removing dirty flag
				const auto ds_mask = (mask & RSX_GCM_CLEAR_DEPTH_STENCIL_MASK);
				if (ds_mask == RSX_GCM_CLEAR_DEPTH_BIT && (ds->aspect() & VK_IMAGE_ASPECT_STENCIL_BIT))
				{
					// Depth was cleared, initialize stencil
					depth_stencil_clear_values.depthStencil.stencil = 0xFF;
					depth_stencil_mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
				}
				else if (ds_mask == RSX_GCM_CLEAR_STENCIL_BIT)
				{
					// Stencil was cleared, initialize depth
					depth_stencil_clear_values.depthStencil.depth = 1.f;
					depth_stencil_mask |= VK_IMAGE_ASPECT_DEPTH_BIT;
				}
			}
			else
			{
				// Barrier required before any writes
				ds->write_barrier(*m_current_command_buffer);
			}
		}
	}

	if (auto colormask = (mask & RSX_GCM_CLEAR_COLOR_RGBA_MASK))
	{
		if (!m_draw_buffers.empty())
		{
			bool use_fast_clear = (colormask == RSX_GCM_CLEAR_COLOR_RGBA_MASK);;
			u8 clear_a = rsx::method_registers.clear_color_a();
			u8 clear_r = rsx::method_registers.clear_color_r();
			u8 clear_g = rsx::method_registers.clear_color_g();
			u8 clear_b = rsx::method_registers.clear_color_b();

			switch (rsx::method_registers.surface_color())
			{
			case rsx::surface_color_format::x32:
			case rsx::surface_color_format::w16z16y16x16:
			case rsx::surface_color_format::w32z32y32x32:
			{
				//NOP
				colormask = 0;
				break;
			}
			case rsx::surface_color_format::b8:
			{
				rsx::get_b8_clear_color(clear_r, clear_g, clear_b, clear_a);
				colormask = rsx::get_b8_clearmask(colormask);
				use_fast_clear = (colormask & RSX_GCM_CLEAR_RED_BIT);
				break;
			}
			case rsx::surface_color_format::g8b8:
			{
				rsx::get_g8b8_clear_color(clear_r, clear_g, clear_b, clear_a);
				colormask = rsx::get_g8b8_r8g8_clearmask(colormask);
				use_fast_clear = ((colormask & RSX_GCM_CLEAR_COLOR_RG_MASK) == RSX_GCM_CLEAR_COLOR_RG_MASK);
				break;
			}
			case rsx::surface_color_format::r5g6b5:
			{
				rsx::get_rgb565_clear_color(clear_r, clear_g, clear_b, clear_a);
				use_fast_clear = ((colormask & RSX_GCM_CLEAR_COLOR_RGB_MASK) == RSX_GCM_CLEAR_COLOR_RGB_MASK);
				break;
			}
			case rsx::surface_color_format::x1r5g5b5_o1r5g5b5:
			{
				rsx::get_a1rgb555_clear_color(clear_r, clear_g, clear_b, clear_a, 255);
				break;
			}
			case rsx::surface_color_format::x1r5g5b5_z1r5g5b5:
			{
				rsx::get_a1rgb555_clear_color(clear_r, clear_g, clear_b, clear_a, 0);
				break;
			}
			case rsx::surface_color_format::a8b8g8r8:
			case rsx::surface_color_format::x8b8g8r8_o8b8g8r8:
			case rsx::surface_color_format::x8b8g8r8_z8b8g8r8:
			{
				rsx::get_abgr8_clear_color(clear_r, clear_g, clear_b, clear_a);
				colormask = rsx::get_abgr8_clearmask(colormask);
				break;
			}
			default:
			{
				break;
			}
			}

			if (colormask)
			{
				if (!use_fast_clear || !full_frame)
				{
					// If we're not clobber all the memory, a barrier is required
					for (const auto& index : m_rtts.m_bound_render_target_ids)
					{
						m_rtts.m_bound_render_targets[index].second->write_barrier(*m_current_command_buffer);
					}
				}

				color_clear_values.color.float32[0] = static_cast<float>(clear_r) / 255;
				color_clear_values.color.float32[1] = static_cast<float>(clear_g) / 255;
				color_clear_values.color.float32[2] = static_cast<float>(clear_b) / 255;
				color_clear_values.color.float32[3] = static_cast<float>(clear_a) / 255;

				if (use_fast_clear)
				{
					for (u32 index = 0; index < m_draw_buffers.size(); ++index)
					{
						clear_descriptors.push_back({ VK_IMAGE_ASPECT_COLOR_BIT, index, color_clear_values });
					}
				}
				else
				{
					color4f clear_color =
					{
						color_clear_values.color.float32[0],
						color_clear_values.color.float32[1],
						color_clear_values.color.float32[2],
						color_clear_values.color.float32[3]
					};

					auto attachment_clear_pass = vk::get_overlay_pass<vk::attachment_clear_pass>();
					attachment_clear_pass->run(*m_current_command_buffer, m_draw_fbo, region.rect, colormask, clear_color, get_render_pass());
				}

				update_color = true;
			}
		}
	}

	if (depth_stencil_mask)
	{
		if ((depth_stencil_mask & VK_IMAGE_ASPECT_STENCIL_BIT) &&
			rsx::method_registers.stencil_mask() != 0xff)
		{
			// Partial stencil clear. Disables fast stencil clear
			auto ds = std::get<1>(m_rtts.m_bound_depth_stencil);
			auto key = vk::get_renderpass_key({ ds });
			auto renderpass = vk::get_renderpass(*m_device, key);

			vk::get_overlay_pass<vk::stencil_clear_pass>()->run(
				*m_current_command_buffer, ds, region.rect,
				depth_stencil_clear_values.depthStencil.stencil,
				rsx::method_registers.stencil_mask(), renderpass);

			depth_stencil_mask &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
		}

		if (depth_stencil_mask)
		{
			clear_descriptors.push_back({ static_cast<VkImageAspectFlags>(depth_stencil_mask), 0, depth_stencil_clear_values });
		}

		update_z = true;
	}

	if (update_color || update_z)
	{
		m_rtts.on_write({ update_color, update_color, update_color, update_color }, update_z);
	}

	if (!clear_descriptors.empty())
	{
		begin_render_pass();
		vkCmdClearAttachments(*m_current_command_buffer, ::size32(clear_descriptors), clear_descriptors.data(), 1, &region);
	}
}

void VKGSRender::flush_command_queue(bool hard_sync, bool do_not_switch)
{
	close_and_submit_command_buffer();

	if (hard_sync)
	{
#ifdef RPCS3_PS4
		// 0.29: hard syncs per 10 s in the 'RSX gameplay' line.
		rsx::g_ps4_rsx_counters.hard_syncs++;
#endif
		// wait for the latest instruction to execute
		m_current_command_buffer->reset();

		// Clear all command buffer statuses
		m_primary_cb_list.poke_all();

		// Drain present queue
#ifdef RPCS3_PS4
		// Same drain, but back off between polls: on Orbis's fixed-priority scheduler a raised RSX
		// thread spinning on poke() while the GPU drains starves the PPU/SPU threads on its core.
		// 0.29:this polls the queued frames' fences, so it shows as vk_fence in a STALL.
		const rsx::ps4_rsx_wait_scope ps4_wait(rsx::ps4_wait_vk_fence);
		// 0.31: no deadline either; report a GPU hang after 10 s like the untimed fence wait.
		vk::ps4_fence_watchdog watchdog;
		for (u32 spins = 0; !m_queued_frames.empty(); spins++)
		{
			check_present_status();

			if (!m_queued_frames.empty())
			{
				watchdog.poll(spins);
				vk::ps4_gpu_wait_backoff(spins);
			}
		}
		watchdog.done();
#else
		while (!m_queued_frames.empty())
		{
			check_present_status();
		}
#endif

		m_flush_requests.clear_pending_flag();
	}

	if (!do_not_switch)
	{
		// Grab next cb in line and make it usable
		// NOTE: Even in the case of a hard sync, this is required to free any waiters on the CB (ZCULL)
		m_current_command_buffer = m_primary_cb_list.next();
		m_current_command_buffer->reset();
	}
	else
	{
		// Special hard-sync where we must preserve the CB. This can happen when an emergency event handler is invoked and needs to flush to hw.
		ensure(hard_sync);
	}

	// Just in case a queued frame holds a ref to this cb, drain the present queue
	check_present_status();

	if (m_occlusion_query_active)
	{
		m_current_command_buffer->flags |= vk::command_buffer::cb_load_occluson_task;
	}

	m_current_command_buffer->begin();
}

std::pair<volatile vk::host_data_t*, VkBuffer> VKGSRender::map_host_object_data() const
{
	return { m_host_dma_ctrl->host_ctx(), m_host_object_data->value };
}

bool VKGSRender::release_GCM_label(u32 type, u32 address, u32 args)
{
	if (!backend_config.supports_host_gpu_labels)
	{
		return false;
	}

	auto host_ctx = ensure(m_host_dma_ctrl->host_ctx());

	if (type == NV4097_TEXTURE_READ_SEMAPHORE_RELEASE && host_ctx->texture_loads_completed())
	{
		// All texture loads already seen by the host GPU
		// Wait for all previously submitted labels to be flushed
		m_host_dma_ctrl->drain_label_queue();
		return false;
	}

	const auto mapping = vk::map_dma(address, 4);
	const auto write_data = std::bit_cast<u32, be_t<u32>>(args);

	if (!dynamic_cast<vk::memory_block_host*>(mapping.second->memory.get()))
	{
		// NVIDIA GPUs can disappoint when DMA blocks straddle VirtualAlloc boundaries.
		// Take the L and try the fallback.
		rsx_log.warning("Host label update at 0x%x was not possible.", address);
		m_host_dma_ctrl->drain_label_queue();
		return false;
	}

	const auto release_event_id = host_ctx->on_label_acquire();

	vk::insert_global_memory_barrier(*m_current_command_buffer);

	if (host_ctx->has_unflushed_texture_loads())
	{
		vkCmdUpdateBuffer(*m_current_command_buffer, mapping.second->value, mapping.first, 4, &write_data);
		flush_command_queue();
	}
	else
	{
		auto cmd = m_secondary_cb_list.next();
		cmd->begin();
		vkCmdUpdateBuffer(*cmd, mapping.second->value, mapping.first, 4, &write_data);
		vkCmdUpdateBuffer(*cmd, m_host_object_data->value, ::offset32(&vk::host_data_t::commands_complete_event), 8, &release_event_id);
		cmd->end();

		vk::queue_submit_t submit_info = { m_device->get_graphics_queue(), nullptr };
		cmd->submit(submit_info);

		host_ctx->on_label_release();
	}

	return true;
}

void VKGSRender::on_guest_texture_read(const vk::command_buffer& cmd)
{
	if (!backend_config.supports_host_gpu_labels)
	{
		return;
	}

	// Queue a sync update on the CB doing the load
	auto host_ctx = ensure(m_host_dma_ctrl->host_ctx());
	const auto event_id = host_ctx->on_texture_load_acquire();
	vkCmdUpdateBuffer(cmd, m_host_object_data->value, ::offset32(&vk::host_data_t::texture_load_complete_event), sizeof(u64), &event_id);
}

void VKGSRender::write_barrier(u32 address, u32 range)
{
	ensure(is_current_thread());
	m_rtts.invalidate_range(utils::address_range32::start_length(address, range));
}

void VKGSRender::sync_hint(rsx::FIFO::interrupt_hint hint, rsx::reports::sync_hint_payload_t payload)
{
	rsx::thread::sync_hint(hint, payload);

	if (!(m_current_command_buffer->flags & vk::command_buffer::cb_has_occlusion_task))
	{
		// Occlusion queries not enabled, do nothing
		return;
	}

	// Occlusion test result evaluation is coming up, avoid a hard sync
	switch (hint)
	{
	case rsx::FIFO::interrupt_hint::conditional_render_eval:
	{
		// If a flush request is already enqueued, do nothing
		if (m_flush_requests.pending())
		{
			return;
		}

		// If the result is not going to be read by CELL, do nothing
		const auto ref_addr = static_cast<u32>(payload.address);
		if (!zcull_ctrl->is_query_result_urgent(ref_addr))
		{
			// No effect on CELL behaviour, it will be faster to handle this in RSX code
			return;
		}

		// OK, cell will be accessing the results, probably.
		// Try to avoid flush spam, it is more costly to flush the CB than it is to just upload the vertex data
		// This is supposed to be an optimization afterall.
		const auto now = get_system_time();
		if ((now - m_last_cond_render_eval_hint) > 50)
		{
			// Schedule a sync on the next loop iteration
			m_flush_requests.post(false);
			m_flush_requests.remove_one();
		}

		m_last_cond_render_eval_hint = now;
		break;
	}
	case rsx::FIFO::interrupt_hint::zcull_sync:
	{
		// Check if the required report is synced to this CB
		auto& data = m_occlusion_map[payload.query->driver_handle];

		// NOTE: Currently, a special condition exists where the indices can be empty even with active draw count.
		// This is caused by async compiler and should be removed when ubershaders are added in
		if (!data.is_current(m_current_command_buffer) || data.indices.empty())
		{
			return;
		}

		// Unavoidable hard sync coming up, flush immediately
		// This heavyweight hint should be used with caution
		std::lock_guard lock(m_flush_queue_mutex);
		flush_command_queue();

		if (m_flush_requests.pending())
		{
			// Clear without wait
			m_flush_requests.clear_pending_flag();
		}
		break;
	}
	}
}

void VKGSRender::do_local_task(rsx::FIFO::state state)
{
	if (m_queue_status & flush_queue_state::deadlock)
	{
		// Clear offloader deadlock
		// NOTE: It is not possible to handle regular flush requests before this is cleared
		// NOTE: This may cause graphics corruption due to unsynchronized modification
		on_invalidate_memory_range(m_offloader_fault_range, m_offloader_fault_cause);
		m_queue_status.clear(flush_queue_state::deadlock);
	}

	if (m_queue_status & flush_queue_state::flushing)
	{
		// Abort recursive CB submit requests.
		// When flushing flag is already set, only deadlock events may be processed.
		return;
	}
	else if (m_flush_requests.pending())
	{
		if (m_flush_queue_mutex.try_lock())
		{
			// TODO: Determine if a hard sync is necessary
			// Pipeline barriers later may do a better job synchronizing than wholly stalling the pipeline
			flush_command_queue();

			m_flush_requests.clear_pending_flag();
#ifdef RPCS3_PS4
			{
				// 0.29:the RSX is parked until the faulting thread finished its flush.
				const rsx::ps4_rsx_wait_scope ps4_wait(rsx::ps4_wait_flush_request);
				// 0.32: set after the flush above and cleared only once every faulting thread dropped its queue
				// reference, so a faulting thread that still holds one and reads true knows this thread is idle.
				m_ps4_rsx_parked_for_flush.store(true);
				m_flush_requests.consumer_wait();
				m_ps4_rsx_parked_for_flush.store(false);
			}
#else
			m_flush_requests.consumer_wait();
#endif
			m_flush_queue_mutex.unlock();
		}
	}
	else if (!in_begin_end && state != rsx::FIFO::state::lock_wait)
	{
		if (m_graphics_state & rsx::pipeline_state::framebuffer_reads_dirty)
		{
			//This will re-engage locks and break the texture cache if another thread is waiting in access violation handler!
			//Only call when there are no waiters
			m_texture_cache.do_update();
			m_graphics_state.clear(rsx::pipeline_state::framebuffer_reads_dirty);
		}
	}

	rsx::thread::do_local_task(state);

#ifdef RPCS3_PS4
	// 0.36: about once per second, also while the FIFO spins on a jump-to-self.
	ps4_check_gpu_liveness();

	// 0.37: the 10 s 'Vulkan backend' line (its counters belong to the RSX thread).
	if (is_current_thread())
	{
		ps4_report_vk_backend();

		// 0.38: program cache shedding under CPU heap or GPU window pressure (about once per second).
		ps4_shed_programs();
	}
#endif

	switch (state)
	{
	case rsx::FIFO::state::lock_wait:
		// Critical check finished
		return;
	//case rsx::FIFO::state::spinning:
	//case rsx::FIFO::state::empty:
		// We have some time, check the present queue
		//check_present_status();
		//break;
	default:
		break;
	}

	if (m_overlay_manager)
	{
		const auto should_ignore = in_begin_end && state != rsx::FIFO::state::empty;
		if ((async_flip_requested & flip_request::native_ui) && !should_ignore && !is_stopped())
		{
			flush_command_queue(true);
			rsx::display_flip_info_t info{};
			info.buffer = current_display_buffer;
			flip(info);
		}
	}
}

bool VKGSRender::load_program()
{
	const auto shadermode = g_cfg.video.shadermode.get();

	// TODO: EXT_dynamic_state should get rid of this sillyness soon (kd)
	const auto vertex_state = vk::decode_vertex_input_assembly_state();

	if (m_graphics_state & rsx::pipeline_state::invalidate_pipeline_bits)
	{
		get_current_fragment_program(fs_sampler_state);
		ensure(current_fragment_program.valid);

		get_current_vertex_program(vs_sampler_state);

		m_graphics_state.clear(rsx::pipeline_state::invalidate_pipeline_bits);
	}
	else if (!(m_graphics_state & rsx::pipeline_state::pipeline_config_dirty) &&
		m_program &&
		m_pipeline_properties.state.ia.topology == vertex_state.primitive &&
		m_pipeline_properties.state.ia.primitiveRestartEnable == vertex_state.restart_index_enabled)
	{
		if (!m_shader_interpreter.is_interpreter(m_program)) [[ likely ]]
		{
			return true;
		}

		if (shadermode == shader_mode::interpreter_only)
		{
			m_program = m_shader_interpreter.get(
				m_pipeline_properties,
				current_fp_metadata,
				current_vp_metadata,
				current_vertex_program.ctrl,
				current_fragment_program.ctrl);

#ifdef RPCS3_PS4
			// 0.38: no interpreter program (alpha test 'never', or a variant link the driver refused): the draw is
			// skipped as not ready instead of being set up without a program.
			if (!m_program)
			{
				m_vs_binding_table = nullptr;
				m_fs_binding_table = nullptr;
				return false;
			}
#endif
			std::tie(m_vs_binding_table, m_fs_binding_table) = get_binding_table();
			return true;
		}
	}

	if (!!(current_fragment_program.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING) !=
		vk::renderpass_has_input_attachments(m_current_renderpass_key))
	{
		invalidate_render_pass();
	}

	auto &vertex_program = current_vertex_program;
	auto &fragment_program = current_fragment_program;

	if (m_graphics_state & rsx::pipeline_state::pipeline_config_dirty)
	{
		vk::pipeline_props properties = vk::decode_rsx_state(
			m_ctx,
			vertex_state,
			m_rtts.m_bound_depth_stencil.second,
			backend_config,
			static_cast<u8>(m_draw_buffers.size()),
			u8((m_current_renderpass_key >> 16) & 0xF),
			m_device->get_depth_bounds_support(),
			!!(current_fragment_program.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING)
		);

		properties.renderpass_key = m_current_renderpass_key;
		if (m_program &&
			!m_shader_interpreter.is_interpreter(m_program) &&
			m_pipeline_properties == properties)
		{
			// Nothing changed
			return true;
		}

		// Fallthrough
		m_pipeline_properties = properties;
		m_graphics_state.clear(rsx::pipeline_state::pipeline_config_dirty);
	}
	else
	{
		// Update primitive type and restart index. Note that this is not needed with EXT_dynamic_state
		m_pipeline_properties.state.set_primitive_type(vertex_state.primitive);
		m_pipeline_properties.state.enable_primitive_restart(vertex_state.restart_index_enabled);
		m_pipeline_properties.renderpass_key = m_current_renderpass_key;
	}

	m_vertex_prog = nullptr;
	m_fragment_prog = nullptr;

	if (shadermode != shader_mode::interpreter_only) [[likely]]
	{
		vk::enter_uninterruptible();

		if (g_cfg.video.debug_overlay)
		{
			m_frame_stats.program_cache_lookups_total += 2;
			if (m_program_cache_hint.has_fragment_program())
			{
				m_frame_stats.program_cache_lookups_ellided++;
			}
			if (m_program_cache_hint.has_vertex_program())
			{
				m_frame_stats.program_cache_lookups_ellided++;
			}
		}

		// Load current program from cache
		std::tie(m_program, m_vertex_prog, m_fragment_prog) = m_prog_buffer->get_graphics_pipeline(
			&m_program_cache_hint,
			vertex_program,
			fragment_program,
			m_pipeline_properties,
			shadermode != shader_mode::recompiler, true);

		vk::leave_uninterruptible();

		if (m_prog_buffer->check_cache_missed())
		{
			// Notify the user with HUD notification
			if (g_cfg.misc.show_shader_compilation_hint)
			{
				if (m_overlay_manager)
				{
					rsx::overlays::show_shader_compile_notification();
				}
			}
		}
	}
	else
	{
		m_program = nullptr;
	}

	if (shadermode == shader_mode::async_with_interpreter || shadermode == shader_mode::interpreter_only)
	{
		const bool is_interpreter = !m_program;
		const bool was_interpreter = m_shader_interpreter.is_interpreter(m_prev_program);

		// First load the next program if not available
		if (!m_program)
		{
			m_program = m_shader_interpreter.get(
				m_pipeline_properties,
				current_fp_metadata,
				current_vp_metadata,
				current_vertex_program.ctrl,
				current_fragment_program.ctrl);

			// Program has changed, reupload
			m_interpreter_state = rsx::invalidate_pipeline_bits;
		}

		// If swapping between interpreter and recompiler, we need to adjust some flags to reupload data as needed.
		if (is_interpreter != was_interpreter)
		{
			// Always reupload transform constants when going between interpreter and recompiler
			m_graphics_state |= rsx::transform_constants_dirty;

			// Always reload fragment constansts when moving from interpreter back to recompiler.
			if (was_interpreter)
			{
				m_graphics_state |= rsx::fragment_constants_dirty;
			}
		}
	}

	if (m_program)
	{
		std::tie(m_vs_binding_table, m_fs_binding_table) = get_binding_table();
#ifdef RPCS3_PS4
		// 0.36: draw programs only; their descriptor pools are released when idle (VKPresent.cpp).
		m_program->ps4_note_draw_program();
#endif
	}
	else
	{
		m_vs_binding_table = nullptr;
		m_fs_binding_table = nullptr;
	}

	return m_program != nullptr;
}

void VKGSRender::load_program_env()
{
	if (!m_program)
	{
		fmt::throw_exception("Unreachable right now");
	}

	const auto& ctx = REGS(m_ctx);
	const auto& gpu_limits = m_device->gpu().get_limits();

	const u32 fragment_constants_size = current_fp_metadata.program_constants_buffer_length;
	const bool is_interpreter = m_shader_interpreter.is_interpreter(m_program);

	const bool update_transform_constants = !!(m_graphics_state & rsx::pipeline_state::transform_constants_dirty);
	const bool update_fragment_constants = !!(m_graphics_state & rsx::pipeline_state::fragment_constants_dirty);
	const bool update_vertex_env = !!(m_graphics_state & rsx::pipeline_state::vertex_state_dirty);
	const bool update_fragment_env = !!(m_graphics_state & rsx::pipeline_state::fragment_state_dirty);
	const bool update_fragment_texture_env = !!(m_graphics_state & rsx::pipeline_state::fragment_texture_state_dirty);
	const bool update_instruction_buffers = (!!m_interpreter_state && is_interpreter);
	const bool update_raster_env = (ctx->polygon_stipple_enabled() && !!(m_graphics_state & rsx::pipeline_state::polygon_stipple_pattern_dirty));
	const bool update_instancing_data = ctx->current_draw_clause.is_trivial_instanced_draw;

	if (update_vertex_env)
	{
		// Vertex state. Note, we're now on std430 alignment here, not hardware alignment.
		// Use the bulk allocator here
		const auto mem = m_vertex_env_allocator->alloc();
		auto buf = m_vertex_env_ring_info.map<char>(mem, 96);

		m_draw_processor.fill_scale_offset_data(buf, false);
		m_draw_processor.fill_user_clip_data(buf + 64);
		*(reinterpret_cast<u32*>(buf + 68)) = ctx->transform_branch_bits();
		*(reinterpret_cast<f32*>(buf + 72)) = ctx->point_size() * resolution_scaling_config.scale_factor();
		*(reinterpret_cast<f32*>(buf + 76)) = ctx->clip_min();
		*(reinterpret_cast<f32*>(buf + 80)) = ctx->clip_max();

		m_vertex_env_ring_info.unmap();
		m_vertex_env_dynamic_offset = mem;

		m_vertex_env_buffer_info = m_vertex_env_ring_info.window<256>(m_vertex_env_dynamic_offset, 96, gpu_limits.maxUniformBufferRange);
		m_vertex_env_dynamic_offset -= m_vertex_env_buffer_info.offset;
	}

	if (update_instancing_data)
	{
		// Combines transform load + instancing lookup table
		const auto alignment = m_device->gpu().get_limits().minStorageBufferOffsetAlignment;
		usz indirection_table_offset = 0;
		usz constants_data_table_offset = 0;

		rsx::io_buffer indirection_table_buf([&](usz size) -> std::pair<void*, usz>
		{
			indirection_table_offset = m_instancing_buffer_ring_info.alloc<1>(utils::align(size, alignment));
			return std::make_pair(m_instancing_buffer_ring_info.map(indirection_table_offset, size), size);
		});

		rsx::io_buffer constants_array_buf([&](usz size) -> std::pair<void*, usz>
		{
			constants_data_table_offset = m_instancing_buffer_ring_info.alloc<1>(utils::align(size, alignment));
			return std::make_pair(m_instancing_buffer_ring_info.map(constants_data_table_offset, size), size);
		});

		const auto bound_vertex_prog = m_shader_interpreter.is_interpreter(m_program) ? nullptr : m_vertex_prog;
		m_draw_processor.fill_constants_instancing_buffer(indirection_table_buf, constants_array_buf, bound_vertex_prog);
		m_instancing_buffer_ring_info.unmap();

		m_instancing_indirection_buffer_info = { *m_instancing_buffer_ring_info.heap, indirection_table_offset, indirection_table_buf.size() };
		m_instancing_constants_array_buffer_info = { *m_instancing_buffer_ring_info.heap, constants_data_table_offset, constants_array_buf.size() };
	}
	else if (update_transform_constants)
	{
		// Transform constants
		usz mem_offset = 0;
		auto alloc_storage = [&](usz size) -> std::pair<void*, usz>
		{
			mem_offset = m_transform_constants_allocator->alloc_bytes(size);
			return std::make_pair(m_transform_constants_ring_info.map(mem_offset, size), size);
		};

		auto io_buf = rsx::io_buffer(alloc_storage);
		upload_transform_constants(io_buf);

		if (!io_buf.empty())
		{
			m_transform_constants_ring_info.unmap();
			m_xform_constants_dynamic_offset = mem_offset;

			m_vertex_constants_buffer_info = m_transform_constants_ring_info.window<16>(m_xform_constants_dynamic_offset, io_buf.size(), gpu_limits.maxUniformBufferRange);
			m_xform_constants_dynamic_offset -= m_vertex_constants_buffer_info.offset;
		}
	}

	if (update_fragment_constants && !m_shader_interpreter.is_interpreter(m_program))
	{
		// Fragment constants
		if (fragment_constants_size)
		{
			m_fragment_constants_dynamic_offset = m_fragment_constants_allocator->alloc_bytes(fragment_constants_size);
			auto buf = m_fragment_constants_ring_info.map(m_fragment_constants_dynamic_offset, fragment_constants_size);

			m_prog_buffer->fill_fragment_constants_buffer({ reinterpret_cast<float*>(buf), fragment_constants_size },
				*ensure(m_fragment_prog), current_fragment_program, true);

			m_fragment_constants_ring_info.unmap();

			m_fragment_constants_buffer_info = m_fragment_constants_ring_info.window<16>(m_fragment_constants_dynamic_offset, fragment_constants_size, gpu_limits.maxUniformBufferRange);
			m_fragment_constants_dynamic_offset -= m_fragment_constants_buffer_info.offset;
		}
	}

	if (update_fragment_env)
	{
		m_fragment_env_dynamic_offset = m_fragment_env_ring_info.static_alloc<32>();
		auto buf = m_fragment_env_ring_info.map(m_fragment_env_dynamic_offset, 32);

		m_draw_processor.fill_fragment_state_buffer(buf, current_fragment_program);
		m_fragment_env_ring_info.unmap();

		m_fragment_env_buffer_info = m_fragment_env_ring_info.window<32>(m_fragment_env_dynamic_offset, 32, gpu_limits.maxUniformBufferRange);
		m_fragment_env_dynamic_offset -= m_fragment_env_buffer_info.offset;
	}

	if (update_fragment_texture_env)
	{
		m_texture_parameters_dynamic_offset = m_fragment_texture_params_ring_info.static_alloc<256, 768>();
		auto buf = m_fragment_texture_params_ring_info.map(m_texture_parameters_dynamic_offset, 768);

		current_fragment_program.texture_params.write_to(buf, current_fp_metadata.referenced_textures_mask);
		m_fragment_texture_params_ring_info.unmap();

		m_fragment_texture_params_buffer_info = m_fragment_texture_params_ring_info.window<768>(m_texture_parameters_dynamic_offset, 768, gpu_limits.maxUniformBufferRange);
		m_texture_parameters_dynamic_offset -= m_fragment_texture_params_buffer_info.offset;
	}

	if (update_raster_env)
	{
		m_stipple_array_dynamic_offset = m_raster_env_ring_info.static_alloc<128>();
		auto buf = m_raster_env_ring_info.map(m_stipple_array_dynamic_offset, 128);

		std::memcpy(buf, ctx->polygon_stipple_pattern(), 128);
		m_raster_env_ring_info.unmap();

		m_graphics_state.clear(rsx::pipeline_state::polygon_stipple_pattern_dirty);
	}

	if (update_instruction_buffers)
	{
		if (m_interpreter_state & rsx::vertex_program_dirty)
		{
			// Attach vertex buffer data
			const auto vp_block_length = current_vp_metadata.ucode_length + 16;
			auto vp_mapping = m_vertex_instructions_buffer.alloc<256>(vp_block_length);
			auto vp_buf = static_cast<u8*>(m_vertex_instructions_buffer.map(vp_mapping, vp_block_length));

			auto vp_config = reinterpret_cast<u32*>(vp_buf);
			vp_config[0] = current_vertex_program.base_address;
			vp_config[1] = current_vertex_program.entry;
			vp_config[2] = current_vertex_program.output_mask;
			vp_config[3] = ctx->two_side_light_en()? 1u: 0u;

			std::memcpy(vp_buf + 16, current_vertex_program.data.data(), current_vp_metadata.ucode_length);
			m_vertex_instructions_buffer.unmap();

			m_vertex_instructions_buffer_info = { *m_vertex_instructions_buffer.heap, vp_mapping, vp_block_length };
		}

		if (m_interpreter_state & rsx::fragment_program_dirty)
		{
			// Attach fragment buffer data
			const auto fp_block_length = current_fp_metadata.program_ucode_length + 16;
			auto fp_mapping = m_fragment_instructions_buffer.alloc<256>(fp_block_length);
			auto fp_buf = static_cast<u8*>(m_fragment_instructions_buffer.map(fp_mapping, fp_block_length));

			// Control mask
			const auto control_masks = reinterpret_cast<u32*>(fp_buf);
			control_masks[0] = ctx->shader_control();
			control_masks[1] = current_fragment_program.texture_state.texture_dimensions;

			std::memcpy(fp_buf + 16, current_fragment_program.get_data(), current_fragment_program.ucode_length);
			m_fragment_instructions_buffer.unmap();

			m_fragment_instructions_buffer_info = { *m_fragment_instructions_buffer.heap, fp_mapping, fp_block_length };
		}
	}

	m_program->bind_uniform(m_vertex_env_buffer_info, vk::glsl::binding_set_index_vertex, m_vs_binding_table->context_buffer_location);
	m_program->bind_uniform(m_vertex_layout_stream_info, vk::glsl::binding_set_index_vertex, m_vs_binding_table->vertex_buffers_location + 2);
	m_program->bind_uniform(m_fragment_env_buffer_info, vk::glsl::binding_set_index_fragment, m_fs_binding_table->context_buffer_location);
	m_program->bind_uniform(m_fragment_texture_params_buffer_info, vk::glsl::binding_set_index_fragment, m_fs_binding_table->tex_param_location);
	m_program->bind_uniform(m_raster_env_buffer_info, vk::glsl::binding_set_index_fragment, m_fs_binding_table->polygon_stipple_params_location);

	if (m_vs_binding_table->cbuf_location != umax)
	{
		m_program->bind_uniform(m_vertex_constants_buffer_info, vk::glsl::binding_set_index_vertex, m_vs_binding_table->cbuf_location);
	}

	if (m_shader_interpreter.is_interpreter(m_program))
	{
		m_program->bind_uniform(m_vertex_instructions_buffer_info, vk::glsl::binding_set_index_vertex, m_shader_interpreter.get_vertex_instruction_location());
		m_program->bind_uniform(m_fragment_instructions_buffer_info, vk::glsl::binding_set_index_fragment, m_shader_interpreter.get_fragment_instruction_location());
	}
	else if (m_fs_binding_table->cbuf_location != umax)
	{
		m_program->bind_uniform(m_fragment_constants_buffer_info, vk::glsl::binding_set_index_fragment, m_fs_binding_table->cbuf_location);
	}

	if (vk::emulate_conditional_rendering())
	{
		const vk::buffer& predicate = m_cond_render_buffer ? *m_cond_render_buffer : *vk::get_scratch_buffer(*m_current_command_buffer, 4, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_ACCESS_NONE);
		const u32 offset = cond_render_ctrl.hw_cond_active ? 0 : 4;
		m_program->bind_uniform({ predicate, offset, 4 }, vk::glsl::binding_set_index_vertex, m_vs_binding_table->cr_pred_buffer_location);
	}

	if (current_vertex_program.ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS)
	{
		m_program->bind_uniform(m_instancing_indirection_buffer_info, vk::glsl::binding_set_index_vertex, m_vs_binding_table->instanced_lut_buffer_location);
		m_program->bind_uniform(m_instancing_constants_array_buffer_info, vk::glsl::binding_set_index_vertex, m_vs_binding_table->instanced_cbuf_location);
	}

	// Clear flags
	rsx::flags32_t handled_flags =
		rsx::pipeline_state::fragment_state_dirty |
		rsx::pipeline_state::vertex_state_dirty |
		rsx::pipeline_state::fragment_texture_state_dirty;

	if (!update_instancing_data)
	{
		handled_flags |= rsx::pipeline_state::transform_constants_dirty;
	}

	if (update_fragment_constants && !m_shader_interpreter.is_interpreter(m_program))
	{
		handled_flags |= rsx::pipeline_state::fragment_constants_dirty;
	}

	if (m_shader_interpreter.is_interpreter(m_program))
	{
		ensure(m_vertex_constants_buffer_info.range >= 468 * 16);
	}

	m_graphics_state.clear(handled_flags);
}

std::pair<const vs_binding_table_t*, const fs_binding_table_t*> VKGSRender::get_binding_table() const
{
	ensure(m_program);

	if (!m_shader_interpreter.is_interpreter(m_program))
	{
		return { &m_vertex_prog->binding_table, &m_fragment_prog->binding_table };
	}

	const auto& [vs, fs] = m_shader_interpreter.get_shaders();
	ensure(vs && fs, "Invalid interpreter configuration");
	return { &vs->binding_table, &fs->binding_table };
}

bool VKGSRender::is_current_program_interpreted() const
{
	return m_program && m_shader_interpreter.is_interpreter(m_program);
}

void VKGSRender::upload_transform_constants(const rsx::io_buffer& buffer)
{
	const bool is_interpreter = m_shader_interpreter.is_interpreter(m_program);
	const usz transform_constants_size = (is_interpreter || m_vertex_prog->has_indexed_constants) ? 8192 : m_vertex_prog->constant_ids.size() * 16;

	if (transform_constants_size)
	{
#ifdef RPCS3_PS4
		// 0.37: 'xform KiB/frame' of the 10 s 'Vulkan backend' line (full 8 KiB block or the constants the program uses).
		auto& ps4_counters = vk::g_ps4_vk_draw_counters;
		(transform_constants_size == 8192 ? ps4_counters.xform_full_bytes : ps4_counters.xform_reloc_bytes) += transform_constants_size;
		ps4_counters.xform_uploads++;
#endif
		buffer.reserve(transform_constants_size);
		auto buf = buffer.data();

		const auto constant_ids = (transform_constants_size == 8192)
			? std::span<const u16>{}
			: std::span<const u16>(m_vertex_prog->constant_ids);
		m_draw_processor.fill_vertex_program_constants_data(buf, constant_ids);
	}
}

void VKGSRender::update_vertex_env(u32 id, const vk::vertex_upload_info& vertex_info)
{
#pragma pack(push, 1)
	struct rsx_prog_vertex_layout_entry_t
	{
		u32 vertex_base_index;
		u32 vertex_index_offset;
		u32 draw_id;
		u32 xform_constants_offset;
		u32 vs_context_offset;
		u32 fs_constants_offset;
		u32 fs_context_offset;
		u32 fs_texture_base_index;
		u32 fs_stipple_pattern_offset;
		u32 reserved;
		s32 attrib_data[1];
	};
#pragma pack(pop)

	// Actual allocation must have been done previously
	const u32 vs_constant_id_offset = static_cast<u32>(m_xform_constants_dynamic_offset) / 16u;
	const u32 vertex_context_offset = static_cast<u32>(m_vertex_env_dynamic_offset) / 96u;
	const u32 vertex_layout_offset = static_cast<u32>(m_vertex_layout_dynamic_offset) / 168u;
	const u32 fs_constant_id_offset = static_cast<u32>(m_fragment_constants_dynamic_offset) / 16u;
	const u32 fs_context_offset = static_cast<u32>(m_fragment_env_dynamic_offset) / 32u;
	const u32 fs_texture_base_index = static_cast<u32>(m_texture_parameters_dynamic_offset) / 48u;
	const u32 fs_stipple_pattern_offset = static_cast<u32>(m_stipple_array_dynamic_offset) / 16u;

	auto buf = m_vertex_layout_ring_info.map(m_vertex_layout_dynamic_offset + (168u * id), 168u);
	auto dst = reinterpret_cast<rsx_prog_vertex_layout_entry_t*>(buf);

	// Pack
	dst->vertex_base_index = vertex_info.vertex_index_base;
	dst->vertex_index_offset = vertex_info.vertex_index_offset;
	dst->draw_id = id;

	dst->xform_constants_offset = vs_constant_id_offset;
	dst->vs_context_offset = vertex_context_offset;

	dst->fs_constants_offset = fs_constant_id_offset;
	dst->fs_context_offset = fs_context_offset;
	dst->fs_texture_base_index = fs_texture_base_index;
	dst->fs_stipple_pattern_offset = fs_stipple_pattern_offset;

	const u32 push_val = vertex_layout_offset + id;
	vkCmdPushConstants(
		*m_current_command_buffer,
		m_program->layout(),
		VK_SHADER_STAGE_VERTEX_BIT,
		0,
		4,
		&push_val);

	if (current_fragment_program.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING)
	{
		// TODO: This should be cached aggressively.
		u32 blend_config[7];
		blend_config[0] = REGS(m_ctx)->registers[NV4097_SET_BLEND_EQUATION];
		blend_config[1] = REGS(m_ctx)->registers[NV4097_SET_BLEND_FUNC_SFACTOR];
		blend_config[2] = REGS(m_ctx)->registers[NV4097_SET_BLEND_FUNC_DFACTOR];

		const auto blend_colors = rsx::get_constant_blend_colors();
		blend_config[3] = std::bit_cast<u32>(blend_colors[0]);
		blend_config[4] = std::bit_cast<u32>(blend_colors[1]);
		blend_config[5] = std::bit_cast<u32>(blend_colors[2]);
		blend_config[6] = std::bit_cast<u32>(blend_colors[3]);

		vkCmdPushConstants(
			*m_current_command_buffer,
			m_program->layout(),
			VK_SHADER_STAGE_FRAGMENT_BIT,
			4,
			28,
			blend_config);
	}

	// Now actually fill in the data
	m_draw_processor.fill_vertex_layout_state(
		m_vertex_layout,
		current_vp_metadata,
		vertex_info.first_vertex,
		vertex_info.allocated_vertex_count,
		dst->attrib_data,
		vertex_info.persistent_window_offset,
		vertex_info.volatile_window_offset);

	m_vertex_layout_ring_info.unmap();
}

void VKGSRender::patch_transform_constants(rsx::context* /*ctx*/, u32 index, u32 count)
{
#ifdef RPCS3_PS4
	// With the shader interpreter bound, m_vertex_prog is the program that is not ready yet, whose metadata a pipeline
	// worker may still be writing: no read of it. The interpreter uploads the whole 8 KiB block at the next draw.
	if (!m_program || !m_vertex_prog || m_shader_interpreter.is_interpreter(m_program))
#else
	if (!m_program || !m_vertex_prog)
#endif
	{
		// Shouldn't be reachable, but handle it correctly anyway
		m_graphics_state |= rsx::pipeline_state::transform_constants_dirty;
		return;
	}

	if (!m_vertex_prog->overlaps_constants_range(index, count))
	{
		// Nothing meaningful to us
		return;
	}

	// Buffer updates mid-pass violate the spec and destroy performance on NVIDIA
	auto allocate_mem = [&](usz size) -> std::pair<void*, usz>
	{
		const usz alignment = m_device->gpu().get_limits().minStorageBufferOffsetAlignment;
		m_xform_constants_dynamic_offset = m_transform_constants_ring_info.alloc<1>(utils::align(size, alignment));
		return std::make_pair(m_transform_constants_ring_info.map(m_xform_constants_dynamic_offset, size), size);
	};

	rsx::io_buffer iobuf(allocate_mem);
	upload_transform_constants(iobuf);

	if (!iobuf.empty())
	{
		m_transform_constants_ring_info.unmap();
	}
}

void VKGSRender::init_buffers(rsx::framebuffer_creation_context context, bool)
{
	prepare_rtts(context);
}

void VKGSRender::close_and_submit_command_buffer(vk::fence* pFence, VkSemaphore wait_semaphore, VkSemaphore signal_semaphore, VkPipelineStageFlags pipeline_stage_flags)
{
	ensure(!m_queue_status.test_and_set(flush_queue_state::flushing));

#ifdef RPCS3_PS4
	// Submits per frame in the 10 s line; the command buffer split counts draws from here.
	vk::g_ps4_vk_draw_counters.submits++;
	m_ps4_cb_draws = 0;
#endif

	// Host MM sync before executing anything on the GPU
	rsx::mm_flush();

	// Workaround for deadlock occuring during RSX offloader fault
	// TODO: Restructure command submission infrastructure to avoid this condition
	const bool sync_success = g_fxo->get<rsx::dma_manager>().sync();
	const VkBool32 force_flush = !sync_success;

	if (vk::test_status_interrupt(vk::heap_dirty))
	{
		if (const auto dirty_list = m_flushable_data_heaps.filter(FN(x->is_dirty()));
			!dirty_list.empty())
		{
			auto secondary_command_buffer = m_secondary_cb_list.next();
			secondary_command_buffer->begin();

			for (auto& heap : dirty_list)
			{
				heap->sync(*secondary_command_buffer);
			}

			secondary_command_buffer->end();

			vk::queue_submit_t submit_info{ m_device->get_graphics_queue(), nullptr };
			secondary_command_buffer->submit(submit_info, force_flush);
		}

		vk::clear_status_interrupt(vk::heap_dirty);
	}

#if 0 // Currently unreachable
	if (m_current_command_buffer->flags & vk::command_buffer::cb_has_conditional_render)
	{
		ensure(m_render_pass_open);
		_vkCmdEndConditionalRenderingEXT(*m_current_command_buffer);
	}
#endif

	// End any active renderpasses; the caller should handle reopening
	if (vk::is_renderpass_open(*m_current_command_buffer))
	{
		close_render_pass();
	}

	// End open queries. Flags will be automatically reset by the submit routine
	if (m_current_command_buffer->flags & vk::command_buffer::cb_has_open_query)
	{
		auto open_query = m_occlusion_map[m_active_query_info->driver_handle].indices.back();
		m_occlusion_query_manager->end_query(*m_current_command_buffer, open_query);
		m_current_command_buffer->flags &= ~vk::command_buffer::cb_has_open_query;
	}

	if (m_host_dma_ctrl && m_host_dma_ctrl->host_ctx()->needs_label_release())
	{
		vkCmdUpdateBuffer(*m_current_command_buffer,
			m_host_object_data->value,
			::offset32(&vk::host_data_t::commands_complete_event),
			sizeof(u64),
			const_cast<u64*>(&m_host_dma_ctrl->host_ctx()->last_label_acquire_event));

		m_host_dma_ctrl->host_ctx()->on_label_release();
	}

	m_current_command_buffer->end();
	m_current_command_buffer->tag();

	// Supporting concurrent access vastly simplifies this logic.
	// Instead of doing CB slice injection, we can just chain these together logically with the async stream going first
	vk::queue_submit_t primary_submit_info{ m_device->get_graphics_queue(), pFence };
	vk::queue_submit_t secondary_submit_info{};

	if (wait_semaphore)
	{
		primary_submit_info.wait_on(wait_semaphore, pipeline_stage_flags);
	}

	if (auto async_scheduler = g_fxo->try_get<vk::AsyncTaskScheduler>();
		async_scheduler && async_scheduler->is_recording())
	{
		if (async_scheduler->is_host_mode())
		{
			const VkSemaphore async_sema = *async_scheduler->get_sema();
			secondary_submit_info.queue_signal(async_sema);
			primary_submit_info.wait_on(async_sema, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

			// Delay object destruction by one cycle
			vk::get_resource_manager()->push_down_current_scope();
		}

		async_scheduler->flush(secondary_submit_info, force_flush);
	}

	if (signal_semaphore)
	{
		primary_submit_info.queue_signal(signal_semaphore);
	}

	m_current_command_buffer->submit(primary_submit_info, force_flush);

	m_queue_status.clear(flush_queue_state::flushing);
}

void VKGSRender::prepare_rtts(rsx::framebuffer_creation_context context)
{
	const bool clipped_scissor = (context == rsx::framebuffer_creation_context::context_draw);
	if (m_current_framebuffer_context == context && !m_graphics_state.test(rsx::rtt_config_dirty) && m_draw_fbo)
	{
		// Fast path
		// Framebuffer usage has not changed, framebuffer exists and config regs have not changed
		set_scissor(clipped_scissor);
		return;
	}

	m_graphics_state.clear(
		rsx::rtt_config_dirty |
		rsx::rtt_config_contested |
		rsx::rtt_config_valid |
		rsx::rtt_cache_state_dirty);

	get_framebuffer_layout(context, m_framebuffer_layout);
	if (!m_graphics_state.test(rsx::rtt_config_valid))
	{
		return;
	}

	if (m_draw_fbo && m_framebuffer_layout.ignore_change)
	{
		// Nothing has changed, we're still using the same framebuffer
		// Update flags to match current
		set_scissor(clipped_scissor);
		return;
	}

	m_rtts.prepare_render_target(*m_current_command_buffer,
		m_framebuffer_layout.color_format, m_framebuffer_layout.depth_format,
		m_framebuffer_layout.width, m_framebuffer_layout.height,
		m_framebuffer_layout.target, m_framebuffer_layout.aa_mode, m_framebuffer_layout.raster_type,
		m_framebuffer_layout.color_addresses, m_framebuffer_layout.zeta_address,
		m_framebuffer_layout.actual_color_pitch, m_framebuffer_layout.actual_zeta_pitch,
		resolution_scaling_config);

	// Reset framebuffer information
	const auto color_bpp = get_format_block_size_in_bytes(m_framebuffer_layout.color_format);
	const auto samples = get_format_sample_count(m_framebuffer_layout.aa_mode);

	for (u8 i = 0; i < rsx::limits::color_buffers_count; ++i)
	{
		// Flush old address if we keep missing it
		if (m_surface_info[i].pitch && g_cfg.video.write_color_buffers)
		{
			const utils::address_range32 rsx_range = m_surface_info[i].get_memory_range();
			m_texture_cache.set_memory_read_flags(rsx_range, rsx::memory_read_flags::flush_once);
#ifdef RPCS3_PS4
			// 0.32: a speculative readback would be appended to the frame's command buffer; only with dma.flag
			// speculate=on. The CPU read it predicts is still served by the regular access-violation flush.
			if (vk::ps4_dma_speculate())
#endif
			m_texture_cache.flush_if_cache_miss_likely(*m_current_command_buffer, rsx_range);
		}

		m_surface_info[i].address = m_surface_info[i].pitch = 0;
		m_surface_info[i].width = m_framebuffer_layout.width;
		m_surface_info[i].height = m_framebuffer_layout.height;
		m_surface_info[i].color_format = m_framebuffer_layout.color_format;
		m_surface_info[i].bpp = color_bpp;
		m_surface_info[i].samples = samples;
	}

	// Process depth surface as well
	{
		if (m_depth_surface_info.pitch && g_cfg.video.write_depth_buffer)
		{
			const utils::address_range32 surface_range = m_depth_surface_info.get_memory_range();
			m_texture_cache.set_memory_read_flags(surface_range, rsx::memory_read_flags::flush_once);
#ifdef RPCS3_PS4
			// 0.32: see the colour buffers above.
			if (vk::ps4_dma_speculate())
#endif
			m_texture_cache.flush_if_cache_miss_likely(*m_current_command_buffer, surface_range);
		}

		m_depth_surface_info.address = m_depth_surface_info.pitch = 0;
		m_depth_surface_info.width = m_framebuffer_layout.width;
		m_depth_surface_info.height = m_framebuffer_layout.height;
		m_depth_surface_info.depth_format = m_framebuffer_layout.depth_format;
		m_depth_surface_info.bpp = get_format_block_size_in_bytes(m_framebuffer_layout.depth_format);
		m_depth_surface_info.samples = samples;
	}

	// Bind created rtts as current fbo...
	const auto draw_buffers = rsx::utility::get_rtt_indexes(m_framebuffer_layout.target);
	m_draw_buffers.clear();
	m_fbo_images.clear();

	for (u8 index : draw_buffers)
	{
		if (auto surface = std::get<1>(m_rtts.m_bound_render_targets[index]))
		{
			m_fbo_images.push_back(surface);

			m_surface_info[index].address = m_framebuffer_layout.color_addresses[index];
			m_surface_info[index].pitch = m_framebuffer_layout.actual_color_pitch[index];
			ensure(surface->rsx_pitch == m_framebuffer_layout.actual_color_pitch[index]);

			m_texture_cache.notify_surface_changed(m_surface_info[index].get_memory_range(m_framebuffer_layout.aa_factors));
			m_draw_buffers.push_back(index);
		}
	}

	if (std::get<0>(m_rtts.m_bound_depth_stencil) != 0)
	{
		auto ds = std::get<1>(m_rtts.m_bound_depth_stencil);
		m_fbo_images.push_back(ds);

		m_depth_surface_info.address = m_framebuffer_layout.zeta_address;
		m_depth_surface_info.pitch = m_framebuffer_layout.actual_zeta_pitch;
		ensure(ds->rsx_pitch == m_framebuffer_layout.actual_zeta_pitch);

		m_texture_cache.notify_surface_changed(m_depth_surface_info.get_memory_range(m_framebuffer_layout.aa_factors));
	}

	// Before messing with memory properties, flush command queue if there are dma transfers queued up
	if (m_current_command_buffer->flags & vk::command_buffer::cb_has_dma_transfer)
	{
		flush_command_queue();
	}

	if (!m_rtts.superseded_surfaces.empty())
	{
		for (auto& surface : m_rtts.superseded_surfaces)
		{
			m_texture_cache.discard_framebuffer_memory_region(*m_current_command_buffer, surface->get_memory_range());
		}

		m_rtts.superseded_surfaces.clear();
	}

	if (!m_rtts.orphaned_surfaces.empty())
	{
		for (auto& [base_addr, surface] : m_rtts.orphaned_surfaces)
		{
			bool lock = surface->is_depth_surface() ? !!g_cfg.video.write_depth_buffer :
				!!g_cfg.video.write_color_buffers;

			if (lock &&
#ifdef TEXTURE_CACHE_DEBUG
				!m_texture_cache.is_protected(
					base_addr,
					surface->get_memory_range(),
					rsx::texture_upload_context::framebuffer_storage)
#else
				!surface->is_locked()
#endif
				)
			{
				lock = false;
			}

			if (!lock) [[likely]]
			{
				m_texture_cache.commit_framebuffer_memory_region(*m_current_command_buffer, surface->get_memory_range());
				continue;
			}

			m_texture_cache.lock_memory_region(
				*m_current_command_buffer, surface, surface->get_memory_range(), false,
				surface->get_surface_width<rsx::surface_metrics::pixels>(), surface->get_surface_height<rsx::surface_metrics::pixels>(), surface->get_rsx_pitch(),
				surface);
		}

		m_rtts.orphaned_surfaces.clear();
	}

	for (u8 index : m_draw_buffers)
	{
		if (!m_surface_info[index].address || !m_surface_info[index].pitch) continue;

		const utils::address_range32 surface_range = m_surface_info[index].get_memory_range();
		if (g_cfg.video.write_color_buffers)
		{
			m_texture_cache.lock_memory_region(
				*m_current_command_buffer, m_rtts.m_bound_render_targets[index].second, surface_range, true,
				m_surface_info[index].width, m_surface_info[index].height, m_framebuffer_layout.actual_color_pitch[index],
				m_rtts.m_bound_render_targets[index].second);
		}
		else
		{
			m_texture_cache.commit_framebuffer_memory_region(*m_current_command_buffer, surface_range);
		}
	}

	if (m_depth_surface_info.address && m_depth_surface_info.pitch)
	{
		const utils::address_range32 surface_range = m_depth_surface_info.get_memory_range();
		if (g_cfg.video.write_depth_buffer)
		{
			m_texture_cache.lock_memory_region(
				*m_current_command_buffer, m_rtts.m_bound_depth_stencil.second, surface_range, true,
				m_depth_surface_info.width, m_depth_surface_info.height, m_framebuffer_layout.actual_zeta_pitch,
				m_rtts.m_bound_depth_stencil.second);
		}
		else
		{
			m_texture_cache.commit_framebuffer_memory_region(*m_current_command_buffer, surface_range);
		}
	}

	std::vector<u8> input_attachments{};
	if ((current_fragment_program.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING) &&
		!m_graphics_state.test(rsx::pipeline_state::fragment_program_state_dirty))
	{
		input_attachments.resize(m_draw_buffers.size());
		std::iota(input_attachments.begin(), input_attachments.end(), 0);
	}

	m_current_renderpass_key = vk::get_renderpass_key(m_fbo_images, input_attachments);
	m_cached_renderpass = vk::get_renderpass(*m_device, m_current_renderpass_key);

	// Search old framebuffers for this same configuration
	const auto [fbo_width, fbo_height] = rsx::apply_resolution_scale<true>(resolution_scaling_config, m_framebuffer_layout.width, m_framebuffer_layout.height);

	if (m_draw_fbo)
	{
		// Release old ref
		m_draw_fbo->release();
	}

	m_draw_fbo = vk::get_framebuffer(*m_device, fbo_width, fbo_height, vk::to_bool32(!input_attachments.empty()), m_cached_renderpass, m_fbo_images);
	m_draw_fbo->add_ref();

	set_viewport();
	set_scissor(clipped_scissor);
	on_framebuffer_layout_updated();

	check_zcull_status(true);
}

void VKGSRender::renderctl(u32 request_code, void* args)
{
	switch (request_code)
	{
	case vk::rctrl_queue_submit:
	{
		const auto packet = reinterpret_cast<vk::queue_submit_t*>(args);
		vk::queue_submit(packet);
		free(packet);
		break;
	}
	default:
		rsx::thread::renderctl(request_code, args);
	}
}

bool VKGSRender::scaled_image_from_memory(const rsx::blit_src_info& src, const rsx::blit_dst_info& dst, bool interpolate)
{
	if (swapchain_unavailable)
		return false;

#ifdef RPCS3_PS4
	// 0.36: render passes closed by a blit count as texture copies.
	const utils::ps4::gpu::rp_close_cause_scope ps4_cause(rsx::ps4_rp_close_texture_copy);
#endif
	if (m_texture_cache.blit(src, dst, interpolate, m_rtts, *m_current_command_buffer))
	{
		m_samplers_dirty.store(true);
		m_current_command_buffer->set_flag(vk::command_buffer::cb_has_blit_transfer);

		if (m_current_command_buffer->flags & vk::command_buffer::cb_has_dma_transfer)
		{
			// A dma transfer has been queued onto this cb
			// This likely means that we're done with the tranfers to the target (writes_likely_completed=1)
			flush_command_queue();
		}
		return true;
	}

	return false;
}

void VKGSRender::begin_occlusion_query(rsx::reports::occlusion_query_info* query)
{
	ensure(!m_occlusion_query_active);

	query->result = 0;
	//query->sync_timestamp = get_system_time();
	m_active_query_info = query;
	m_occlusion_query_active = true;
	m_current_command_buffer->flags |= vk::command_buffer::cb_load_occluson_task;
}

void VKGSRender::end_occlusion_query(rsx::reports::occlusion_query_info* query)
{
	ensure(query == m_active_query_info);

	// NOTE: flushing the queue is very expensive, do not flush just because query stopped
	if (m_current_command_buffer->flags & vk::command_buffer::cb_has_open_query)
	{
		// VK_1_0 rules dictate that query must match subpass behavior on begin/end query.
		// This is slow, so only do this for drivers that care.
		if (vk::use_strict_query_scopes() &&
			vk::is_renderpass_open(*m_current_command_buffer))
		{
#ifdef RPCS3_PS4
			const utils::ps4::gpu::rp_close_cause_scope ps4_cause(rsx::ps4_rp_close_query);
#endif
			vk::end_renderpass(*m_current_command_buffer);
		}

		// End query
		auto open_query = m_occlusion_map[m_active_query_info->driver_handle].indices.back();
		m_occlusion_query_manager->end_query(*m_current_command_buffer, open_query);
		m_current_command_buffer->flags &= ~vk::command_buffer::cb_has_open_query;
	}

	// Clear occlusion load flag
	m_current_command_buffer->flags &= ~vk::command_buffer::cb_load_occluson_task;

	m_occlusion_query_active = false;
	m_active_query_info = nullptr;
}

bool VKGSRender::check_occlusion_query_status(rsx::reports::occlusion_query_info* query)
{
	if (!query->num_draws)
		return true;

	auto &data = m_occlusion_map[query->driver_handle];
	if (data.indices.empty())
		return true;

	if (data.is_current(m_current_command_buffer))
		return false;

	const u32 oldest = data.indices.front();
	return m_occlusion_query_manager->check_query_status(oldest);
}

void VKGSRender::get_occlusion_query_result(rsx::reports::occlusion_query_info* query)
{
	auto &data = m_occlusion_map[query->driver_handle];
	if (data.indices.empty())
		return;

	if (query->num_draws)
	{
#ifdef RPCS3_PS4
		// 0.29:ZCULL/occlusion readback (hard sync + CB fence wait) as the STALL wait reason.
		const rsx::ps4_rsx_wait_scope ps4_wait(rsx::ps4_wait_zcull);
#endif
		if (data.is_current(m_current_command_buffer))
		{
			std::lock_guard lock(m_flush_queue_mutex);
			flush_command_queue();

			if (m_flush_requests.pending())
			{
				m_flush_requests.clear_pending_flag();
			}

			rsx_log.warning("[Performance warning] Unexpected ZCULL read caused a hard sync");
			busy_wait();
		}

		data.sync();

#ifdef RPCS3_PS4
		// 0.39: a result the query pool gave up on after 2 s (counted there) is classified here: sync() waits only
		// for the deferred submit, not for the GPU, so the command buffer may still be running.
		const u32 ps4_query_timeouts = utils::ps4::gpu::query_timeouts();
#endif

		// Gather data
		for (const auto occlusion_id : data.indices)
		{
			query->result += m_occlusion_query_manager->get_query_result(occlusion_id);
			if (query->result && !g_cfg.video.precise_zpass_count)
			{
				// We only need one hit unless precise zcull is requested
				break;
			}
		}

#ifdef RPCS3_PS4
		static std::atomic<u32> s_ps4_query_timeout_notes{0};
		if (utils::ps4::gpu::query_timeouts() != ps4_query_timeouts && s_ps4_query_timeout_notes.fetch_add(1, std::memory_order_relaxed) < 4)
		{
			auto* cb = data.command_buffer_to_wait;
			const char* state = cb->ps4_submitted_unsignaled() ? "is still running on the GPU"
				: cb->reset_id == data.command_buffer_sync_id ? "has signalled but the result is missing"
				: "was recycled before the read";
			const std::string line = fmt::format("PS4 GPU: occlusion query timed out: its command buffer %s (sync id %llu, buffer reset id %llu, %u queued frames)",
				state, data.command_buffer_sync_id, cb->reset_id, ::size32(m_queued_frames));
			rsx_log.error("%s", line);
			ps4_startup_checkpoint(line.c_str());
		}
#endif
	}

	m_occlusion_query_manager->free_queries(*m_current_command_buffer, data.indices);
	data.indices.clear();
}

void VKGSRender::discard_occlusion_query(rsx::reports::occlusion_query_info* query)
{
	if (m_active_query_info == query)
	{
		end_occlusion_query(query);
	}

	auto &data = m_occlusion_map[query->driver_handle];
	if (data.indices.empty())
		return;

	m_occlusion_query_manager->free_queries(*m_current_command_buffer, data.indices);
	data.indices.clear();
}

void VKGSRender::emergency_query_cleanup(vk::command_buffer* commands)
{
	ensure(commands == static_cast<vk::command_buffer*>(m_current_command_buffer));

	if (m_current_command_buffer->flags & vk::command_buffer::cb_has_open_query)
	{
		auto open_query = m_occlusion_map[m_active_query_info->driver_handle].indices.back();
		m_occlusion_query_manager->end_query(*m_current_command_buffer, open_query);
		m_current_command_buffer->flags &= ~vk::command_buffer::cb_has_open_query;
	}
}

void VKGSRender::begin_conditional_rendering(const std::vector<rsx::reports::occlusion_query_info*>& sources)
{
	ensure(!sources.empty());

	// Flag check whether to calculate all entries or only one
	bool partial_eval;

	// Try and avoid regenerating the data if its a repeat/spam
	// NOTE: The incoming list is reversed with the first entry being the newest
	if (m_cond_render_sync_tag == sources.front()->sync_tag)
	{
		// Already synched, check subdraw which is possible if last sync happened while query was active
		if (!m_active_query_info || m_active_query_info != sources.front())
		{
			rsx::thread::begin_conditional_rendering(sources);
			return;
		}

		// Partial evaluation only
		partial_eval = true;
	}
	else
	{
		m_cond_render_sync_tag = sources.front()->sync_tag;
		partial_eval = false;
	}

	// Time to aggregate
	if (!m_cond_render_buffer)
	{
		auto& memory_props = m_device->get_memory_mapping();
		auto usage_flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

		if (m_device->get_conditional_render_support())
		{
			usage_flags |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
		}

		m_cond_render_buffer = std::make_unique<vk::buffer>(
			*m_device, 8,
			memory_props.device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			usage_flags, 0, VMM_ALLOCATION_POOL_UNDEFINED);

		// Fill CR+4 with all-ones. Instead of dynamic flags, just bind the all-ones word to disable CR
		vkCmdFillBuffer(*m_current_command_buffer, m_cond_render_buffer->value, 4, 4, 0xFFFFFFFFu);

		// Ensure the all-ones mask is written before next VS invocation
		vk::insert_buffer_memory_barrier(*m_current_command_buffer, m_cond_render_buffer->value, 4, 4,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	}

	VkPipelineStageFlags dst_stage;
	VkAccessFlags dst_access;
	u32 dst_offset = 0;
	u32 num_hw_queries = 0;
	usz first = 0;
	usz last = (!partial_eval) ? sources.size() : 1;

	// Count number of queries available. This is an "opening" evaluation, if there is only one source, read it as-is.
	// The idea is to avoid scheduling a compute task unless we have to.
	for (usz i = first; i < last; ++i)
	{
		auto& query_info = m_occlusion_map[sources[i]->driver_handle];
		num_hw_queries += ::size32(query_info.indices);
	}

	if (m_device->get_conditional_render_support())
	{
		dst_stage = VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT;
		dst_access = VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT;
	}
	else
	{
		dst_stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
		dst_access = VK_ACCESS_SHADER_READ_BIT;
	}

#ifdef RPCS3_PS4
	// zcull_skip_visible: a source query holding a draw skipped while its program compiled misses samples, and with
	// no host data at all the predicate would keep a stale value. Both cases draw (visible), the conservative answer.
	// Without an occlusion query pool (its creation failed) there is never host data: visible too.
	bool ps4_any_incomplete = false;
	for (usz i = first; i < last; ++i)
	{
		ps4_any_incomplete = ps4_any_incomplete || sources[i]->ps4_incomplete;
	}

	// A partial evaluation only adds the active query to the value the full one (same sync tag) wrote for every
	// source: there, only a skipped draw of the active query forces the predicate, an empty active query keeps it.
	const u32 ps4_host_queries = partial_eval && !num_hw_queries ? 1u : num_hw_queries;

	if (utils::ps4::rsx_programs::cond_render_forced_visible(ps4_skip_visible() || vk::ps4_query_pool_unavailable(), ps4_any_incomplete, ps4_host_queries))
	{
		if (vk::is_renderpass_open(*m_current_command_buffer))
		{
			// Buffer fills are transfer commands: outside a render pass
			const utils::ps4::gpu::rp_close_cause_scope ps4_cause(rsx::ps4_rp_close_query);
			vk::end_renderpass(*m_current_command_buffer);
		}

		vkCmdFillBuffer(*m_current_command_buffer, m_cond_render_buffer->value, 0, 4, 1u);
		vk::insert_buffer_memory_barrier(*m_current_command_buffer, m_cond_render_buffer->value, 0, 4,
			VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage,
			VK_ACCESS_TRANSFER_WRITE_BIT, dst_access);

		utils::ps4::rsx_programs::g_pipeline_stats.cond_render_forced_visible.fetch_add(1, std::memory_order_relaxed);
		rsx::thread::begin_conditional_rendering(sources);
		return;
	}
#endif

	if (num_hw_queries == 1 && !partial_eval) [[ likely ]]
	{
		// Accept the first available query handle as the source of truth. No aggregation is required.
		for (usz i = first; i < last; ++i)
		{
			auto& query_info = m_occlusion_map[sources[i]->driver_handle];
			if (!query_info.indices.empty())
			{
				const auto& index = query_info.indices.front();
				m_occlusion_query_manager->get_query_result_indirect(*m_current_command_buffer, index, 1, m_cond_render_buffer->value, 0);

				vk::insert_buffer_memory_barrier(*m_current_command_buffer, m_cond_render_buffer->value, 0, 4,
					VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage,
					VK_ACCESS_TRANSFER_WRITE_BIT, dst_access);

				rsx::thread::begin_conditional_rendering(sources);
				return;
			}
		}

		// This is unreachable unless something went horribly wrong
		fmt::throw_exception("Unreachable");
	}
	else if (num_hw_queries > 0)
	{
		// We'll need to do some result aggregation using a compute shader.
		vk::buffer* scratch = nullptr;

		// Range latching. Because of how the query pool manages allocations using a stack, we get an inverse sequential set of handles/indices that we can easily group together.
		// This drastically boosts performance on some drivers like the NVIDIA proprietary one that seems to have a rather high cost for every individual query transer command.
		struct { u32 first, last; } query_range = { umax, 0 };

		auto copy_query_range_impl = [&]()
		{
			if (!scratch)
			{
				scratch = vk::get_scratch_buffer(*m_current_command_buffer, num_hw_queries * 4, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
			}

			const auto count = (query_range.last - query_range.first + 1);
			m_occlusion_query_manager->get_query_result_indirect(*m_current_command_buffer, query_range.first, count, scratch->value, dst_offset);
			dst_offset += count * 4;
		};

		for (usz i = first; i < last; ++i)
		{
			auto& query_info = m_occlusion_map[sources[i]->driver_handle];
			for (const auto& index : query_info.indices)
			{
				// First iteration?
				if (query_range.first == umax)
				{
					query_range = { index, index };
					continue;
				}

				// Head?
				if ((query_range.first - 1) == index)
				{
					query_range.first = index;
					continue;
				}

				// Tail?
				if ((query_range.last + 1) == index)
				{
					query_range.last = index;
					continue;
				}

				// Flush pending queue. In practice, this is never reached and we fall out to the spill block outside the loops
				copy_query_range_impl();

				// Start a new range for the current index
				query_range = { index, index };
			}
		}

		if (query_range.first != umax)
		{
			// Dangling queries, flush
			copy_query_range_impl();
		}

		// Sanity check
		ensure(scratch && dst_offset <= scratch->size());

		if (!partial_eval)
		{
			// Fast path should have been caught above
			ensure(dst_offset > 4);

			// Clear result to zero
			vkCmdFillBuffer(*m_current_command_buffer, m_cond_render_buffer->value, 0, 4, 0);

			vk::insert_buffer_memory_barrier(*m_current_command_buffer, m_cond_render_buffer->value, 0, 4,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
		}

		vk::insert_buffer_memory_barrier(*m_current_command_buffer, scratch->value, 0, dst_offset,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

		vk::get_compute_task<vk::cs_aggregator>()->run(*m_current_command_buffer, m_cond_render_buffer.get(), scratch, dst_offset / 4);

		vk::insert_buffer_memory_barrier(*m_current_command_buffer, m_cond_render_buffer->value, 0, 4,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst_stage,
			VK_ACCESS_SHADER_WRITE_BIT, dst_access);
	}
	else if (m_program)
	{
		// This can sometimes happen when shaders are compiling, only log if there is a program hit
		rsx_log.warning("Dubious query data pushed to cond render! Please report to developers(q.pending=%d)", sources.front()->pending);
	}

	rsx::thread::begin_conditional_rendering(sources);
}

void VKGSRender::end_conditional_rendering()
{
	thread::end_conditional_rendering();
}
