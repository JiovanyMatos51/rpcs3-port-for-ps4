#pragma once

#include "RSXFragmentProgram.h"
#include "RSXVertexProgram.h"

#include "Utilities/mutex.h"
#include "util/logs.hpp"
#include "util/fnv_hash.hpp"
#include "util/v128.hpp"
#include <util/bless.hpp>

#include <span>
#include <unordered_map>

#ifdef RPCS3_PS4
#include "Utilities/File.h"
#include "util/ps4_rsx_programs.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

void ps4_startup_checkpoint(const char* stage);
#endif

enum class SHADER_TYPE
{
	SHADER_TYPE_VERTEX,
	SHADER_TYPE_FRAGMENT
};

namespace program_hash_util
{
	struct vertex_program_utils
	{
		struct vertex_program_metadata
		{
			bit_set<rsx::max_vertex_program_instructions> instruction_mask;
			u32 ucode_length;
			u32 referenced_textures_mask;
			u16 referenced_inputs_mask;
			u16 reserved;
		};

		static usz get_vertex_program_ucode_hash(const RSXVertexProgram &program);

		static vertex_program_metadata analyse_vertex_program(const u32* data, u32 entry, RSXVertexProgram& dst_prog);
	};

	struct vertex_program_storage_hash
	{
		usz operator()(const RSXVertexProgram &program) const;
	};

	struct vertex_program_compare
	{
		static bool compare_properties(const RSXVertexProgram& binary1, const RSXVertexProgram& binary2);

		bool operator()(const RSXVertexProgram &binary1, const RSXVertexProgram &binary2) const;
	};

	struct fragment_program_utils
	{
		struct fragment_program_metadata
		{
			u32 program_start_offset;
			u32 program_ucode_length;
			u32 program_constants_buffer_length;
			u16 referenced_textures_mask;
			u16 bx2_texture_reads_mask;

			bool has_pack_instructions;
			bool has_branch_instructions;
			bool is_nop_shader;           // Does this affect Z-pass testing???
		};

		/**
		* returns true if the given source Operand is a constant
		*/

		static bool is_any_src_constant(v128 sourceOperand);

		static usz get_fragment_program_ucode_size(const void* ptr);

		static fragment_program_metadata analyse_fragment_program(const void* ptr);

		static usz get_fragment_program_ucode_hash(const RSXFragmentProgram &program);
	};

	struct fragment_program_storage_hash
	{
		usz operator()(const RSXFragmentProgram &program) const;
	};

	struct fragment_program_compare
	{
		static bool compare_properties(const RSXFragmentProgram& binary1, const RSXFragmentProgram& binary2);

		bool operator()(const RSXFragmentProgram &binary1, const RSXFragmentProgram &binary2) const;
	};
}

namespace rsx
{
	struct program_cache_hint_t
	{
		template <typename T>
		T* get_fragment_program() const
		{
			return utils::bless<T>(m_cached_fragment_program);
		}

		template <typename T>
		T* get_vertex_program() const
		{
			return utils::bless<T>(m_cached_vertex_program);
		}

		bool has_vertex_program() const
		{
			return m_cached_vertex_program != nullptr;
		}

		bool has_fragment_program() const
		{
			return m_cached_fragment_program != nullptr;
		}

		// Invalidate caches if the renderer state shows they are out of date
		void invalidate(u32 flags);

		// Invalidate vertex program if the cached program is not compatible with the incoming
		void invalidate_vertex_program(const RSXVertexProgram& p);

		// Invalidate fragment program if the cached program is not compatible with the incoming
		void invalidate_fragment_program(const RSXFragmentProgram& p);

		// Helper - optionally cache the vertex program if possible
		static void cache_vertex_program(program_cache_hint_t* cache, const RSXVertexProgram& ref, void* vertex_program);

		// Helper - optionally cache the fragment program if possible
		static void cache_fragment_program(program_cache_hint_t* cache, const RSXFragmentProgram& ref, void* fragment_program);

	protected:
		void* m_cached_fragment_program = nullptr;
		void* m_cached_vertex_program = nullptr;

		RSXFragmentProgram m_cached_fp_properties;
		RSXVertexProgram m_cached_vp_properties;
	};

	void write_fragment_constants_to_buffer(const std::span<f32>& buffer, const RSXFragmentProgram& rsx_prog, const std::vector<u32>& offsets_cache, bool sanitize = true);
}


/**
* Cache for program help structure (blob, string...)
* The class is responsible for creating the object so the state only has to call getGraphicPipelineState
* Template argument is a struct which has the following type declaration :
* - a typedef VertexProgramData to a type that encapsulate vertex program info. It should provide an Id member.
* - a typedef FragmentProgramData to a types that encapsulate fragment program info. It should provide an Id member and a fragment constant offset vector.
* - a typedef PipelineData encapsulating monolithic program.
* - a typedef pipeline_properties to a type that encapsulate various state info relevant to program compilation (alpha test, primitive type,...)
* - a	typedef ExtraData type that will be passed to the buildProgram function.
* It should also contains the following function member :
* - static void recompile_fragment_program(RSXFragmentProgram *RSXFP, FragmentProgramData& fragmentProgramData, usz ID);
* - static void recompile_vertex_program(RSXVertexProgram *RSXVP, VertexProgramData& vertexProgramData, usz ID);
* - static PipelineData build_program(VertexProgramData &vertexProgramData, FragmentProgramData &fragmentProgramData, const pipeline_properties &pipeline_properties, const ExtraData& extraData);
* - static void validate_pipeline_properties(const VertexProgramData &vertexProgramData, const FragmentProgramData &fragmentProgramData, pipeline_properties& props);
*/
template<typename backend_traits>
class program_state_cache
{
	using pipeline_storage_type = typename backend_traits::pipeline_storage_type;
	using pipeline_type = typename backend_traits::pipeline_type;
	using pipeline_properties = typename backend_traits::pipeline_properties;
	using vertex_program_type = typename backend_traits::vertex_program_type;
	using fragment_program_type = typename backend_traits::fragment_program_type;

	using binary_to_vertex_program = std::unordered_map<RSXVertexProgram, vertex_program_type, program_hash_util::vertex_program_storage_hash, program_hash_util::vertex_program_compare> ;
	using binary_to_fragment_program = std::unordered_map<RSXFragmentProgram, fragment_program_type, program_hash_util::fragment_program_storage_hash, program_hash_util::fragment_program_compare>;

	using pipeline_data_type = std::tuple<pipeline_type*, const vertex_program_type*, const fragment_program_type*>;

	struct pipeline_key
	{
		u32 vertex_program_id;
		u32 fragment_program_id;
		pipeline_properties properties;
	};

	struct pipeline_key_hash
	{
		usz operator()(const pipeline_key &key) const
		{
			usz hashValue = 0;
			hashValue ^= rpcs3::hash_base<unsigned>(key.vertex_program_id);
			hashValue ^= rpcs3::hash_base<unsigned>(key.fragment_program_id);
			hashValue ^= rpcs3::hash_struct<pipeline_properties>(key.properties);
			return hashValue;
		}
	};

	struct pipeline_key_compare
	{
		bool operator()(const pipeline_key &key1, const pipeline_key &key2) const
		{
			return (key1.vertex_program_id == key2.vertex_program_id) && (key1.fragment_program_id == key2.fragment_program_id) && (key1.properties == key2.properties);
		}
	};

protected:
	using decompiler_callback_t = std::function<void(const pipeline_properties&, const RSXVertexProgram&, const RSXFragmentProgram&)>;

	shared_mutex m_vertex_mutex;
	shared_mutex m_fragment_mutex;
	shared_mutex m_pipeline_mutex;
	shared_mutex m_decompiler_mutex;

	atomic_t<usz> m_next_id = 0;
	bool m_cache_miss_flag; // Set if last lookup did not find any usable cached programs

	binary_to_vertex_program m_vertex_shader_cache;
	binary_to_fragment_program m_fragment_shader_cache;
#ifdef RPCS3_PS4
	// 0.38: an entry also records when the renderer last took its pipeline (ps4_now_s seconds; 0 = never, e.g. only
	// the disk preload) and, for a placeholder whose link the driver refused, when that happened (0 = not refused).
	struct ps4_pipeline_entry
	{
		pipeline_storage_type pipeline;
		u32 last_use_s = 0;
		u32 refused_s = 0;
	};

	std::unordered_map<pipeline_key, ps4_pipeline_entry, pipeline_key_hash, pipeline_key_compare> m_storage;
#else
	std::unordered_map<pipeline_key, pipeline_storage_type, pipeline_key_hash, pipeline_key_compare> m_storage;
#endif

	decompiler_callback_t notify_pipeline_compiled;

	vertex_program_type __null_vertex_program;
	fragment_program_type __null_fragment_program;
	pipeline_storage_type __null_pipeline_handle;

#ifdef RPCS3_PS4
	// Renderer lookups (cache_hint set, RSX thread) only: the pipeline handed out last (still bound by the renderer,
	// never evicted), a coarse clock refreshed every 64 lookups, the last idle-pipeline check, the eviction switch
	// (-1 not read yet) and the eviction passes so far.
	const pipeline_type* m_ps4_last_handed = nullptr;
	u32 m_ps4_clock_s = 0;
	u32 m_ps4_lookups = 0;
	u32 m_ps4_last_sweep_s = 0;
	int m_ps4_lru = -1;
	u32 m_ps4_eviction_passes = 0;

	// Shaders whose first decompile/compile is still running on the thread that inserted them, or queued for or running
	// on a pipeline worker (their module handle is not final yet), and the earliest time a module the driver refused
	// may be created again.
	std::mutex m_ps4_module_mutex;
	std::vector<const void*> m_ps4_compiling;
	std::atomic<u32> m_ps4_module_retry_s{0};
	std::atomic<u32> m_ps4_module_retries_ok{0};

	static u32 ps4_now_s()
	{
		return static_cast<u32>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) + 1;
	}

	void ps4_compiling(const void* shader, bool begin)
	{
		std::lock_guard lock(m_ps4_module_mutex);
		if (begin)
		{
			m_ps4_compiling.push_back(shader);
		}
		else
		{
			m_ps4_compiling.erase(std::remove(m_ps4_compiling.begin(), m_ps4_compiling.end(), shader), m_ps4_compiling.end());
		}
	}

	// 0.39 (shader-translate-async.flag, read at the first renderer lookup of this cache; -1 = not read yet): a new
	// program of a backend whose traits declare ps4_async_translate (Vulkan) is translated on the pipeline workers. Its
	// id is set at once under the cache lock, and it stays in m_ps4_compiling (not ready: interpreter or skipped draw)
	// until a worker has translated it, or the renderer thread did because no worker had started it within
	// translate_take_back_us. The job reads the cache's own key (a map node, erased only by clear(), which runs after
	// the workers are gone), never the caller's program, which the next draw changes.
	static constexpr bool ps4_has_async_translate = requires { requires backend_traits::ps4_async_translate; };
	int m_ps4_translate_async = -1;

	struct ps4_translate_job
	{
		std::atomic<u32> state{0}; // 0 queued, 1 claimed by a worker or by the renderer thread
		u64 queued_us = 0;
		const void* shader = nullptr;
		std::function<void()> run;

		bool claim() noexcept
		{
			u32 expected = 0;
			return state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
		}
	};

	// Posted translations not finished yet (under m_ps4_module_mutex).
	std::vector<std::shared_ptr<ps4_translate_job>> m_ps4_translate_jobs;

	static u64 ps4_now_us()
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	// Renderer lookups only.
	bool ps4_translate_async_on()
	{
		if (m_ps4_translate_async < 0)
		{
			m_ps4_translate_async = fs::is_file(utils::ps4::rsx_programs::translate_async_flag) ? 1 : 0;
			if (m_ps4_translate_async)
			{
				ps4_startup_checkpoint("RSX: new shader programs are translated on the pipeline workers (shader-translate-async.flag)");
			}
		}

		return m_ps4_translate_async > 0;
	}

	// Queues the translation of a program just inserted (under the exclusive lock of its kind, already in
	// m_ps4_compiling, id set). It leaves m_ps4_compiling once translated.
	void ps4_post_translation(const void* shader, std::function<void()> translate)
	{
		auto job = std::make_shared<ps4_translate_job>();
		job->shader = shader;
		job->queued_us = ps4_now_us();
		job->run = [this, shader, translate = std::move(translate)]()
		{
			translate();

			std::lock_guard lock(m_ps4_module_mutex);
			m_ps4_compiling.erase(std::remove(m_ps4_compiling.begin(), m_ps4_compiling.end(), shader), m_ps4_compiling.end());
			m_ps4_translate_jobs.erase(std::remove_if(m_ps4_translate_jobs.begin(), m_ps4_translate_jobs.end(),
				[shader](const std::shared_ptr<ps4_translate_job>& j) { return j->shader == shader; }), m_ps4_translate_jobs.end());
		};

		{
			std::lock_guard lock(m_ps4_module_mutex);
			m_ps4_translate_jobs.push_back(job);
		}

		backend_traits::ps4_post_translation([job]()
		{
			if (job->claim())
			{
				job->run();
				utils::ps4::rsx_programs::g_pipeline_stats.translate_async.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}

	// Renderer thread, for a program that is not ready: a translation queued translate_take_back_us ago that no worker
	// has started is run here, as without the flag. True when it ran.
	bool ps4_take_back_translation(const void* shader)
	{
		std::shared_ptr<ps4_translate_job> job;
		{
			std::lock_guard lock(m_ps4_module_mutex);
			for (const auto& entry : m_ps4_translate_jobs)
			{
				if (entry->shader == shader)
				{
					job = entry;
					break;
				}
			}
		}

		if (!job || ps4_now_us() - job->queued_us < utils::ps4::rsx_programs::translate_take_back_us || !job->claim())
		{
			return false;
		}

		const auto start = std::chrono::steady_clock::now();
		job->run();
		ps4_note_rsx_translation(start);
		utils::ps4::rsx_programs::g_pipeline_stats.translate_taken_back.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	// False while the shader has no module: its first compile is still running on another thread, or the driver refused
	// the module (shader::compile keeps the SPIR-V), which is then created again at most once per
	// pipeline_refused_retry_s. Its pipelines are not ready meanwhile (interpreter or skipped draw), never linked
	// with a null module.
	template <typename Program>
	bool ps4_module_ready(const Program& program, shared_mutex& mutex)
	{
		if constexpr (requires (Program& p) { p.handle == nullptr; p.Compile(); p.id; })
		{
			if (program.handle) [[likely]]
			{
				// 0.39: a pipeline worker may have translated it (the module handle is its last store): what it wrote
				// before is read after this.
				std::atomic_thread_fence(std::memory_order_acquire);
				return true;
			}

			if constexpr (ps4_has_async_translate)
			{
				if (ps4_take_back_translation(&program) && program.handle)
				{
					return true;
				}
			}

			const u32 now = ps4_now_s();
			if (now < m_ps4_module_retry_s.load(std::memory_order_relaxed))
			{
				return false;
			}

			// Exclusive: no lookup of this shader kind and no other retry runs meanwhile.
			std::lock_guard lock(mutex);
			{
				std::lock_guard compiling(m_ps4_module_mutex);
				if (std::find(m_ps4_compiling.begin(), m_ps4_compiling.end(), static_cast<const void*>(&program)) != m_ps4_compiling.end())
				{
					return false;
				}
			}

			if (program.handle)
			{
				return true;
			}

			if (now < m_ps4_module_retry_s.load(std::memory_order_relaxed))
			{
				return false;
			}

			const_cast<Program&>(program).Compile();
			if (!program.handle)
			{
				m_ps4_module_retry_s.store(now + utils::ps4::rsx_programs::pipeline_refused_retry_s, std::memory_order_relaxed);
				return false;
			}

			if (const u32 n = m_ps4_module_retries_ok.fetch_add(1, std::memory_order_relaxed) + 1; n <= 16)
			{
				rsx_log.notice("PS4: shader module %u created on a later attempt after the driver had refused it (retry %u)", program.id, n);
			}

			return true;
		}
		else
		{
			return true;
		}
	}

	u32 ps4_time(bool renderer) const
	{
		return renderer && m_ps4_clock_s ? m_ps4_clock_s : ps4_now_s();
	}

	// A new program translated by a renderer lookup (cache hint set; the disk preload passes none), on the renderer
	// thread: counted with its time for the backend telemetry line.
	static void ps4_note_rsx_translation(std::chrono::steady_clock::time_point start) noexcept
	{
		namespace rp = utils::ps4::rsx_programs;
		const u64 us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
		rp::g_pipeline_stats.rsx_translate.fetch_add(1, std::memory_order_relaxed);
		rp::add_duration(rp::g_pipeline_stats.rsx_translate_us, rp::g_pipeline_stats.rsx_translate_ms, us);
		rp::note_max(rp::g_pipeline_stats.rsx_translate_max_us, us);
	}

	pipeline_data_type ps4_hand(ps4_pipeline_entry& entry, bool renderer, const vertex_program_type& vp, const fragment_program_type& fp)
	{
		m_cache_miss_flag = !entry.pipeline;
		if (renderer)
		{
			entry.last_use_s = m_ps4_clock_s;
			m_ps4_last_handed = entry.pipeline.get();
		}

		return { entry.pipeline.get(), &vp, &fp };
	}

	// The driver refused the link after a memory reclaim and one retry (pipe compiler): the placeholder stays, so the
	// pipeline is not ready, and the next lookup after pipeline_refused_retry_s links it again.
	void ps4_note_refused(const pipeline_key& key)
	{
		{
			std::lock_guard lock(m_pipeline_mutex);
			auto& entry = m_storage[key];
			if (entry.pipeline)
			{
				return;
			}

			entry.refused_s = ps4_now_s();
		}

		utils::ps4::rsx_programs::g_pipeline_stats.refused.fetch_add(1, std::memory_order_relaxed);
	}

	// Renderer lookup: refreshes the coarse clock and, once per pipeline_sweep_s, destroys idle pipelines above the cap.
	void ps4_renderer_tick()
	{
		if ((m_ps4_lookups++ & 63) && m_ps4_clock_s)
		{
			return;
		}

		m_ps4_clock_s = ps4_now_s();
		if (m_ps4_clock_s - m_ps4_last_sweep_s >= utils::ps4::rsx_programs::pipeline_sweep_s)
		{
			m_ps4_last_sweep_s = m_ps4_clock_s;
			ps4_evict_idle();
		}
	}

	// Above pipeline_live_cap live pipelines, those the renderer has not taken for pipeline_idle_s (or never) are
	// destroyed through the backend's garbage collector (ps4_dispose_pipeline, found by argument-dependent lookup;
	// without one nothing is evicted), oldest first, down to pipeline_live_target. The pipeline handed out last is
	// never one of them; a destroyed one is linked again on its next use.
	void ps4_evict_idle()
	{
		namespace rp = utils::ps4::rsx_programs;

		if constexpr (requires (pipeline_storage_type& p) { ps4_dispose_pipeline(std::move(p)); })
		{
			if (m_ps4_lru < 0)
			{
				m_ps4_lru = fs::is_file(rp::no_pipeline_lru_flag) ? 0 : 1;
				ps4_startup_checkpoint(m_ps4_lru
					? "RSX: above 4000 live graphics pipelines, those unused for 10 min are destroyed (oldest first, down to 3000; no-pipeline-lru.flag keeps all)"
					: "RSX: every graphics pipeline is kept until the game stops (no-pipeline-lru.flag)");
			}

			std::vector<pipeline_storage_type> victims;
			u32 live = 0;
			{
				std::lock_guard lock(m_pipeline_mutex);

				if (!m_ps4_lru || m_storage.size() <= rp::pipeline_live_cap)
				{
					rp::g_pipeline_stats.live.store(static_cast<u32>(m_storage.size()), std::memory_order_relaxed);
					return;
				}

				std::vector<typename decltype(m_storage)::iterator> entries;
				std::vector<u32> last_use;
				entries.reserve(m_storage.size());
				last_use.reserve(m_storage.size());

				for (auto it = m_storage.begin(); it != m_storage.end(); ++it)
				{
					if (!it->second.pipeline)
					{
						// Still compiling or refused.
						continue;
					}

					entries.push_back(it);
					last_use.push_back(it->second.pipeline.get() == m_ps4_last_handed ? m_ps4_clock_s : it->second.last_use_s);
				}

				for (const std::size_t index : rp::idle_victims(last_use, m_ps4_clock_s, rp::pipeline_idle_s, rp::pipeline_live_cap, rp::pipeline_live_target))
				{
					victims.push_back(std::move(entries[index]->second.pipeline));
					m_storage.erase(entries[index]);
				}

				live = static_cast<u32>(entries.size() - victims.size());
			}

			rp::g_pipeline_stats.live.store(live, std::memory_order_relaxed);
			if (victims.empty())
			{
				return;
			}

			const u64 total = rp::g_pipeline_stats.evicted.fetch_add(victims.size(), std::memory_order_relaxed) + victims.size();
			const u32 count = static_cast<u32>(victims.size());
			for (auto& pipeline : victims)
			{
				ps4_dispose_pipeline(std::move(pipeline));
			}

			// The 1st, 2nd, 4th, 8th... pass of this cache.
			if (const u32 pass = ++m_ps4_eviction_passes; !(pass & (pass - 1)))
			{
				const std::string line = fmt::format("RSX: pipelines live %u (evicted %llu): destroyed %u not used for %u s (pass %u)",
					live, total, count, rp::pipeline_idle_s, pass);
				rsx_log.notice("PS4 %s", line);
				ps4_startup_checkpoint(line.c_str());
			}
		}
	}
#endif

	/// bool here to inform that the program was preexisting.
	std::tuple<const vertex_program_type&, bool> search_vertex_program(
		rsx::program_cache_hint_t* cache_hint,
		const RSXVertexProgram& rsx_vp
#ifdef RPCS3_PS4
		, bool ps4_async = false
#endif
		)
	{
		if (cache_hint && cache_hint->has_vertex_program())
		{
			// The caller guarantees that the cached vertex program is correct.
			return std::forward_as_tuple(*cache_hint->get_vertex_program<vertex_program_type>(), true);
		}

		bool recompile = false;
		vertex_program_type* new_shader;
		{
			reader_lock lock(m_vertex_mutex);

			const auto& I = m_vertex_shader_cache.find(rsx_vp);
			if (I != m_vertex_shader_cache.end())
			{
				rsx::program_cache_hint_t::cache_vertex_program(cache_hint, rsx_vp, &(I->second));
				return std::forward_as_tuple(I->second, true);
			}

			rsx_log.trace("VP not found in buffer!");

			lock.upgrade();
			auto [it, inserted] = m_vertex_shader_cache.try_emplace(rsx_vp);
			new_shader = &(it->second);
			recompile = inserted;
#ifdef RPCS3_PS4
			if (inserted)
			{
				// Registered under the exclusive lock: no lookup may take its null module for a refused one.
				ps4_compiling(new_shader, true);

				if constexpr (ps4_has_async_translate)
				{
					if (ps4_async && cache_hint && ps4_translate_async_on())
					{
						// The id is final at once (this lookup's pipeline key uses it); a worker translates the
						// program from the cache's own key.
						new_shader->id = static_cast<decltype(new_shader->id)>(m_next_id++);
						const RSXVertexProgram* key = &it->first;
						vertex_program_type* shader = new_shader;
						ps4_post_translation(shader, [key, shader]()
						{
							backend_traits::ps4_translate_vertex_program(*key, *shader);
						});
						recompile = false;
					}
				}
			}
#endif
		}

		if (recompile)
		{
#ifdef RPCS3_PS4
			const auto ps4_start = std::chrono::steady_clock::now();
#endif
			backend_traits::recompile_vertex_program(rsx_vp, *new_shader, m_next_id++);
#ifdef RPCS3_PS4
			if (cache_hint)
			{
				ps4_note_rsx_translation(ps4_start);
			}

			ps4_compiling(new_shader, false);
#endif
		}

		rsx::program_cache_hint_t::cache_vertex_program(cache_hint, rsx_vp, new_shader);
		return std::forward_as_tuple(*new_shader, false);
	}

	/// bool here to inform that the program was preexisting.
	std::tuple<const fragment_program_type&, bool> search_fragment_program(rsx::program_cache_hint_t* cache_hint, const RSXFragmentProgram& rsx_fp
#ifdef RPCS3_PS4
		, bool ps4_async = false
#endif
		)
	{
		if (cache_hint && cache_hint->has_fragment_program())
		{
			// The caller guarantees that the cached fragemnt program is correct.
			return std::forward_as_tuple(*cache_hint->get_fragment_program<fragment_program_type>(), true);
		}

		bool recompile = false;
		typename binary_to_fragment_program::iterator it;
		fragment_program_type* new_shader;
		{
			reader_lock lock(m_fragment_mutex);

			const auto& I = m_fragment_shader_cache.find(rsx_fp);
			if (I != m_fragment_shader_cache.end())
			{
				rsx::program_cache_hint_t::cache_fragment_program(cache_hint, rsx_fp, &(I->second));
				return std::forward_as_tuple(I->second, true);
			}

			rsx_log.trace("FP not found in buffer!");

			lock.upgrade();
			std::tie(it, recompile) = m_fragment_shader_cache.try_emplace(rsx_fp);
			new_shader = &(it->second);
#ifdef RPCS3_PS4
			if (recompile)
			{
				ps4_compiling(new_shader, true);

				if constexpr (ps4_has_async_translate)
				{
					if (ps4_async && cache_hint && ps4_translate_async_on())
					{
						// The key owns its microcode before a worker reads it; the id is final at once.
						it->first.clone_data();
						new_shader->id = static_cast<decltype(new_shader->id)>(m_next_id++);
						const RSXFragmentProgram* key = &it->first;
						fragment_program_type* shader = new_shader;
						ps4_post_translation(shader, [key, shader]()
						{
							backend_traits::ps4_translate_fragment_program(*key, *shader);
						});
						recompile = false;
					}
				}
			}
#endif
		}

		if (recompile)
		{
			it->first.clone_data();
#ifdef RPCS3_PS4
			const auto ps4_start = std::chrono::steady_clock::now();
#endif
			backend_traits::recompile_fragment_program(rsx_fp, *new_shader, m_next_id++);
#ifdef RPCS3_PS4
			if (cache_hint)
			{
				ps4_note_rsx_translation(ps4_start);
			}

			ps4_compiling(new_shader, false);
#endif
		}

		rsx::program_cache_hint_t::cache_fragment_program(cache_hint, rsx_fp, new_shader);
		return std::forward_as_tuple(*new_shader, false);
	}

public:

	struct program_buffer_patch_entry
	{
		union
		{
			u32 hex_key;
			f32 fp_key;
		};

		union
		{
			u32 hex_value;
			f32 fp_value;
		};

		program_buffer_patch_entry() = default;

		program_buffer_patch_entry(f32 key, f32 value)
		{
			fp_key = key;
			fp_value = value;
		}

		program_buffer_patch_entry(u32 key, u32 value)
		{
			hex_key = key;
			hex_value = value;
		}

		bool test_and_set(f32 value, f32* dst) const
		{
			u32 hex = std::bit_cast<u32>(value);
			if ((hex & 0x7FFFFFFF) == (hex_key & 0x7FFFFFFF))
			{
				hex = (hex & ~0x7FFFFFF) | hex_value;
				*dst = std::bit_cast<f32>(hex);
				return true;
			}

			return false;
		}
	};

public:
	program_state_cache() = default;
	~program_state_cache()
	{}

	template<typename... Args>
	pipeline_data_type get_graphics_pipeline(
		rsx::program_cache_hint_t* cache_hint,
		const RSXVertexProgram& vertex_shader,
		const RSXFragmentProgram& fragment_shader,
		pipeline_properties& pipeline_properties,
		bool compile_async,
		bool allow_notification,
		Args&& ...args
	)
	{
#ifdef RPCS3_PS4
		// 0.39: with asynchronous pipelines a new program may also be translated off this thread (Vulkan,
		// shader-translate-async.flag); the synchronous recompiler keeps translating inline.
		const auto& vp_search = search_vertex_program(cache_hint, vertex_shader, compile_async);
		const auto& fp_search = search_fragment_program(cache_hint, fragment_shader, compile_async);
#else
		const auto& vp_search = search_vertex_program(cache_hint, vertex_shader);
		const auto& fp_search = search_fragment_program(cache_hint, fragment_shader);
#endif

		const bool already_existing_fragment_program = std::get<1>(fp_search);
		const bool already_existing_vertex_program = std::get<1>(vp_search);
		const vertex_program_type& vertex_program = std::get<0>(vp_search);
		const fragment_program_type& fragment_program = std::get<0>(fp_search);
		const pipeline_key key = { vertex_program.id, fragment_program.id, pipeline_properties };

		m_cache_miss_flag = true;

#ifdef RPCS3_PS4
		// 0.38: renderer lookups pass the cache hint; the disk preload does not.
		const bool ps4_renderer = cache_hint != nullptr;
		if (ps4_renderer)
		{
			ps4_renderer_tick();
		}

		if (!ps4_module_ready(vertex_program, m_vertex_mutex) || !ps4_module_ready(fragment_program, m_fragment_mutex)) [[unlikely]]
		{
			if (ps4_renderer)
			{
				m_ps4_last_handed = nullptr;
			}

			return { nullptr, &vertex_program, &fragment_program };
		}
#endif

		if (already_existing_vertex_program && already_existing_fragment_program)
		{
			// There is a high chance the pipeline object was compiled if the two shaders already existed before
			backend_traits::validate_pipeline_properties(vertex_program, fragment_program, pipeline_properties);

			reader_lock lock(m_pipeline_mutex);
			if (const auto I = m_storage.find(key); I != m_storage.end())
			{
#ifdef RPCS3_PS4
				// Ready, still compiling, or refused less than pipeline_refused_retry_s ago (not ready).
				if (!I->second.refused_s || !utils::ps4::rsx_programs::refused_retry_due(I->second.refused_s, ps4_time(ps4_renderer)))
				{
					return ps4_hand(I->second, ps4_renderer, vertex_program, fragment_program);
				}
#else
				m_cache_miss_flag = (I->second == __null_pipeline_handle);
				return { I->second.get(), &vertex_program, &fragment_program };
#endif
			}
		}

#ifdef RPCS3_PS4
		{
			std::lock_guard lock(m_pipeline_mutex);

			auto [I, inserted] = m_storage.try_emplace(key);
			if (inserted)
			{
				// Placeholder, so the same pipeline is not linked twice.
				I->second.last_use_s = ps4_renderer ? m_ps4_clock_s : 0;
			}
			else if (!I->second.refused_s || !utils::ps4::rsx_programs::refused_retry_due(I->second.refused_s, ps4_time(ps4_renderer)))
			{
				// Another submission completed or queued it in the mean time.
				return ps4_hand(I->second, ps4_renderer, vertex_program, fragment_program);
			}
			else
			{
				// Refused at least pipeline_refused_retry_s ago: linked again below.
				I->second.refused_s = 0;
			}
		}
#else
		{
			std::lock_guard lock(m_pipeline_mutex);

			// Check if another submission completed in the mean time
			if (const auto I = m_storage.find(key); I != m_storage.end())
			{
				m_cache_miss_flag = (I->second == __null_pipeline_handle);
				return { I->second.get(), &vertex_program, &fragment_program };
			}

			// Insert a placeholder if the key still doesn't exist to avoid re-linking of the same pipeline
			m_storage[key] = std::move(__null_pipeline_handle);
		}
#endif

		rsx_log.notice("Add program (vp id = %d, fp id = %d)", vertex_program.id, fragment_program.id);

		std::function<pipeline_type* (pipeline_storage_type&)> callback;

#ifdef RPCS3_PS4
		// An asynchronous link reports twice (no pipeline yet when queued, then the worker's result, in either order), an
		// inline one once: a missing pipeline in the last report is a refusal.
		std::shared_ptr<std::atomic<u32>> ps4_reports = compile_async ? std::make_shared<std::atomic<u32>>(0) : nullptr;

		if (allow_notification)
		{
			callback = [this, vertex_shader, fragment_shader_ = RSXFragmentProgram::clone(fragment_shader), key, ps4_reports]
			(pipeline_storage_type& pipeline) -> pipeline_type*
			{
				if (!pipeline)
				{
					if (utils::ps4::rsx_programs::null_report_is_refusal(!!ps4_reports, ps4_reports ? ps4_reports->fetch_add(1) : 0))
					{
						ps4_note_refused(key);
					}

					return nullptr;
				}

				rsx_log.success("Program compiled successfully");
				notify_pipeline_compiled(key.properties, vertex_shader, fragment_shader_);

				std::lock_guard lock(m_pipeline_mutex);
				auto& pipe_result = m_storage[key];
				pipe_result.pipeline = std::move(pipeline);
				pipe_result.refused_s = 0;
				return pipe_result.pipeline.get();
			};
		}
		else
		{
			callback = [this, key, ps4_reports](pipeline_storage_type& pipeline) -> pipeline_type*
			{
				if (!pipeline)
				{
					if (utils::ps4::rsx_programs::null_report_is_refusal(!!ps4_reports, ps4_reports ? ps4_reports->fetch_add(1) : 0))
					{
						ps4_note_refused(key);
					}

					return nullptr;
				}

				std::lock_guard lock(m_pipeline_mutex);
				auto& pipe_result = m_storage[key];
				pipe_result.pipeline = std::move(pipeline);
				pipe_result.refused_s = 0;
				return pipe_result.pipeline.get();
			};
		}
#else
		if (allow_notification)
		{
			callback = [this, vertex_shader, fragment_shader_ = RSXFragmentProgram::clone(fragment_shader), key]
			(pipeline_storage_type& pipeline) -> pipeline_type*
			{
				if (!pipeline)
				{
					return nullptr;
				}

				rsx_log.success("Program compiled successfully");
				notify_pipeline_compiled(key.properties, vertex_shader, fragment_shader_);

				std::lock_guard lock(m_pipeline_mutex);
				auto& pipe_result = m_storage[key];
				pipe_result = std::move(pipeline);
				return pipe_result.get();
			};
		}
		else
		{
			callback = [this, key](pipeline_storage_type& pipeline) -> pipeline_type*
			{
				if (!pipeline)
				{
					return nullptr;
				}

				std::lock_guard lock(m_pipeline_mutex);
				auto& pipe_result = m_storage[key];
				pipe_result = std::move(pipeline);
				return pipe_result.get();
			};
		}
#endif

		auto result = backend_traits::build_pipeline(
			vertex_program,                 // VS, must already be decompiled and recompiled above
			fragment_program,               // FS, must already be decompiled and recompiled above
			pipeline_properties,             // Pipeline state
			compile_async,                  // Allow asynchronous compilation
			callback,                       // Insertion and notification callback
			std::forward<Args>(args)...);   // Other arguments

#ifdef RPCS3_PS4
		if (ps4_renderer)
		{
			m_ps4_last_handed = result;
		}
#endif

		return { result, &vertex_program, &fragment_program };
	}

	void fill_fragment_constants_buffer(std::span<f32> dst_buffer, const fragment_program_type& fragment_program, const RSXFragmentProgram& rsx_prog, bool sanitize = false) const
	{
		if (dst_buffer.size_bytes() < (fragment_program.constant_offsets.size() * 16))
		{
			// This can happen if CELL alters the shader after it has been loaded by RSX.
			rsx_log.error("Insufficient constants buffer size passed to fragment program! Corrupt shader?");
			return;
		}

		rsx::write_fragment_constants_to_buffer(dst_buffer, rsx_prog, fragment_program.constant_offsets, sanitize);
	}

	void clear()
	{
		std::scoped_lock lock(m_vertex_mutex, m_fragment_mutex, m_decompiler_mutex, m_pipeline_mutex);

		notify_pipeline_compiled = {};
		m_fragment_shader_cache.clear();
		m_vertex_shader_cache.clear();
		m_storage.clear();
#ifdef RPCS3_PS4
		m_ps4_last_handed = nullptr;

		// Translations left by the workers (dropped with their queue before this): they referenced the programs above.
		std::lock_guard jobs(m_ps4_module_mutex);
		m_ps4_translate_jobs.clear();
#endif
	}
};
