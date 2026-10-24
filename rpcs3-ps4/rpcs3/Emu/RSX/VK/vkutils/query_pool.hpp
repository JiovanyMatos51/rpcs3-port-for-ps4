#pragma once

#include "shared.h"
#include "../../Utils/rsx_utils.h"

namespace vk
{
	class query_pool : public rsx::ref_counted
	{
		VkQueryPool m_query_pool;
		VkDevice m_device;
		u32 m_size;

	public:
		query_pool(VkDevice dev, VkQueryType type, u32 size)
			: m_query_pool(VK_NULL_HANDLE)
			, m_device(dev)
			, m_size(size)
		{
			VkQueryPoolCreateInfo info{};
			info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			info.queryType  = type;
			info.queryCount = size;
#ifdef RPCS3_PS4
			// A refused creation (out of device memory or address space) is handled by the occlusion query manager:
			// a memory reclaim and one retry, then a session without occlusion queries (they answer visible).
			m_ps4_result = vkCreateQueryPool(dev, &info, nullptr, &m_query_pool);
			if (m_ps4_result != VK_SUCCESS)
			{
				m_query_pool = VK_NULL_HANDLE;
				return;
			}
#else
			CHECK_RESULT(vkCreateQueryPool(dev, &info, nullptr, &m_query_pool));
#endif

			// Take 'size' references on this object
			ref_count.release(static_cast<s32>(size));
		}

#ifdef RPCS3_PS4
		VkResult m_ps4_result = VK_SUCCESS;

		bool ps4_created() const
		{
			return m_query_pool != VK_NULL_HANDLE;
		}
#endif

		~query_pool()
		{
			vkDestroyQueryPool(m_device, m_query_pool, nullptr);
		}

		operator VkQueryPool()
		{
			return m_query_pool;
		}

		inline u32 size() const
		{
			return m_size;
		}
	};
}
