#pragma once

#include "swapchain_core.h"

namespace vk
{
	// The Orbis Mesa WSI presents a headless Vulkan surface through sceVideoOut.
	// Its VkSwapchainKHR images and synchronization are handled by swapchain_WSI.
	[[maybe_unused]] static VkSurfaceKHR make_WSI_surface(
		VkInstance vk_instance, display_handle_t /*window_handle*/, WSI_config* config)
	{
		config->supports_automatic_wm_reports = false;

		const auto create_headless_surface = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
			vkGetInstanceProcAddr(vk_instance, "vkCreateHeadlessSurfaceEXT"));
		if (!create_headless_surface)
		{
			fmt::throw_exception("Vulkan driver did not provide vkCreateHeadlessSurfaceEXT");
		}

		VkHeadlessSurfaceCreateInfoEXT create_info{};
		create_info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;

		VkSurfaceKHR surface = VK_NULL_HANDLE;
		CHECK_RESULT(create_headless_surface(vk_instance, &create_info, nullptr, &surface));
		return ensure(surface, "Failed to initialize Orbis Vulkan display surface");
	}
}
