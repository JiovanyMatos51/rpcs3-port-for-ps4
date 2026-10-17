#pragma once

#include "../../Common/ring_buffer_helper.h"
#include "../VulkanAPI.h"
#include "buffer_object.h"
#include "commands.h"
#include "ex.h"

#include <memory>
#include <type_traits>
#include <vector>

namespace vk
{
	enum data_heap_pool_flags
	{
		heap_pool_default            = 0,
		heap_pool_low_latency        = (1 << 0),
		heap_pool_fixed_size         = (1 << 1),
		heap_pool_force_vram_shadow  = (1 << 2),
	};

	class data_heap : public rsx::data_heap
	{
	private:
		usz initial_size = 0;
		bool mapped = false;
		void* _ptr = nullptr;

		rsx::flags32_t m_flags = heap_pool_default;

		bool notify_on_grow = false;
		bool m_prefer_writethrough = false;

		std::unique_ptr<buffer> shadow;
		std::vector<VkBufferCopy> dirty_ranges;

		mutable utils::address_range64 m_cached_buffer_range{};

	protected:
		bool grow(usz size) override;
		bool can_allocate_heap(const vk::memory_type_info& target_heap, usz size, int max_usage_percent);

		void* map_impl(usz offset, usz size);

	public:
		std::unique_ptr<buffer> heap;

		// NOTE: Some drivers (RADV) use heavyweight OS map/unmap routines that are insanely slow
		// Avoid mapping/unmapping to keep these drivers from stalling
		// NOTE2: HOST_CACHED flag does not keep the mapped ptr around in the driver either

		void create(VkBufferUsageFlags usage, usz size, rsx::flags32_t flags, const char* name, usz guard = 0x10000, VkBool32 notify = VK_FALSE);
		void destroy();

		template <typename T = void>
		T* map(usz offset, usz size)
		{
			return reinterpret_cast<T*>(map_impl(offset, size));
		}

		void unmap(bool force = false);

		void sync(const vk::command_buffer& cmd);

		template<int Alignment, typename T = char>
			requires std::is_trivially_destructible_v<T>
		std::pair<usz, T*> alloc_and_map(usz count)
		{
			const auto size_bytes = count * sizeof(T);
			const auto addr = alloc<Alignment>(size_bytes);
			return { addr, reinterpret_cast<T*>(map(addr, size_bytes)) };
		}

		template <usz Alignment>
		VkDescriptorBufferInfoEx window(usz offset, usz range, u64 window_size) const
		{
			if (window_size > size())
			{
				// Driver specific. AMD allows viewing upto 4GB as UBO no problem.
				return { *heap, 0, VK_WHOLE_SIZE };
			}

			if (m_cached_buffer_range.valid() && utils::address_range64::start_length(offset, range).inside(m_cached_buffer_range)) [[ likely ]]
			{
				return { *heap, m_cached_buffer_range.start, m_cached_buffer_range.length() };
			}

			const u64 aligned_window_size = static_cast<u64>(window_size / Alignment) * Alignment;
			const u64 start_partition = offset / aligned_window_size;
			const u64 end_partition = (offset + range - 1) / aligned_window_size;

			if (start_partition == end_partition) [[ likely ]]
			{
				const u64 block_addr = start_partition * aligned_window_size;
				const u64 block_end = std::min<u64>(block_addr + aligned_window_size, size());
				m_cached_buffer_range = utils::address_range64::start_end(block_addr, block_end - 1);
				AUDIT(m_cached_buffer_range.valid());
				return { *heap, m_cached_buffer_range.start, m_cached_buffer_range.length() };
			}

			// We have a partition straddler. Invalidate caching and return exact range.
			m_cached_buffer_range = {};
			return { *heap, offset, range };
		}

		// Properties
		bool is_dirty() const;
		bool has_shadow() const { return !!shadow; }
	};

	extern data_heap* get_upload_heap();

#ifdef RPCS3_PS4
	// 0.38: the renderer's choice of ring sizes (rings-full.flag: the 0.37 sizes), made before its rings exist; the
	// auxiliary upload heap (created on first use) follows it: 16 MiB, or 64 MiB with the flag.
	void ps4_set_full_rings(bool full);
	usz ps4_aux_upload_heap_size();

	// 0.39: the size the title's ring record keeps for the auxiliary upload heap (0 = none; set by the renderer before
	// the heap exists). ps4_aux_upload_heap_size() is then that size, between 16 MiB and the 0.37 64 MiB.
	void ps4_set_aux_upload_heap_record(usz size);

	// 0.39: one texture upload that got its own staging buffer (vk::upload_image) instead of space in 'heap'. Returns
	// the number of such uploads since boot (any thread).
	u32 ps4_note_staging_upload(const data_heap& heap) noexcept;
	// Those of them that did not fit the auxiliary upload heap, since boot. The renderer grows that heap when they come
	// more than 2 per second for 3 s.
	u32 ps4_aux_staging_uploads() noexcept;
#endif
}
