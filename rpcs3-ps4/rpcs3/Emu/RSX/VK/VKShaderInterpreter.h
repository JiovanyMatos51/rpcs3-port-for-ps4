#pragma once
#include "Emu/RSX/VK/VKProgramPipeline.h"
#include "Emu/RSX/Program/ProgramStateCache.h"
#include "Emu/RSX/VK/VKPipelineCompiler.h"
#include "vkutils/descriptors.h"
#include <unordered_map>

#ifdef RPCS3_PS4
#include <atomic>
#include <unordered_set>
#include <vector>
#endif

class VKVertexProgram;
class VKFragmentProgram;

namespace rsx
{
	struct shader_loading_dialog;
}

namespace vk
{
#ifdef RPCS3_PS4
	// 0.38 (any thread): interpreter pipelines in the newest renderer's cache, pipelines evicted from it, links the
	// driver refused, and first-use links that waited because interpreter_pending_max were already queued (since boot).
	u32 ps4_interpreter_pipelines_live(u64& evicted, u64& failed, u64& deferred) noexcept;

	// 0.38 (any thread): cap of the interpreter pipeline cache (0 = default 128, minimum 16). The RSX thread applies a
	// lower cap at the next variant change; a memory shedder sets 32 and restores 0.
	void ps4_set_interpreter_cap(u32 cap) noexcept;
#endif

	using ::program_hash_util::fragment_program_utils;
	using ::program_hash_util::vertex_program_utils;

	class shader_interpreter
	{
		using async_build_fn_callback = std::function<void(std::shared_ptr<glsl::program>&)>;

		VkDevice m_device = VK_NULL_HANDLE;

		struct pipeline_key
		{
			u64 compiler_opt;
			vk::pipeline_props properties;

			bool operator == (const pipeline_key& other) const
			{
				return other.compiler_opt == compiler_opt && other.properties == properties;
			}
		};

		struct key_hasher
		{
			usz operator()(const pipeline_key& key) const
			{
				return rpcs3::hash_struct(key.properties) ^ key.compiler_opt;
			}
		};

		struct shader_cache_entry_t
		{
			std::shared_ptr<VKFragmentProgram> m_fs;
			std::shared_ptr<VKVertexProgram> m_vs;
		};

		struct pipeline_info_ex_t
		{
			u32 vertex_instruction_location = 0;
			u32 fragment_instruction_location = 0;
			u32 fragment_textures_location = 0;
		};

		struct pipeline_cache_entry_t
		{
			u32 flags = 0;
			std::shared_ptr<glsl::program> program;
#ifdef RPCS3_PS4
			u64 ps4_last_use = 0; // m_ps4_use_seq when it was last bound or inserted (LRU order)
#endif
		};

		std::unordered_map<pipeline_key, pipeline_cache_entry_t, key_hasher> m_program_cache;
		std::unordered_map<u64, std::shared_ptr<VKVertexProgram>> m_vs_shader_cache;
		std::unordered_map<u64, std::shared_ptr<VKFragmentProgram>> m_fs_shader_cache;
		std::unordered_map<u64, pipeline_info_ex_t> m_pipeline_info_cache;

		mutable shared_mutex m_program_cache_lock;
		mutable shared_mutex m_vs_shader_cache_lock;
		mutable shared_mutex m_fs_shader_cache_lock;

		pipeline_key m_current_key{};
		pipeline_info_ex_t m_current_pipeline_info_ex{};
		std::shared_ptr<glsl::program> m_current_interpreter;

#ifdef RPCS3_PS4
		// 0.38: the compiler options the bound program's shaders were built with (the generic form when a variant is
		// drawn with it), keys whose asynchronous link is queued, keys whose link the driver refused, and programs
		// replaced in the cache by a finished link (the bound one may be among them: the RSX thread hands them to the
		// garbage collector once unbound). Pending, refused and retired are guarded by m_program_cache_lock. A program
		// leaves the cache only through the garbage collector, so one that recorded command buffers still reference is
		// never destroyed early.
		u64 m_ps4_bound_opt = 0;
		bool m_ps4_bound_textures = false; // m_ps4_bound_opt has COMPILER_OPT_ENABLE_TEXTURES (0.39)
		std::unordered_set<pipeline_key, key_hasher> m_ps4_pending;
		std::unordered_map<pipeline_key, u64, key_hasher> m_ps4_refused; // key -> earliest retry (get_system_time)
		std::vector<std::shared_ptr<glsl::program>> m_ps4_retired;
		std::atomic<u32> m_ps4_retired_count{ 0 };
		std::atomic<u64> m_ps4_use_seq{ 0 };
		bool m_ps4_async_links = true; // false: interpreter_only mode or interpreter-sync-link.flag
		bool m_ps4_lru = true;         // false: interpreter-no-lru.flag, or the variant preload filled the cache

		glsl::program* ps4_get(const pipeline_key& key);
		void ps4_bind(const pipeline_key& key, u64 shader_opt, const std::shared_ptr<glsl::program>& program);
		void ps4_queue_link(const pipeline_key& key);
		void ps4_release_retired();
		void ps4_evict(u32 cap);
#endif

		std::shared_ptr<VKVertexProgram> build_vs(u64 compiler_opt);
		std::shared_ptr<VKFragmentProgram> build_fs(u64 compiler_opt);
		std::shared_ptr<glsl::program> link(const vk::pipeline_props& properties, u64 compiler_opt, bool async = false, async_build_fn_callback async_callback = {});

		u32 init(std::shared_ptr<VKVertexProgram>& vk_prog, u64 compiler_opt) const;
		u32 init(std::shared_ptr<VKFragmentProgram>& vk_prog, u64 compiler_opt) const;

		const pipeline_info_ex_t* get_pipeline_info_ex(u64 compiler_opt);

	public:
		void init(const vk::render_device& dev);
		void destroy();

		void preload(rsx::shader_loading_dialog* dlg);

		glsl::program* get(
			const vk::pipeline_props& properties,
			const program_hash_util::fragment_program_utils::fragment_program_metadata& fp_metadata,
			const program_hash_util::vertex_program_utils::vertex_program_metadata& vp_metadata,
			u32 vp_ctrl,
			u32 fp_ctrl);

		// Retrieve the shader components that make up the current interpreter
		std::pair<std::shared_ptr<VKVertexProgram>, std::shared_ptr<VKFragmentProgram>> get_shaders() const;

		bool is_interpreter(const glsl::program* prog) const;

		u32 get_vertex_instruction_location() const;
		u32 get_fragment_instruction_location() const;

		void update_fragment_textures(const std::array<VkDescriptorImageInfoEx, 68>& sampled_images);

#ifdef RPCS3_PS4
		// 0.39: the bound program's shaders declare the fragment texture arrays (true for the generic form also when the
		// drawn variant samples no texture): its texture environment must be bound before every draw.
		bool ps4_bound_has_textures() const noexcept
		{
			return m_ps4_bound_textures;
		}
#endif
	};
}
