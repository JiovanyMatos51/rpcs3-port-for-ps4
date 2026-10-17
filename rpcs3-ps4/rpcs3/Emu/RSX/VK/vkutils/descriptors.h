#pragma once

#include "../VulkanAPI.h"
#include "Utilities/mutex.h"

#include "commands.h"
#include "device.h"

#include "Emu/RSX/Common/simple_array.hpp"

#ifdef RPCS3_PS4
#include <atomic>
#include <memory>
#include <vector>
#endif

namespace vk
{
	struct gc_callback_t
	{
		std::function<void()> m_callback;

		gc_callback_t(std::function<void()> callback)
			: m_callback(callback)
		{}

		~gc_callback_t()
		{
			if (m_callback)
			{
				m_callback();
			}
		}
	};

	struct descriptor_set_dynamic_offset_t
	{
		int location;
		u32 value;
	};

	class descriptor_pool
	{
	public:
		descriptor_pool() = default;
		~descriptor_pool() = default;

		void create(const vk::render_device& dev, const rsx::simple_array<VkDescriptorPoolSize>& pool_sizes, u32 min_sets = 1024, u32 max_sets = 1024);
		void destroy();

		VkDescriptorSet allocate(VkDescriptorSetLayout layout, VkBool32 use_cache = VK_TRUE);

		operator VkDescriptorPool() { return m_current_pool_handle; }
		FORCE_INLINE bool valid() const { return !m_device_subpools.empty(); }
		FORCE_INLINE u32 max_sets() const { return m_current_subpool_index >= m_device_subpools.size() ? 0u : m_device_subpools[m_current_subpool_index].size; }

#ifdef RPCS3_PS4
		// VkDescriptorPool objects this pool created (owner thread).
		u32 ps4_subpool_count() const { return m_device_subpools.size(); }

		// Hands a pool nothing uses any more to the garbage collector, which destroys it once the command buffer being
		// recorded (and so every earlier one that may bind its sets) has completed. Owner thread.
		static void ps4_dispose(std::unique_ptr<descriptor_pool>& pool);
#endif

	private:
		FORCE_INLINE bool can_allocate(u32 required_count, u32 already_used_count = 0) const { return (required_count + already_used_count) <= max_sets(); };
		void reset(u32 subpool_id, VkDescriptorPoolResetFlags flags);
		void next_subpool();

		std::pair<VkResult, VkDescriptorPool> new_subpool();

		struct logical_subpool_t
		{
			VkDescriptorPool handle = VK_NULL_HANDLE;
			u32 size = 0;
			VkBool32 busy = VK_FALSE;
#ifdef RPCS3_PS4
			u64 ps4_window_bytes = 0; // estimated GPU window of this VkDescriptorPool
#endif
		};

		struct autoscaling_config_t
		{
			u32 min_pool_size = 0;
			u32 max_pool_size = 0;
			u32 current_size = 0;

			// Debounce setup.
			static constexpr u32 increment_min_steps = 2u;
			u32 increment_steps = 0;

			u32 get_pool_size();
		};

		const vk::render_device* m_owner = nullptr;
		VkDescriptorPoolCreateInfo m_create_info = {};
		autoscaling_config_t m_autoscaling_config = {};
		rsx::simple_array<VkDescriptorPoolSize> m_create_info_pool_sizes;

		rsx::simple_array<logical_subpool_t> m_device_subpools;
		VkDescriptorPool m_current_pool_handle = VK_NULL_HANDLE;
		u32 m_current_subpool_index = umax;
		u32 m_current_subpool_offset = 0;

		shared_mutex m_subpool_lock;

		static constexpr size_t max_cache_size = 64;
		VkDescriptorSetLayout m_cached_layout = VK_NULL_HANDLE;
		rsx::simple_array<VkDescriptorSet> m_descriptor_set_cache;
		rsx::simple_array<VkDescriptorSetLayout> m_allocation_request_cache;

#ifdef RPCS3_PS4
		// Shared with the subpool resets queued in the garbage collector (they run on the driver thread): cleared by
		// destroy(), so a reset that runs after the pool was destroyed does nothing, whatever order the collector uses.
		std::shared_ptr<std::atomic<bool>> m_ps4_alive = std::make_shared<std::atomic<bool>>(true);
#endif
	};

	class descriptor_set
	{
#ifdef RPCS3_PS4
		// Every vk::glsl::program keeps its descriptor_set, and with it these scratch arrays, until the game stops.
		// The PC sizes (16384 + 64 entries per array, about 0.9 MiB per set plus the queued writes) are committed in
		// full on the console, where a few hundred bound programs exhausted the 2 GiB CPU heap. Queued writes are
		// flushed every 64 entries instead (256 until 0.35: 60 KiB per set, 15 KiB now). The queued writes point into
		// these arrays, so they must never reallocate between two flushes; the overflow margin holds what a single
		// commit stores after the last pressure check in on_bind() (the shader interpreter stores 4 x 16 image infos
		// in one commit, the most of any program). 0.37: entries beyond the reserved size go to ps4_spill() instead.
		static constexpr size_t max_cache_size = 64;
		static constexpr size_t max_overflow_size = 64;
#else
		static constexpr size_t max_cache_size = 16384;
		static constexpr size_t max_overflow_size = 64;
#endif
		static constexpr size_t m_pool_size = max_cache_size + max_overflow_size;

		void init(VkDescriptorSet new_set);

	public:
#if defined(__clang__) && (__clang_major__ < 16)
		// Clang (pre 16.x) does not support LWG 2089, std::construct_at for POD types
		struct WriteDescriptorSetT : public VkWriteDescriptorSet
		{
			WriteDescriptorSetT(
				VkStructureType                  sType,
				const void*                      pNext,
				VkDescriptorSet                  dstSet,
				uint32_t                         dstBinding,
				uint32_t                         dstArrayElement,
				uint32_t                         descriptorCount,
				VkDescriptorType                 descriptorType,
				const VkDescriptorImageInfo*     pImageInfo,
				const VkDescriptorBufferInfo*    pBufferInfo,
				const VkBufferView*              pTexelBufferView)
			{
				this->sType = sType,
				this->pNext = pNext,
				this->dstSet = dstSet,
				this->dstBinding = dstBinding,
				this->dstArrayElement = dstArrayElement,
				this->descriptorCount = descriptorCount,
				this->descriptorType = descriptorType,
				this->pImageInfo = pImageInfo,
				this->pBufferInfo = pBufferInfo,
				this->pTexelBufferView = pTexelBufferView;
			}
		};
#else
		using WriteDescriptorSetT = VkWriteDescriptorSet;
#endif

	public:
		descriptor_set(VkDescriptorSet set);
		descriptor_set() = default;
		~descriptor_set();

		descriptor_set(const descriptor_set&) = delete;

		void swap(descriptor_set& other);
		descriptor_set& operator = (VkDescriptorSet set);

		VkDescriptorSet value() const { return m_handle; }
		operator bool() const { return m_handle != VK_NULL_HANDLE; }

		VkDescriptorSet* ptr();
		void push(const VkBufferView& buffer_view, VkDescriptorType type, u32 binding);
		void push(const VkDescriptorBufferInfo& buffer_info, VkDescriptorType type, u32 binding);
		void push(const VkDescriptorImageInfo& image_info, VkDescriptorType type, u32 binding);
		void push(const VkDescriptorImageInfo* image_info, u32 count, VkDescriptorType type, u32 binding);
		void push(const rsx::simple_array<VkCopyDescriptorSet>& copy_cmd, u32 type_mask = umax);
		void push(const rsx::simple_array<VkWriteDescriptorSet>& write_cmds, u32 type_mask = umax);
		void push(const descriptor_set_dynamic_offset_t& offset);

		// Event handlers
		void on_bind();
		void bind(const vk::command_buffer& cmd, VkPipelineBindPoint bind_point, VkPipelineLayout layout);

		void flush();

#ifdef RPCS3_PS4
		// Before the set's pool is released: writes the queued updates, drops the handle (the next commit allocates a
		// new set and rewrites every slot) and frees the scratch arrays, which the next use reserves again.
		void ps4_release();
#endif

		// Typed temporary storage access. Should be inline, the overhead is significant
		FORCE_INLINE VkBufferView* store(const VkBufferView& buffer_view)
		{
#ifdef RPCS3_PS4
			if (m_buffer_view_pool.size() >= m_buffer_view_pool.capacity()) [[unlikely]]
			{
				return static_cast<VkBufferView*>(ps4_spill(&buffer_view, sizeof(buffer_view)));
			}
#endif
			m_buffer_view_pool.push_back(buffer_view);
			return &m_buffer_view_pool.back();
		}

		FORCE_INLINE VkDescriptorBufferInfo* store(const VkDescriptorBufferInfo& buffer_info)
		{
#ifdef RPCS3_PS4
			if (m_buffer_info_pool.size() >= m_buffer_info_pool.capacity()) [[unlikely]]
			{
				return static_cast<VkDescriptorBufferInfo*>(ps4_spill(&buffer_info, sizeof(buffer_info)));
			}
#endif
			m_buffer_info_pool.push_back(buffer_info);
			return &m_buffer_info_pool.back();
		}

		FORCE_INLINE VkDescriptorImageInfo* store(const VkDescriptorImageInfo& image_info)
		{
#ifdef RPCS3_PS4
			if (m_image_info_pool.size() >= m_image_info_pool.capacity()) [[unlikely]]
			{
				return static_cast<VkDescriptorImageInfo*>(ps4_spill(&image_info, sizeof(image_info)));
			}
#endif
			m_image_info_pool.push_back(image_info);
			return &m_image_info_pool.back();
		}

		FORCE_INLINE VkDescriptorImageInfo* store(const rsx::simple_array<VkDescriptorImageInfo>& image_infos)
		{
#ifdef RPCS3_PS4
			if (m_image_info_pool.size() + image_infos.size() > m_image_info_pool.capacity()) [[unlikely]]
			{
				return static_cast<VkDescriptorImageInfo*>(ps4_spill(image_infos.data(), image_infos.size_bytes()));
			}
#endif
			const auto offset = m_image_info_pool.size();
			m_image_info_pool += image_infos;
			return &m_image_info_pool[offset];
		}

		FORCE_INLINE bool storage_cache_pressure() const
		{
			return
				m_image_info_pool.size() >= max_cache_size ||
				m_buffer_info_pool.size() >= max_cache_size ||
				m_buffer_view_pool.size() >= max_cache_size;
		}

		// Temporary storage accessors
		const rsx::simple_array<WriteDescriptorSetT> peek() const { return m_pending_writes; }
		u64 cache_id() const { return m_storage_cache_id; }

		// Basic lockable for storage coherence
		void lock() { m_storage_lock.lock(); }
		void unlock() { m_storage_lock.unlock(); }

	private:
		VkDescriptorSet m_handle = VK_NULL_HANDLE;
		u64 m_update_after_bind_mask = 0;
		u64 m_push_type_mask = 0;
		bool m_in_use = false;

		shared_mutex m_storage_lock;
		atomic_t<u64> m_storage_cache_id = 0;

		rsx::simple_array<VkBufferView> m_buffer_view_pool;
		rsx::simple_array<VkDescriptorBufferInfo> m_buffer_info_pool;
		rsx::simple_array<VkDescriptorImageInfo> m_image_info_pool;
		rsx::simple_array<u32> m_dynamic_offsets;

		rsx::simple_array<WriteDescriptorSetT> m_pending_writes;
		rsx::simple_array<VkCopyDescriptorSet> m_pending_copies;

#ifdef RPCS3_PS4
		// Bytes the scratch arrays reserved at first use, subtracted from the live total when the set goes away.
		u64 m_ps4_storage_bytes = 0;

		// 0.37: scratch entries that do not fit the reserved arrays. Growing an array would move entries the queued
		// writes and the caller's descriptor template already point to, and flushing first would hand back storage
		// those pointers still use (and takes m_storage_lock, which update_descriptor_template holds). Such entries
		// get a block of their own instead, with the arrays' lifetime: released by the next flush().
		std::vector<std::unique_ptr<u64[]>> m_ps4_spill;
		void* ps4_spill(const void* data, usz bytes);
#endif
	};

	namespace descriptors
	{
		void init();
		void flush();
		void destroy();

		VkDescriptorSetLayout create_layout(const rsx::simple_array<VkDescriptorSetLayoutBinding>& bindings);
	}

#ifdef RPCS3_PS4
	// Memory telemetry for the 60 s 'PS4 VRAM:' line. Relaxed counters, any thread.
	// Live descriptor_sets whose scratch arrays were reserved (one per program descriptor table bound at least once).
	u32 ps4_descriptor_sets_inited() noexcept;
	// CPU heap bytes those scratch arrays reserved at first use.
	u64 ps4_descriptor_storage_bytes() noexcept;
	// Live VkDescriptorPool objects created by descriptor_pool.
	u32 ps4_descriptor_pools_live() noexcept;
	// Their estimated GPU window bytes (utils::ps4::gpu::descriptor_pool_window_bytes).
	u64 ps4_descriptor_pool_window_bytes() noexcept;
	// Shaders whose GLSL text and SPIR-V words were released once their module existed, and the bytes released
	// (VKProgramPipeline.cpp).
	u32 ps4_shader_text_released_count() noexcept;
	u64 ps4_shader_text_released_bytes() noexcept;
	// 0.37: scratch entries stored outside a full scratch array (descriptor_set::ps4_spill), since boot.
	u64 ps4_descriptor_spills() noexcept;
#endif
}
