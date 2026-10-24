#include "state_tracker.hpp"
#include "vao.hpp"

namespace gl
{
	static thread_local bool s_tls_primary_context_thread = false;
	static gl::driver_state* s_current_state = nullptr;

	void set_primary_context_thread(bool value)
	{
		s_tls_primary_context_thread = value;
	}

	bool is_primary_context_thread()
	{
		return s_tls_primary_context_thread;
	}

	void set_command_context(gl::command_context& ctx)
	{
		s_current_state = ctx.operator->();
	}

	void set_command_context(gl::driver_state& ctx)
	{
		s_current_state = &ctx;
	}

#ifdef RPCS3_PS4
	static bool (*s_ps4_vram_relief)() = nullptr;

	void ps4_set_vram_relief(bool (*relief)())
	{
		s_ps4_vram_relief = relief;
	}

	bool ps4_vram_relief()
	{
		return s_ps4_vram_relief && s_ps4_vram_relief();
	}

	void ps4_clear_command_context()
	{
		s_current_state = nullptr;
	}

	bool ps4_has_command_context()
	{
		return s_current_state != nullptr;
	}

	gl::command_context get_command_context()
	{
		if (!s_current_state) [[unlikely]]
		{
			// Bookkeeping only (bind tables): nothing reaches a destroyed renderer
			static gl::driver_state s_detached;
			return { s_detached };
		}

		return { *s_current_state };
	}
#else
	gl::command_context get_command_context()
	{
		return { *s_current_state };
	}
#endif

	attrib_t vao::operator[](u32 index) const noexcept
	{
		return attrib_t(index);
	}
}
