#include "stdafx.h"
#include "cache_utils.hpp"
#include "system_utils.hpp"
#include "system_config.h"
#include "IdManager.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/PPUAnalyser.h"
#include "Emu/Cell/PPUThread.h"

#ifdef RPCS3_PS4
#include "util/ps4_disc_probe.hpp"
#include "util/ps4_safe_mode.hpp"
#endif

LOG_CHANNEL(sys_log, "SYS");

#ifdef RPCS3_PS4
// Trims dev_hdd1/caches in the order of utils::ps4::disk_cache (the most recently used game caches stay) and says what
// was really removed. 'file_list': the folder's entries without "." and ".."; 'size': their total.
static void ps4_trim_disk_cache(const std::string& cache_location, const std::vector<fs::dir_entry>& file_list, u64 size, u64 max_size)
{
	namespace dc = utils::ps4::disk_cache;

	std::vector<dc::entry> entries;
	entries.reserve(file_list.size());

	for (const auto& item : file_list)
	{
		entries.push_back({item.name, item.mtime, item.is_directory});
	}

	std::vector<usz> kept_index;
	const std::vector<usz> order = dc::eviction_order(entries, dc::protected_newest, &kept_index);
	const u64 to_remove = dc::bytes_to_remove(size, max_size);

	u64 removed = 0;
	std::vector<std::string> removed_names;
	std::string failure;

	for (const usz index : order)
	{
		if (removed >= to_remove)
		{
			break;
		}

		const auto& item = file_list[index];
		const std::string name = cache_location + "/" + item.name;
		const bool is_dir = fs::is_dir(name);
		const u64 item_size = is_dir ? fs::get_dir_size(name) : item.size;

		if (is_dir && item_size == umax)
		{
			failure = fmt::format("; stopped: the size of '%s' could not be read (%s)", item.name, fs::g_tls_error);
			break;
		}

		if (is_dir ? !fs::remove_all(name, true, true) : !fs::remove_file(name))
		{
			failure = fmt::format("; stopped: '%s' could not be removed (%s)", item.name, fs::g_tls_error);
			break;
		}

		removed += item_size;
		removed_names.push_back(item.name);
	}

	std::vector<std::string> kept_names;

	for (const usz index : kept_index)
	{
		kept_names.push_back(file_list[index].name);
	}

	const u64 left = size - std::min(size, removed);
	const std::string line = fmt::format("disk cache: dev_hdd1/caches held %.1f MB, limit %llu MB: removed %.1f MB (%s), %.1f MB left%s; kept the newest: %s%s",
		size / 1048576.0, max_size >> 20, removed / 1048576.0, dc::names_text(removed_names), left / 1048576.0,
		left > max_size ? " (still above the limit: the newest caches are kept)" : "", dc::names_text(kept_names), failure);
	sys_log.success("%s", line);
	ps4_startup_checkpoint(line.c_str());
}
#endif

namespace rpcs3::cache
{
	std::string get_ppu_cache()
	{
		const auto _main = g_fxo->try_get<main_ppu_module<lv2_obj>>();

		if (!_main || _main->cache.empty())
		{
			ppu_log.error("PPU Cache location not initialized.");
			return {};
		}

		return _main->cache;
	}

	void limit_cache_size()
	{
		const std::string cache_location = rpcs3::utils::get_hdd1_dir() + "/caches";

		if (!fs::is_dir(cache_location))
		{
			sys_log.warning("Cache does not exist (%s)", cache_location);
			return;
		}

		const u64 size = fs::get_dir_size(cache_location);

		if (size == umax)
		{
			sys_log.error("Could not calculate cache directory '%s' size (%s)", cache_location, fs::g_tls_error);
			return;
		}

		const u64 max_size = static_cast<u64>(g_cfg.vfs.cache_max_size) * 1024 * 1024;

		if (max_size == 0) // Everything must go, so no need to do checks
		{
			fs::remove_all(cache_location, false);
			sys_log.success("Cleared disk cache");
			return;
		}

		if (size <= max_size)
		{
			sys_log.trace("Cache size below limit: %llu/%llu", size, max_size);
			return;
		}

		sys_log.success("Cleaning disk cache...");
		std::vector<fs::dir_entry> file_list{};
		fs::dir cache_dir(cache_location);
		if (!cache_dir)
		{
			sys_log.error("Could not open cache directory '%s' (%s)", cache_location, fs::g_tls_error);
			return;
		}

		// retrieve items to delete
		for (const auto &item : cache_dir)
		{
			if (item.name != "." && item.name != "..")
				file_list.push_back(item);
		}

		cache_dir.close();

#ifdef RPCS3_PS4
		// disk-cache-upstream.flag keeps the order below (oldest first, any folder)
		if (::utils::ps4::safe_mode::enabled(::utils::ps4::safe_mode::feature::disk_cache_order))
		{
			ps4_trim_disk_cache(cache_location, file_list, size, max_size);
			return;
		}
#endif

		// sort oldest first
		std::sort(file_list.begin(), file_list.end(), FN(x.mtime < y.mtime));

		// keep removing until cache is empty or enough bytes have been cleared
		// cache is cleared down to 80% of limit to increase interval between clears
		const u64 to_remove = static_cast<u64>(size - max_size * 0.8);
		u64 removed = 0;
		for (const auto &item : file_list)
		{
			const std::string &name = cache_location + "/" + item.name;
			const bool is_dir = fs::is_dir(name);
			const u64 item_size = is_dir ? fs::get_dir_size(name) : item.size;

			if (is_dir && item_size == umax)
			{
				sys_log.error("Failed to calculate '%s' item '%s' size (%s)", cache_location, item.name, fs::g_tls_error);
				break;
			}

			if (is_dir ? !fs::remove_all(name, true, true) : !fs::remove_file(name))
			{
				sys_log.error("Could not remove cache directory '%s' item '%s' (%s)", cache_location, item.name, fs::g_tls_error);
				break;
			}

			removed += item_size;
			if (removed >= to_remove)
				break;
		}

		sys_log.success("Cleaned disk cache, removed %.2f MB", size / 1024.0 / 1024.0);
	}
}
