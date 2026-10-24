#include "util/logs.hpp"
#include "Utilities/File.h"
#include "Utilities/mutex.h"
#include "Utilities/Thread.h"
#include "Utilities/StrFmt.h"
#include <cstring>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include <thread>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <regex>

using namespace std::literals::chrono_literals;

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#include <zlib.h>

#ifdef __ORBIS__
#include "util/ps4_backoff.hpp"
#include "util/ps4_lifecycle.hpp"

#include <atomic>
#include <cstdio>
#include <string_view>
#include <vector>
#include <unistd.h>

// rpcs3/ps4_fatal.cpp: appends one line to startup.log with plain POSIX calls, never through this logger, so the Log
// Writer may call it while it holds m_m.
void ps4_fatal_note(const char* prefix, const char* detail) noexcept;

namespace
{
	// 0.40: the repeated-line limiter of the live log (util/ps4_lifecycle.hpp). Constant-initialized, so a line written
	// before main finds it ready.
	utils::ps4::repeat_limiter s_repeat_limiter;

	// 0 = log-flood-off.flag not checked yet, 1 = the limiter acts, 2 = off (every line is written)
	std::atomic<int> s_repeat_mode{0};

	bool repeat_limiter_on() noexcept
	{
		int mode = s_repeat_mode.load(std::memory_order_relaxed);

		if (mode == 0)
		{
			mode = ::access("/data/rpcs3/log-flood-off.flag", F_OK) == 0 ? 2 : 1;
			s_repeat_mode.store(mode, std::memory_order_relaxed);
		}

		return mode == 1;
	}

	// Never limited: the always and fatal levels, the PS4 channels (heartbeat, session lines, the copies of startup.log
	// lines), which a report needs whole, and sys_fs: its per-call lines are the record of which files a game opened
	// (a missing language file, a movie, an I/O error), one per open, and a long load opens thousands.
	bool repeat_exempt(const logs::message& msg) noexcept
	{
		const logs::level level = msg;

		if (level == logs::level::always || level == logs::level::fatal)
		{
			return true;
		}

		const char* const name = msg->name;
		return name && ((name[0] == 'P' && name[1] == 'S' && name[2] == '4') || std::strcmp(name, "sys_fs") == 0);
	}

	// True for "<prefix><digits>.log".
	bool is_log_part(std::string_view entry, std::string_view prefix)
	{
		if (entry.size() < prefix.size() + 5 || !entry.starts_with(prefix) || !entry.ends_with(".log"))
		{
			return false;
		}

		const std::string_view digits = entry.substr(prefix.size(), entry.size() - prefix.size() - 4);

		if (digits.size() > 9)
		{
			return false;
		}

		for (const char c : digits)
		{
			if (c < '0' || c > '9')
			{
				return false;
			}
		}

		return true;
	}

	// 0.34: <stem>.part<N>.log files of the previous session become previous-<stem>.part<N>.log (the older previous
	// parts are removed first), so every <stem>.part<N>.log belongs to the running session.
	void retire_log_parts(const std::string& name)
	{
		const usz slash = name.find_last_of('/');
		const std::string dir = slash == umax ? std::string() : name.substr(0, slash + 1);
		std::string stem = slash == umax ? name : name.substr(slash + 1);

		if (stem.ends_with(".log"))
		{
			stem.resize(stem.size() - 4);
		}

		const std::string part = stem + ".part";
		const std::string previous = "previous-" + part;
		std::vector<std::string> current;
		std::vector<std::string> old;

		{
			fs::dir listing;

			if (!listing.open(dir.empty() ? std::string(".") : dir))
			{
				return;
			}

			for (fs::dir_entry entry; listing.read(entry) && current.size() + old.size() < 128;)
			{
				if (entry.is_directory)
				{
					continue;
				}

				if (is_log_part(entry.name, part))
				{
					current.push_back(entry.name);
				}
				else if (is_log_part(entry.name, previous))
				{
					old.push_back(entry.name);
				}
			}
		}

		for (const std::string& entry : old)
		{
			fs::remove_file(dir + entry);
		}

		for (const std::string& entry : current)
		{
			fs::rename(dir + entry, dir + "previous-" + entry, true);
		}
	}
}
#endif

static std::string default_string()
{
	if (thread_ctrl::is_main())
	{
		return {};
	}

	return fmt::format("TID: %u", thread_ctrl::get_tid());
}

// Thread-specific log prefix provider
thread_local std::string(*g_tls_log_prefix)() = &default_string;

// Another thread-specific callback
thread_local void(*g_tls_log_control)(const char* fmt, u64 progress) = [](const char*, u64){};

template<>
void fmt_class_string<logs::level>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto lev)
	{
		switch (lev)
		{
		case logs::level::always: return "Nothing";
		case logs::level::fatal: return "Fatal";
		case logs::level::error: return "Error";
		case logs::level::todo: return "TODO";
		case logs::level::success: return "Success";
		case logs::level::warning: return "Warning";
		case logs::level::notice: return "Notice";
		case logs::level::trace: return "Trace";
		}

		return unknown;
	});
}

namespace logs
{
	static_assert(std::is_empty_v<message> && sizeof(message) == 1);
	static_assert(sizeof(channel) == alignof(channel));
	static_assert(uchar(level::always) == 0);
	static_assert(uchar(level::fatal) == 1);
	static_assert(uchar(level::trace) == 7);
	static_assert((offsetof(channel, fatal) & 7) == 1);
	static_assert((offsetof(channel, trace) & 7) == 7);

	// Memory-mapped buffer size
#ifdef __ORBIS__
	// The ring is committed in full (value-initialized) inside the 2 GiB CPU heap; 8 MiB still absorbs the bursts.
	constexpr u64 s_log_size = 8 * 1024 * 1024;
#else
	constexpr u64 s_log_size = 32 * 1024 * 1024;
#endif
	static_assert(s_log_size * s_log_size > s_log_size && (s_log_size & (s_log_size - 1)) == 0); // Assert on an overflowing value

	class file_writer
	{
		std::thread m_writer{};
		fs::file m_fout{};
		fs::file m_fout2{};
		u64 m_max_size{};

		std::unique_ptr<uchar[]> m_fptr{};
		z_stream m_zs{};
		shared_mutex m_m{};

		atomic_t<u64, 128> m_buf{0}; // MSB (39 bits): push begin, LSB (25 bis): push size
		atomic_t<u64, 128> m_out{0}; // Amount of bytes written to file

		uchar m_zout[65536]{};

#ifdef __ORBIS__
		// 0.34: m_max_size is the size of one file, not of the session: at that size the file becomes
		// <stem>.part<N>.log and a new one starts, so the tail before a late crash always exists.
		std::string m_name{};        // the log file
		std::string m_part_prefix{}; // "<dir>/<stem>.part"
		u64 m_file_start = 0;        // m_out when the current file was started
		u32 m_parts = 0;             // files finished in this session

		// Under m_m.
		void rotate();
#endif

		// Write buffered logs immediately
		bool flush(u64 bufv);

	public:
		file_writer(const std::string& name, u64 max_size);

		virtual ~file_writer();

		// Append raw data
		void log(const char* text, usz size);

		// Ensure written to disk
		void sync();

		// Close file handle after flushing to disk
		void close_prematurely();
	};

	struct file_listener final : file_writer, public listener
	{
		file_listener(const std::string& path, u64 max_size);

		~file_listener() override = default;

		void log(u64 stamp, const message& msg, std::string_view prefix, std::string_view text) override;

		void sync() override
		{
			file_writer::sync();
		}

		void close_prematurely() override
		{
			file_writer::close_prematurely();
		}
	};

	struct root_listener final : public listener
	{
		root_listener() = default;

		~root_listener() override = default;

		// Encode level, current thread name, channel name and write log message
		void log(u64, const message&, std::string_view, std::string_view) override
		{
			// Do nothing
		}

		// Channel registry
		std::unordered_multimap<std::string, channel*> channels{};

		// Messages for delayed listener initialization
		std::vector<stored_message> messages{};
	};

	static root_listener* get_logger()
	{
		// Use magic static
		static root_listener logger{};
		return &logger;
	}

	static u64 get_stamp()
	{
		static struct time_initializer
		{
#ifdef _WIN32
			LARGE_INTEGER freq;
			LARGE_INTEGER start;

			time_initializer()
			{
				QueryPerformanceFrequency(&freq);
				QueryPerformanceCounter(&start);
			}
#else
			steady_clock::time_point start = steady_clock::now();
#endif

			u64 get() const
			{
#ifdef _WIN32
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				const LONGLONG diff = now.QuadPart - start.QuadPart;
				return diff / freq.QuadPart * 1'000'000 + diff % freq.QuadPart * 1'000'000 / freq.QuadPart;
#else
				return (steady_clock::now() - start).count() / 1000;
#endif
			}
		} timebase{};

		return timebase.get();
	}

	// Channel registry mutex
	static shared_mutex g_mutex;

	// Must be set to true in main()
	static atomic_t<bool> g_init{false};

	void reset()
	{
		std::lock_guard lock(g_mutex);

		for (auto&& pair : get_logger()->channels)
		{
			pair.second->enabled.release(level::_default);
		}
	}

	void silence()
	{
		std::lock_guard lock(g_mutex);

		for (auto&& pair : get_logger()->channels)
		{
			pair.second->enabled.release(level::always);
		}
	}

	void set_level(const std::string& ch_name, level value)
	{
		std::lock_guard lock(g_mutex);

		if (ch_name.find_first_of(".+*?^$()[]{}|\\") != umax)
		{
			const std::regex ex(ch_name);

			// RegEx pattern
			for (auto& channel_pair : get_logger()->channels)
			{
				std::smatch sm;

				if (std::regex_match(channel_pair.first, sm, ex))
				{
					channel_pair.second->enabled.release(value);
				}
			}

			return;
		}

		auto found = get_logger()->channels.equal_range(ch_name);

		while (found.first != found.second)
		{
			found.first->second->enabled.release(value);
			found.first++;
		}
	}

	level get_level(const std::string& ch_name)
	{
		std::lock_guard lock(g_mutex);

		const auto found = get_logger()->channels.equal_range(ch_name);

		if (found.first != found.second)
		{
			return found.first->second->enabled.observe();
		}

		return level::always;
	}

	void set_channel_levels(const std::map<std::string, logs::level, std::less<>>& map)
	{
		for (auto&& pair : map)
		{
			logs::set_level(pair.first, pair.second);
		}
	}

	std::set<std::string> get_channels()
	{
		std::set<std::string> result;

		std::lock_guard lock(g_mutex);

		for (auto&& p : get_logger()->channels)
		{
			if (!p.first.empty())
			{
				result.insert(p.first);
			}
		}

		return result;
	}

	// Must be called in main() to stop accumulating messages in g_messages
	void set_init(std::initializer_list<stored_message> init_msg)
	{
		if (!g_init)
		{
			std::lock_guard lock(g_mutex);

			// Prepend main messages
			for (const auto& msg : init_msg)
			{
				get_logger()->broadcast(msg);
			}

			// Send initial messages
			for (const auto& msg : get_logger()->messages)
			{
				get_logger()->broadcast(msg);
			}

			// Clear it
#ifdef RPCS3_PS4
			// Release the storage too: the vector is never used again
			std::vector<stored_message>().swap(get_logger()->messages);
#else
			get_logger()->messages.clear();
#endif
			g_init = true;
		}
	}
}

logs::listener::~listener()
{
	// Shut up all channels on exit
	if (auto logger = get_logger())
	{
		if (logger == this)
		{
			return;
		}

		for (auto&& pair : logger->channels)
		{
			pair.second->enabled.release(level::always);
		}
	}
}

void logs::listener::add(logs::listener* _new)
{
	// Get first (main) listener
	listener* lis = get_logger();

	std::lock_guard lock(g_mutex);

	// Install new listener at the end of linked list
	listener* null = nullptr;

	while (lis->m_next || !lis->m_next.compare_exchange(null, _new))
	{
		lis = lis->m_next;
		null = nullptr;
	}
}

void logs::listener::broadcast(const logs::stored_message& msg) const
{
	for (auto lis = m_next.load(); lis; lis = lis->m_next)
	{
		lis->log(msg.stamp, msg.m, msg.prefix, msg.text);
	}
}

void logs::listener::sync()
{
}

void logs::listener::close_prematurely()
{
}

void logs::listener::sync_all()
{
	for (listener* lis = get_logger(); lis; lis = lis->m_next)
	{
		lis->sync();
	}
}

void logs::listener::shutdown_all()
{
	std::lock_guard lock(g_mutex);

	for (listener* lis = get_logger()->m_next.exchange(nullptr); lis;)
	{
		lis = lis->m_next.exchange(nullptr);
	}
}

void logs::listener::close_all_prematurely()
{
	for (listener* lis = get_logger(); lis; lis = lis->m_next)
	{
		lis->close_prematurely();
	}
}

logs::registerer::registerer(channel& _ch)
{
	std::lock_guard lock(g_mutex);

	get_logger()->channels.emplace(_ch.name, &_ch);
}

void logs::message::broadcast(const char* fmt, const fmt_type_info* sup, ...) const
{
	// Get timestamp
	const u64 stamp = get_stamp();

#ifdef __ORBIS__
	// 0.40: a message that repeats without end is cut down here, before anything is formatted: after its first 64
	// lines, 8 per 10 s pass and the rest is counted in one summary line per window.
	if (g_init && !repeat_exempt(*this) && repeat_limiter_on())
	{
		const u64 key = reinterpret_cast<uptr>(fmt) * 0x9e3779b97f4a7c15ull ^ reinterpret_cast<uptr>(this);
		const auto verdict = s_repeat_limiter.admit(key, fmt, this, stamp);

		if (verdict.summary)
		{
			const std::string summary = utils::ps4::repeat_summary_line(verdict.summary, verdict.sample);

			for (listener* lis = get_logger(); lis; lis = lis->m_next)
			{
				lis->log(stamp, *this, {}, summary);
			}
		}

		if (!verdict.pass)
		{
			return;
		}
	}
#endif

	// Notify start operation
	g_tls_log_control(fmt, 0);

	// Get text, extract va_args
	thread_local std::string text;
	thread_local std::vector<u64> args;

	static constexpr fmt_type_info empty_sup{};

	usz args_count = 0;
	for (auto v = sup; v && v->fmt_string; v++)
		args_count++;

	text.clear();
	args.resize(args_count);

	va_list c_args;
	va_start(c_args, sup);
	for (u64& arg : args)
		arg = va_arg(c_args, u64);
	va_end(c_args);
	fmt::raw_append(text, fmt, sup ? sup : &empty_sup, args.data());
	std::string prefix = g_tls_log_prefix();

	// Get first (main) listener
	listener* lis = get_logger();

	if (!g_init)
	{
		std::lock_guard lock(g_mutex);

		if (!g_init)
		{
			while (lis)
			{
				lis->log(stamp, *this, prefix, text);
				lis = lis->m_next;
			}

			// Store message additionally
			get_logger()->messages.emplace_back(stored_message{*this, stamp, std::move(prefix), text});
		}
	}

	// Send message to all listeners
	while (lis)
	{
		lis->log(stamp, *this, prefix, text);
		lis = lis->m_next;
	}

	// Notify end operation
	g_tls_log_control(fmt, -1);
}

#ifdef __ORBIS__
namespace logs
{
	// 0.40: the frontend's main loop calls this about once per second (rpcs3/ps4_main.cpp). It writes the summary of a
	// window that ended with lines left out and no later line of its message, and one line (also to startup.log, for
	// the first 32) when the limiter starts acting on a message, all on the main thread.
	void ps4_flush_repeated_lines()
	{
		if (!g_init)
		{
			return;
		}

		if (!repeat_limiter_on())
		{
			static bool s_off_told = false;

			if (!std::exchange(s_off_told, true))
			{
				ps4_fatal_note("log: ", "repeated-line limiter off (log-flood-off.flag): every repeated line is written");
			}

			return;
		}

		static u32 s_limited_told = 0;
		const u64 stamp = get_stamp();

		s_repeat_limiter.flush(stamp,
			[&](const char* text, const void* tag)
			{
				const message& msg = *static_cast<const message*>(tag);
				const std::string line = utils::ps4::repeat_limited_line(text);

				if (s_limited_told < 32)
				{
					const std::string prefix = msg->name && *msg->name ? std::string(msg->name) + ": " : std::string();
					ps4_fatal_note(prefix.c_str(), line.c_str());
				}
				else if (s_limited_told == 32)
				{
					ps4_fatal_note("log: ", "more messages reached the repeated-line limit; their lines are in RPCS3.log only");
				}

				s_limited_told++;
				get_logger()->broadcast(stored_message{msg, stamp, std::string(), line});
			},
			[&](const char* text, const void* tag, std::uint32_t count)
			{
				get_logger()->broadcast(stored_message{*static_cast<const message*>(tag), stamp, std::string(), utils::ps4::repeat_summary_line(count, text)});
			});
	}
}
#endif

logs::file_writer::file_writer(const std::string& name, u64 max_size)
	: m_max_size(max_size)
{
	if (name.empty() || !max_size)
	{
		return;
	}

	// Initialize ringbuffer
	m_fptr = std::make_unique<uchar[]>(s_log_size);

#ifdef __ORBIS__
	// 0.34: names for rotate(); parts of the previous session are kept once as previous-<stem>.part<N>.log.
	m_name = name;
	m_part_prefix = name.ends_with(".log") ? name.substr(0, name.size() - 4) : name;
	m_part_prefix += ".part";
	retire_log_parts(name);
#endif

	// Actual log file (allowed to fail)
	if (!m_fout.open(name, fs::rewrite))
	{
		fprintf(stderr, "Log file open failed: %s (error %d)\n", name.c_str(), errno);
	}

#ifdef __ORBIS__
	// 0.33: no compressed copy on the PS4. It doubled the log writes to /data, the log history keeps the plain file,
	// and the app usually ends without running the destructor, which left the .gz unreadable (mode 0) over FTP. A
	// leftover from an older build is removed so it is never mistaken for this session's log.
	fs::remove_file(name + ".gz");
#else
	// Compressed log, make it inaccessible (foolproof)
	if (m_fout2.open(name + ".gz", fs::rewrite + fs::unread))
	{
#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
		if (deflateInit2(&m_zs, 9, Z_DEFLATED, 16 + 15, 9, Z_DEFAULT_STRATEGY) != Z_OK)
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif
		{
			m_fout2.close();
		}
	}

	if (!m_fout2)
	{
		fprintf(stderr, "Log file open failed: %s.gz (error %d)\n", name.c_str(), errno);
	}
#endif

#ifdef _WIN32
	// Autodelete compressed log file
	FILE_DISPOSITION_INFO disp{};
	disp.DeleteFileW = true;
	SetFileInformationByHandle(m_fout2.get_handle(), FileDispositionInfo, &disp, sizeof(disp));
#endif

	m_writer = std::thread([this]()
	{
		thread_base::set_name("Log Writer");

#ifndef __ORBIS__
		thread_ctrl::scoped_priority low_prio(-1);
#else
		// Orbis schedules fixed priorities: a writer below the emulator threads was starved by
		// spinning PPU/SPU threads, and every logs::listener::sync() caller (cellSaveData, fatal
		// paths) then waited forever (0.20 autosave freeze). Keep the inherited priority.
		utils::progress_backoff busy_backoff;
#endif

		while (true)
		{
			const u64 bufv = m_buf;

			if (bufv % s_log_size)
			{
				// Wait if threads are writing logs
#ifdef __ORBIS__
				// A producer preempted between reserve and commit must get a core to finish its copy.
				busy_backoff.yield();
#else
				std::this_thread::yield();
#endif
				continue;
			}

#ifdef __ORBIS__
			busy_backoff.reset();
#endif

			if (!flush(bufv))
			{
				if (m_out == umax)
				{
					break;
				}

				std::this_thread::sleep_for(10ms);
			}
		}
	});
}

logs::file_writer::~file_writer()
{
	if (!m_fptr)
	{
		return;
	}

	// Stop writer thread
	file_writer::sync();

	m_out = -1;
	m_writer.join();

	if (m_fout2)
	{
		m_zs.avail_in = 0;
		m_zs.next_in  = nullptr;

		do
		{
			m_zs.avail_out = sizeof(m_zout);
			m_zs.next_out  = m_zout;

			if (deflate(&m_zs, Z_FINISH) == Z_STREAM_ERROR || m_fout2.write(m_zout, sizeof(m_zout) - m_zs.avail_out) != sizeof(m_zout) - m_zs.avail_out)
			{
				break;
			}
		}
		while (m_zs.avail_out == 0);

		deflateEnd(&m_zs);
	}

#ifdef _WIN32
	// Cancel compressed log file auto-deletion
	FILE_DISPOSITION_INFO disp;
	disp.DeleteFileW = false;
	SetFileInformationByHandle(m_fout2.get_handle(), FileDispositionInfo, &disp, sizeof(disp));
#elif !defined(__ORBIS__)
	// Restore compressed log file permissions
	::fchmod(m_fout2.get_handle(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
#endif
}

bool logs::file_writer::flush(u64 bufv)
{
	std::lock_guard lock(m_m);

	const u64 read_pos = m_out;
	const u64 out_index = read_pos % s_log_size;
	const u64 pushed = (bufv / s_log_size) % s_log_size;
#ifdef __ORBIS__
	// 0.34: no session cap; rotate() starts a new file every m_max_size bytes.
	const u64 end = out_index <= pushed ? read_pos - out_index + pushed : ((read_pos + s_log_size) & ~(s_log_size - 1));
#else
	const u64 end = std::min<u64>(out_index <= pushed ? read_pos - out_index + pushed : ((read_pos + s_log_size) & ~(s_log_size - 1)), m_max_size);
#endif

	if (end > read_pos)
	{
		// Avoid writing too big fragments
		const u64 size = std::min<u64>(end - read_pos, sizeof(m_zout) / 2);

		// Write uncompressed
#ifdef __ORBIS__
		// 0.33: a failed write (full /data, a lasting I/O error) only ends this log. The Log Writer is a plain
		// std::thread: anything fatal here would park the app, and every thread that logs would then wait on m_m.
		// unix_file::write reports the failure as a short count with g_tls_error set instead of failing an assertion.
		fs::g_tls_error = fs::error::ok;
#endif
		if (m_fout && m_fout.write(m_fptr.get() + out_index, size) != size)
		{
#ifdef __ORBIS__
			const std::string detail = fmt::format("RPCS3 log write failed at %llu bytes (%s); the log is closed for the rest of this session",
				read_pos, fs::g_tls_error == fs::error::ok ? "short write" : fmt::format("%s", fs::g_tls_error));
			ps4_fatal_note("log: ", detail.c_str());
#endif
			m_fout.close();
		}

		// Write compressed
		if (m_fout2)
		{
			m_zs.avail_in = static_cast<uInt>(size);
			m_zs.next_in  = m_fptr.get() + out_index;

			do
			{
				m_zs.avail_out = sizeof(m_zout);
				m_zs.next_out  = m_zout;

				if (deflate(&m_zs, Z_NO_FLUSH) == Z_STREAM_ERROR || m_fout2.write(m_zout, sizeof(m_zout) - m_zs.avail_out) != sizeof(m_zout) - m_zs.avail_out)
				{
					deflateEnd(&m_zs);
					m_fout2.close();
					break;
				}
			}
			while (m_zs.avail_out == 0);
		}

		m_out += size;

#ifdef __ORBIS__
		if (m_fout && m_out - m_file_start >= m_max_size)
		{
			rotate();
		}
#endif

		return true;
	}

	return false;
}

#ifdef __ORBIS__
void logs::file_writer::rotate()
{
	// The finished file becomes <stem>.part<N>.log and the part two before it is removed, so at most two parts and
	// the current file exist. Paths are formatted into fixed buffers and ps4_fatal_note does not allocate.
	const u32 part = m_parts + 1;
	char finished[512];
	char line[640];
	std::snprintf(finished, sizeof(finished), "%s%u.log", m_part_prefix.c_str(), part);
	m_fout.close();

	if (::rename(m_name.c_str(), finished) != 0)
	{
		// Nothing is overwritten when the rename fails: the log ends here, as it did at its size cap before 0.34.
		std::snprintf(line, sizeof(line), "%s reached %llu MiB and could not be renamed to %s (errno %d); the log is closed for the rest of this session",
			m_name.c_str(), static_cast<unsigned long long>(m_max_size >> 20), finished, errno);
		ps4_fatal_note("log: ", line);
		return;
	}

	m_parts = part;

	if (part > 2)
	{
		char old[512];
		std::snprintf(old, sizeof(old), "%s%u.log", m_part_prefix.c_str(), part - 2);
		::unlink(old);
	}

	m_file_start = m_out;

	if (!m_fout.open(m_name, fs::rewrite))
	{
		std::snprintf(line, sizeof(line), "%s reached %llu MiB and was saved as %s, but a new file could not be opened (errno %d); the log is closed for the rest of this session",
			m_name.c_str(), static_cast<unsigned long long>(m_max_size >> 20), finished, errno);
		ps4_fatal_note("log: ", line);
		return;
	}

	std::snprintf(line, sizeof(line), "\xEF\xBB\xBF(continued: the previous %llu MiB of this session are in %s)\n", static_cast<unsigned long long>(m_max_size >> 20), finished);
	m_fout.write(line, std::strlen(line));
	std::snprintf(line, sizeof(line), "%s reached %llu MiB: saved as %s, a new file continues (the newest 2 parts are kept)",
		m_name.c_str(), static_cast<unsigned long long>(m_max_size >> 20), finished);
	ps4_fatal_note("log: ", line);
}
#endif

void logs::file_writer::log(const char* text, usz size)
{
	if (!m_fptr)
	{
		return;
	}

#ifdef __ORBIS__
	utils::progress_backoff backoff;
#endif

	// TODO: write bigger fragment directly in blocking manner
	while (size && size < s_log_size)
	{
		const auto [bufv, pos] = m_buf.fetch_op([&](u64& v) -> uchar*
		{
			const u64 out = m_out % s_log_size;
			const u64 v1 = (v / s_log_size) % s_log_size;
			const u64 v2 = v % s_log_size;

			if (v1 + v2 + size >= (out <= v1 ? out + s_log_size : out)) [[unlikely]]
			{
				return nullptr;
			}

			v += size;
			return m_fptr.get() + (v1 + v2) % s_log_size;
		});

		if (!pos) [[unlikely]]
		{
#ifdef __ORBIS__
			// 0.34: m_max_size is a per-file size (rotate()); only a closed log or the destructor (m_out = umax) stop it.
			if (m_out == umax || (!m_fout && !m_fout2))
#else
			if (m_out >= m_max_size || (!m_fout && !m_fout2))
#endif
			{
				// Logging is inactive
				return;
			}

			if ((bufv % s_log_size) + size >= s_log_size || bufv % s_log_size)
			{
				// Concurrency limit reached
#ifdef __ORBIS__
				backoff.yield();
#else
				std::this_thread::yield();
#endif
			}
			else if (!m_m.is_free())
			{
				// Wait for another flush call to complete
				m_m.lock_unlock();
			}
			else
			{
				// Queue is full, need to write out
				flush(bufv);
			}

			continue;
		}

		if (pos - m_fptr.get() + size > s_log_size)
		{
			const auto frag = s_log_size - (pos - m_fptr.get());
			std::memcpy(pos, text, frag);
			std::memcpy(m_fptr.get(), text + frag, size - frag);
		}
		else
		{
			std::memcpy(pos, text, size);
		}

		m_buf += (size * s_log_size) - size;
		break;
	}
}

void logs::file_writer::sync()
{
	if (!m_fptr)
	{
		return;
	}

#ifdef __ORBIS__
	// Never wait unboundedly for the writer on PS4: a PPU thread spinning here holds its passive-lock
	// slot without cpu_flag::wait and cannot acknowledge an LV2 suspension, which stops the whole guest
	// scheduler (0.20 autosave freeze). The exit/main path gets a longer bound.
	const auto deadline = std::chrono::steady_clock::now() +
		(thread_ctrl::get_current() ? std::chrono::milliseconds(200) : std::chrono::milliseconds(2000));
#endif

	// Wait for the writer thread
	while ((m_out % s_log_size) * s_log_size != m_buf % (s_log_size * s_log_size))
	{
#ifdef __ORBIS__
		if (m_out == umax)
#else
		if (m_out >= m_max_size)
#endif
		{
			break;
		}

#ifdef __ORBIS__
		if (std::chrono::steady_clock::now() >= deadline)
		{
			// Give up: losing the tail of the log is better than freezing the guest or the exit path
			return;
		}

		// yield() does not reliably let the writer run under fixed priorities: really sleep
		std::this_thread::sleep_for(200us);
#else
		std::this_thread::yield();
#endif
	}

	if (thread_ctrl::get_current())
	{
		return;
	}

#ifdef __ORBIS__
	// 0.33: flush() closes m_fout under m_m after a failed write, so test and sync it under m_m too; otherwise the
	// file could be closed between the test and the call (fs::file::sync on an empty handle ends the thread). Bounded
	// like the wait above: a thread that died inside flush() keeps m_m forever. There is no compressed stream here.
	while (!m_m.try_lock())
	{
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return;
		}

		std::this_thread::sleep_for(200us);
	}

	if (m_fout)
	{
		m_fout.sync();
	}

	m_m.unlock();
#else
	// Ensure written to disk
	if (m_fout)
	{
		m_fout.sync();
	}

	if (m_fout2)
	{
		m_fout2.sync();
	}
#endif
}

void logs::file_writer::close_prematurely()
{
	if (!m_fptr)
	{
		return;
	}

	// Ensure written to disk
	sync();

	std::lock_guard lock(m_m);

	if (m_fout2)
	{
		m_zs.avail_in = 0;
		m_zs.next_in  = nullptr;

		do
		{
			m_zs.avail_out = sizeof(m_zout);
			m_zs.next_out  = m_zout;

			if (deflate(&m_zs, Z_FINISH) == Z_STREAM_ERROR || m_fout2.write(m_zout, sizeof(m_zout) - m_zs.avail_out) != sizeof(m_zout) - m_zs.avail_out)
			{
				break;
			}
		}
		while (m_zs.avail_out == 0);

		deflateEnd(&m_zs);

#ifdef _WIN32
		// Cancel compressed log file auto-deletion
		FILE_DISPOSITION_INFO disp;
		disp.DeleteFileW = false;
		SetFileInformationByHandle(m_fout2.get_handle(), FileDispositionInfo, &disp, sizeof(disp));
#else
		// Restore compressed log file permissions
		::fchmod(m_fout2.get_handle(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
#endif

		m_fout2.close();
	}

	if (m_fout)
	{
		m_fout.close();
	}
}

logs::file_listener::file_listener(const std::string& path, u64 max_size)
	: file_writer(path, max_size)
	, listener()
{
	// Write UTF-8 BOM
	file_writer::log("\xEF\xBB\xBF", 3);
}

void logs::file_listener::log(u64 stamp, const logs::message& msg, std::string_view prefix, std::string_view _text)
{
	/*constinit thread_local*/ std::string text;
	text.reserve(50000);

	// Used character: U+00B7 (Middle Dot)
	switch (msg)
	{
	case level::always:  text = reinterpret_cast<const char*>(u8"·A "); break;
	case level::fatal:   text = reinterpret_cast<const char*>(u8"·F "); break;
	case level::error:   text = reinterpret_cast<const char*>(u8"·E "); break;
	case level::todo:    text = reinterpret_cast<const char*>(u8"·U "); break;
	case level::success: text = reinterpret_cast<const char*>(u8"·S "); break;
	case level::warning: text = reinterpret_cast<const char*>(u8"·W "); break;
	case level::notice:  text = reinterpret_cast<const char*>(u8"·! "); break;
	case level::trace:   text = reinterpret_cast<const char*>(u8"·T "); break;
	}

	// Print microsecond timestamp
	const u64 hours = stamp / 3600'000'000;
	const u64 mins = (stamp % 3600'000'000) / 60'000'000;
	const u64 secs = (stamp % 60'000'000) / 1'000'000;
	const u64 frac = (stamp % 1'000'000);
	fmt::append(text, "%u:%02u:%02u.%06u ", hours, mins, secs, frac);

	if (stamp == 0)
	{
		// Workaround for first special messages to keep backward compatibility
		text.clear();
	}

	if (!prefix.empty())
	{
		text += "{";
		text += prefix;
		text += "} ";
	}

	if (stamp && msg->name && '\0' != *msg->name)
	{
		text += msg->name;
		text += msg == level::todo ? " TODO: " : ": ";
	}
	else if (msg == level::todo)
	{
		text += "TODO: ";
	}

	text += _text;
	text += '\n';

	file_writer::log(text.data(), text.size());
}

std::unique_ptr<logs::listener> logs::make_file_listener(const std::string& path, u64 max_size)
{
	std::unique_ptr<logs::listener> result = std::make_unique<logs::file_listener>(path, max_size);

	// Register file listener
	result->add(result.get());
	return result;
}
