#include "stdafx.h"
#include "Emu/System.h"
#include "Emu/system_utils.hpp"
#include "Emu/VFS.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/PPUModule.h"

#include "Emu/Cell/lv2/sys_fs.h"
#include "cellSysutil.h"
#include "util/init_mutex.hpp"
#include "Utilities/StrUtil.h"

LOG_CHANNEL(cellSysutil);

template<>
void fmt_class_string<CellSysCacheError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(CELL_SYSCACHE_ERROR_ACCESS_ERROR);
			STR_CASE(CELL_SYSCACHE_ERROR_INTERNAL);
			STR_CASE(CELL_SYSCACHE_ERROR_NOTMOUNTED);
			STR_CASE(CELL_SYSCACHE_ERROR_PARAM);
		}

		return unknown;
	});
}

extern lv2_fs_mount_point g_mp_sys_dev_hdd1;

extern std::string get_syscache_state_corruption_indicator_file_path(std::string_view dir_path);

#ifdef RPCS3_PS4
// rpcs3/ps4_fatal.cpp: appends one line to startup.log
void ps4_fatal_note(const char* prefix, const char* detail) noexcept;

static void ps4_syscache_host_error(const char* what, const std::string& path)
{
	const fs::error err = fs::g_tls_error;

	cellSysutil.error("cellSysCache: failed to %s (path='%s', reason='%s')", what, path, err);

	// startup.log takes the first few; a game that retries in a loop must not flood it with synced writes
	static atomic_t<u32> s_noted = 0;

	if (s_noted++ < 4)
	{
		ps4_fatal_note("cellSysCache: host file error, the cache is not used: ", fmt::format("%s %s (%s)", what, path, err).c_str());
	}
}
#endif

struct syscache_info
{
	const std::string cache_root = rpcs3::utils::get_hdd1_dir() + "/caches/";

	stx::init_mutex init;

	std::string cache_id;

	bool retain_caches = false;

	syscache_info() noexcept
	{
		// Check if dev_hdd1 is mounted by parent process
		if (!Emu.hdd1.empty())
		{
			const auto lock = init.init();

			// Extract cache id from path
			std::string_view id = Emu.hdd1;
			id = id.substr(0, id.find_last_not_of(fs::delim) + 1);
			id = id.substr(id.find_last_of(fs::delim) + 1);
			cache_id = std::string{id};

			if (!Emu.DeserialManager() && !fs::write_file<true>(get_syscache_state_corruption_indicator_file_path(Emu.hdd1), fs::write_new))
			{
#ifdef RPCS3_PS4
				// 0.33: without the indicator an interrupted session would leave this cache looking complete to the next
				// boot, so it is not relayed: it is removed and the id dropped. The game's cellSysCacheMount then
				// creates a fresh cache (CELL_SYSCACHE_RET_OK_CLEARED) or gets CELL_SYSCACHE_ERROR_ACCESS_ERROR.
				ps4_syscache_host_error("create the corruption indicator of the parent's cache", Emu.hdd1);
				clear(true);
				cache_id.clear();
				return;
#else
				fmt::throw_exception("Failed to create HDD1 corruption indicator file! (path='%s', reason='%s')", Emu.hdd1, fs::g_tls_error);
#endif
			}

			cellSysutil.success("Retained cache from parent process: %s", Emu.hdd1);
			return;
		}

		// Find existing cache at startup
		const std::string prefix = Emu.GetTitleID() + '_';

		for (auto&& entry : fs::dir(cache_root))
		{
			if (entry.is_directory && entry.name.starts_with(prefix))
			{
				cache_id = vfs::unescape(entry.name);

				if (fs::is_file(get_syscache_state_corruption_indicator_file_path(cache_root + '/' + cache_id)))
				{
					// State is not complete
					clear(true);
					cache_id.clear();
					continue;
				}

				cellSysutil.notice("Retained cache from past data: %s", cache_root + '/' + cache_id);
				break;
			}
		}
	}

	void clear(bool remove_root, bool lock = false) const noexcept
	{
		// Clear cache
		if (!vfs::host::remove_all(cache_root + cache_id, cache_root, &g_mp_sys_dev_hdd1, remove_root, lock))
		{
			cellSysutil.fatal("cellSysCache: failed to clear cache directory '%s%s' (%s)", cache_root, cache_id, fs::g_tls_error);
		}

		// Poison opened files in /dev_hdd1 to return CELL_EIO on access
		if (remove_root)
		{
			idm::select<lv2_fs_object, lv2_file>([](u32 /*id*/, lv2_file& file)
			{
				if (file.file && file.mp->flags & lv2_mp_flag::cache)
				{
					file.lock = 2;
				}
			});
		}
	}

	~syscache_info() noexcept
	{
		if (cache_id.empty())
		{
			return;
		}

		if (!retain_caches)
		{
			vfs::host::remove_all(cache_root + cache_id, cache_root, &g_mp_sys_dev_hdd1, true, false, true);
			return;
		}

		idm::select<lv2_fs_object, lv2_file>([](u32 /*id*/, lv2_file& file)
		{
			if (file.file && file.mp->flags & lv2_mp_flag::cache && file.flags & CELL_FS_O_ACCMODE)
			{
				file.file.sync();
			}
		});

		fs::remove_file(get_syscache_state_corruption_indicator_file_path(cache_root + cache_id));
	}
};

extern std::string get_syscache_state_corruption_indicator_file_path(std::string_view dir_path)
{
	constexpr std::u8string_view append_path = u8"/＄hdd0_temp_state_indicator";
	const std::string_view filename = reinterpret_cast<const char*>(append_path.data());

	if (dir_path.empty())
	{
		return rpcs3::utils::get_hdd1_dir() + "/caches/" + ensure(g_fxo->try_get<syscache_info>())->cache_id + "/" + filename.data();
	}

	return std::string{dir_path} + filename.data();
}

extern void signal_system_cache_can_stay()
{
	ensure(g_fxo->try_get<syscache_info>())->retain_caches = true;
}

error_code cellSysCacheClear()
{
	cellSysutil.notice("cellSysCacheClear()");

	auto& cache = g_fxo->get<syscache_info>();

	const auto lock = cache.init.access();

	if (!lock)
	{
		return CELL_SYSCACHE_ERROR_NOTMOUNTED;
	}

	// Clear existing cache
	if (!cache.cache_id.empty())
	{
		std::lock_guard lock0(g_mp_sys_dev_hdd1.mutex);
		cache.clear(false);
	}

	return not_an_error(CELL_SYSCACHE_RET_OK_CLEARED);
}

error_code cellSysCacheMount(vm::ptr<CellSysCacheParam> param)
{
	cellSysutil.notice("cellSysCacheMount(param=*0x%x ('%s'))", param, param.ptr(&CellSysCacheParam::cacheId));

	auto& cache = g_fxo->get<syscache_info>();

	if (!param)
	{
		return CELL_SYSCACHE_ERROR_PARAM;
	}

	std::string cache_name;

	ensure(vm::read_string(param.ptr(&CellSysCacheParam::cacheId).addr(), sizeof(param->cacheId), cache_name), "Access violation");

	if (!cache_name.empty() && sysutil_check_name_string(cache_name.data(), 1, CELL_SYSCACHE_ID_SIZE) != 0)
	{
		return CELL_SYSCACHE_ERROR_PARAM;
	}

	// Full virtualized cache id (with title id included)
	std::string cache_id = vfs::escape(Emu.GetTitleID() + '_' + cache_name);

	// Full path to virtual cache root (/dev_hdd1)
	std::string new_path = cache.cache_root + cache_id + '/';

	// Set fixed VFS path
	strcpy_trunc(param->getCachePath, "/dev_hdd1");

	// Lock pseudo-mutex
	const auto lock = cache.init.init_always([&]
	{
	});

	std::lock_guard lock0(g_mp_sys_dev_hdd1.mutex);

	// Check if can reuse existing cache (won't if cache id is an empty string or cache is damaged/incomplete)
	if (!cache_name.empty() && cache_id == cache.cache_id)
	{
		// Isn't mounted yet on first call to cellSysCacheMount
		if (vfs::mount("/dev_hdd1", new_path))
			g_fxo->get<lv2_fs_mount_info_map>().add("/dev_hdd1", &g_mp_sys_dev_hdd1);

		cellSysutil.success("Mounted existing cache at %s", new_path);
		return not_an_error(CELL_SYSCACHE_RET_OK_RELAYED);
	}

	const bool can_create = cache.cache_id != cache_id || !cache.cache_id.empty();

	if (!cache.cache_id.empty())
	{
		// Clear previous cache
		cache.clear(true);
	}

	// Set new cache id
	cache.cache_id = std::move(cache_id);

	if (can_create)
	{
		const bool created = fs::create_dir(new_path);

		if (!created)
		{
			if (fs::g_tls_error != fs::error::exist)
			{
#ifdef RPCS3_PS4
				// 0.33: this runs under the hdd1 mount lock, which a PPU thread that ends here would keep for the rest of
				// the process (no unwinding on this console). The game gets the error instead; the id is dropped so a
				// later mount takes this create path again rather than relaying a directory that does not exist.
				ps4_syscache_host_error("create the cache directory", new_path);
				cache.cache_id.clear();
				return CELL_SYSCACHE_ERROR_ACCESS_ERROR;
#else
				fmt::throw_exception("Failed to create HDD1 cache! (path='%s', reason='%s')", new_path, fs::g_tls_error);
#endif
			}

			// Clear new cache
			cache.clear(false);
		}
	}

	if (!fs::write_file<true>(get_syscache_state_corruption_indicator_file_path(new_path), fs::write_new))
	{
#ifdef RPCS3_PS4
		// 0.33: as above. The directory is removed too: without the indicator it would be relayed to the next boot as a
		// complete cache.
		ps4_syscache_host_error("create the corruption indicator", new_path);
		cache.clear(true);
		cache.cache_id.clear();
		return CELL_SYSCACHE_ERROR_ACCESS_ERROR;
#else
		fmt::throw_exception("Failed to create HDD1 corruption indicator file! (path='%s', reason='%s')", new_path, fs::g_tls_error);
#endif
	}

	if (vfs::mount("/dev_hdd1", new_path))
		g_fxo->get<lv2_fs_mount_info_map>().add("/dev_hdd1", &g_mp_sys_dev_hdd1);

	return not_an_error(CELL_SYSCACHE_RET_OK_CLEARED);
}


extern void cellSysutil_SysCache_init()
{
	REG_FUNC(cellSysutil, cellSysCacheMount);
	REG_FUNC(cellSysutil, cellSysCacheClear);
}
