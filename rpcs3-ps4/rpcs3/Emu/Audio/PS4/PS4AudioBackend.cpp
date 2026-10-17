#include "stdafx.h"
#include "PS4AudioBackend.h"
#include "Emu/Audio/audio_device_enumerator.h"
#include "util/ps4_audio_stream.hpp"
#include "Utilities/Thread.h"

#ifdef RPCS3_PS4

#include "util/ps4_audio_conceal.hpp"

#include <orbis/AudioOut.h>
#include <orbis/libkernel.h>
#include <orbis/UserService.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>

LOG_CHANNEL(ps4_audio_log, "PS4 AudioOut");

// Utilities/Thread.cpp: native priority of the main thread (process base, 700 on the console)
int ps4_thread_base_native_priority() noexcept;
// ps4_launcher.cpp, any thread: one startup.log line.
void ps4_startup_checkpoint(const char* stage);
// util/logs.cpp: the per-thread prefix of RPCS3.log lines (a bare "TID: n" for threads outside thread_base)
extern thread_local std::string(*g_tls_log_prefix)();

namespace
{
	// cellAudio retries a dead backend about twice a second. A port that keeps failing must not flood the logs:
	// the first failures are logged, then one in 100.
	std::atomic<u32> s_ps4_open_failures{0};
	std::atomic<u32> s_ps4_output_failures{0};

	// The backend is reopened on every recovery or setting change; its startup.log lines are written once.
	std::atomic<bool> s_ps4_conceal_reported{false};
	std::atomic<bool> s_ps4_priority_reported{false};

	bool ps4_log_failure(std::atomic<u32>& count, u32& number)
	{
		number = count.fetch_add(1, std::memory_order_relaxed) + 1;
		return number <= 8 || number % 100 == 0;
	}
}

PS4AudioBackend::~PS4AudioBackend()
{
	Close();
}

bool PS4AudioBackend::Open(std::string_view dev_id, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout)
{
	Close();
	m_failed = false;
	if (!dev_id.empty() && dev_id != audio_device_enumerator::DEFAULT_DEV_ID)
	{
		ps4_audio_log.error("Only the default PS4 audio device is supported");
		return false;
	}
	if (freq != AudioFreq::FREQ_48K)
	{
		ps4_audio_log.error("Only 48 kHz output is supported by this PS4 backend");
		return false;
	}
	if (sample_size != AudioSampleSize::FLOAT && sample_size != AudioSampleSize::S16)
	{
		return false;
	}

	// The main PS4 AudioOut port is stereo. RPCS3's mixer handles downmixing
	// when the emulated game requests more channels.
	m_sampling_rate = freq;
	m_sample_size = sample_size;
	setup_channel_layout(static_cast<u32>(ch_cnt), 2, layout, ps4_audio_log);
	if (m_channels != 2)
	{
		ps4_audio_log.error("The PS4 backend requires a stereo output layout");
		return false;
	}

	static std::once_flag audio_init_once;
	static bool audio_initialized = false;
	std::call_once(audio_init_once, []
	{
		sceUserServiceInitialize(nullptr);
		audio_initialized = sceAudioOutInit() >= 0;
	});
	if (!audio_initialized)
	{
		ps4_audio_log.error("sceAudioOutInit failed");
		return false;
	}
	const u32 format = sample_size == AudioSampleSize::S16
		? ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO
		: ORBIS_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO;
	m_handle = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM,
		ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, format);
	if (m_handle <= 0)
	{
		if (u32 number = 0; ps4_log_failure(s_ps4_open_failures, number))
		{
			ps4_audio_log.error("sceAudioOutOpen failed: 0x%x (failure %u)", m_handle, number);
		}
		m_handle = -1;
		return false;
	}

	m_conceal = !fs::is_file("/data/rpcs3/audio-no-conceal.flag");
	m_high_priority = fs::is_file("/data/rpcs3/audio-high-priority.flag");

	if (!s_ps4_conceal_reported.exchange(true))
	{
		ps4_startup_checkpoint(m_conceal
			? "AudioOut: underrun concealment on (short fade instead of silence; audio-no-conceal.flag turns it off)"
			: "AudioOut: underrun concealment off (audio-no-conceal.flag)");
	}
	return true;
}

void PS4AudioBackend::Close()
{
	Pause();
	if (m_handle >= 0)
	{
		sceAudioOutClose(m_handle);
		m_handle = -1;
	}
}

void PS4AudioBackend::Play()
{
	if (m_handle < 0 || m_running)
	{
		return;
	}
	if (m_thread_valid)
	{
		pthread_join(m_thread, nullptr);
		m_thread_valid = false;
	}
	if (m_running.exchange(true))
	{
		return;
	}
	m_playing = true;
	const int error = pthread_create(&m_thread, nullptr, [](void* self) -> void*
	{
		static_cast<PS4AudioBackend*>(self)->output_loop();
		return nullptr;
	}, this);
	m_thread_valid = error == 0;
	if (!m_thread_valid)
	{
		ps4_audio_log.error("PS4: audio output thread could not be created (error 0x%x); audio stays silent", static_cast<u32>(error));
		m_running = false;
		m_playing = false;
	}
}

void PS4AudioBackend::Pause()
{
	m_running = false;
	if (m_thread_valid)
	{
		pthread_join(m_thread, nullptr);
		m_thread_valid = false;
	}
	m_playing = false;
}

void PS4AudioBackend::output_loop()
{
	// 0.36: a name for the kernel thread list and for the 10 s 'AudioOut:' lines in RPCS3.log
	pthread_set_name_np(pthread_self(), "PS4 AudioOut");
	g_tls_log_prefix = []() -> std::string { return "PS4 AudioOut Thread"; };

	// This std::thread is created by the (raised) cellAudio thread and inherits its priority: in the
	// 0.24 log its +1 then gave 684 instead of 692. Start from the process base so the boost stays one
	// step above the guest threads. The boost itself is kept: this thread blocks in sceAudioOutOutput.
	// audio-high-priority.flag: two steps above the base instead (one above the raised RSX thread, which shares the
	// one-step priority with round-robin among equals).
	bool high_priority = false;
	if (const int base = ps4_thread_base_native_priority(); base >= 256)
	{
		high_priority = m_high_priority && base - 16 >= 256 && scePthreadSetprio(scePthreadSelf(), base - 16) == 0;

		if (!high_priority)
		{
			scePthreadSetprio(scePthreadSelf(), base);
		}
		else if (!s_ps4_priority_reported.exchange(true))
		{
			ps4_audio_log.notice("AudioOut: output thread at priority %d (audio-high-priority.flag)", base - 16);
		}
	}

	// A scoped +1 on top of the flag's priority would add a third step.
	std::optional<thread_ctrl::scoped_priority> priority;
	if (!high_priority)
	{
		priority.emplace(+1);
	}

	const u32 bytes = 256 * 2 * get_sample_size();
	const bool conceal = m_conceal;
	const bool s16 = m_sample_size == AudioSampleSize::S16;
	utils::ps4::audio_conceal::conceal_state conceal_state;
	u64 blocks = 0, underruns = 0, concealed = 0, lock_misses = 0, total_blocks = 0;
	auto report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	// Start a fresh cellAudio attribution window with this port.
	static_cast<void>(ps4_cell_audio_take_window());
	const int result = utils::ps4::audio_stream(bytes, [&] { return m_running.load(); }, [&](u8* buffer, u32 length)
	{
		std::memset(buffer, 0, length);
		u32 complete = 0;
		{
			std::unique_lock lock(m_cb_mutex, std::defer_lock);
			// Match the upstream desktop backends' bounded callback lock.
			if (lock.try_lock_for(std::chrono::microseconds(50)))
			{
				const u32 written = m_write_callback ? std::min(m_write_callback(length, buffer), length) : 0;
				complete = written - written % (2 * get_sample_size());
				std::memset(buffer + complete, 0, length - complete);
				if (complete < length) ++underruns;
			}
			else ++lock_misses;
		}
		// Short or missed blocks: fade out a mirrored continuation of the audio instead of a hard 5.33 ms silence.
		// Complete blocks only update the history (and fade in after a gap).
		if (conceal && utils::ps4::audio_conceal::fill(conceal_state, buffer, complete, length, s16) != 0)
		{
			++concealed;
		}
		++blocks;
		++total_blocks;
		if (const auto now = std::chrono::steady_clock::now(); now >= report)
		{
			// The cellAudio part tells why blocks ran short: game_late = periods the game left its port buffers
			// untouched past the timeout (time advanced without its audio); host_late = cellAudio thread wake-ups
			// more than one period after the time it asked for (host scheduling or oversleep). untouched_waits and
			// wait_ms = cellAudio polling for the game's next block (a game that paces the audio); drops = mixed
			// blocks lost to a full output ring; ring_min_ms = lowest output ring fill when a period was mixed.
			const ps4_cell_audio_window cell = ps4_cell_audio_take_window();
			// This TU is -fno-exceptions: fixed buffers for the optional parts.
			char ring_min[24] = "n/a";
			if (cell.ring_min_us != umax)
			{
				std::snprintf(ring_min, sizeof(ring_min), "%.1f", static_cast<double>(cell.ring_min_us) / 1000.0);
			}
			char extra[96] = "";
			int used = 0;
			if (cell.stretch_ratio_milli != umax)
			{
				used = std::snprintf(extra, sizeof(extra), " stretch=on ratio=%.2f", static_cast<double>(cell.stretch_ratio_milli) / 1000.0);
			}
			if (cell.fine_poll && used >= 0 && static_cast<usz>(used) < sizeof(extra))
			{
				std::snprintf(extra + used, sizeof(extra) - used, " assist=%s poll_ms=%llu", cell.fine_poll == 2 ? "on" : "off",
					static_cast<unsigned long long>(cell.poll_us / 1000));
			}
			// Lateness policy and continuity: periods held on the output reserve, how long cellAudio waited for the
			// game's block (histogram in ms), idle ports, the game's production rate (1.00 = real time) and the mean
			// tempo of the window.
			char rate[16] = "n/a";
			if (cell.rate_milli != umax)
			{
				std::snprintf(rate, sizeof(rate), "%.2f", static_cast<double>(cell.rate_milli) / 1000.0);
			}
			char policy[224] = "";
			std::snprintf(policy, sizeof(policy), " reserve_waits=%llu late_max_ms=%.1f late<5/5-10/10-20/20-40/>40=%llu/%llu/%llu/%llu/%llu idle_ports=%llu rate=%s tempo_avg=%.2f",
				static_cast<unsigned long long>(cell.reserve_waits), static_cast<double>(cell.late_max_us) / 1000.0,
				static_cast<unsigned long long>(cell.late_hist[0]), static_cast<unsigned long long>(cell.late_hist[1]),
				static_cast<unsigned long long>(cell.late_hist[2]), static_cast<unsigned long long>(cell.late_hist[3]),
				static_cast<unsigned long long>(cell.late_hist[4]), static_cast<unsigned long long>(cell.idle_ports), static_cast<const char*>(rate),
				cell.tempo_periods ? static_cast<double>(cell.tempo_milli_sum) / static_cast<double>(cell.tempo_periods) / 1000.0 : 1.0);
			ps4_audio_log.notice("AudioOut: blocks=%llu underruns=%llu concealed=%llu callback_lock_misses=%llu; cellAudio: periods=%llu game_late=%llu host_late=%llu (max %llu us) in_progress_waits=%llu silent=%llu untouched_waits=%llu wait_ms=%llu drops=%llu ring_min_ms=%s%s%s",
				blocks, underruns, concealed, lock_misses, cell.periods, cell.game_late, cell.host_late, cell.max_wake_late_us, cell.in_progress_waits, cell.silent,
				cell.untouched_waits, cell.wait_us / 1000, cell.drops, static_cast<const char*>(ring_min), static_cast<const char*>(extra), static_cast<const char*>(policy));
			blocks = underruns = concealed = lock_misses = 0;
			report = now + std::chrono::seconds(10);
		}
	}, [&](const void* buffer) { return sceAudioOutOutput(m_handle, buffer); });
	if (result < 0)
	{
		// Mark the port dead before the state event: Operational() turns false, cellAudio logs 'Backend stopped
		// unexpectedly', closes this backend and opens a new port on its next recovery pass.
		m_failed = true;
		m_running = false;
		if (u32 number = 0; ps4_log_failure(s_ps4_output_failures, number))
		{
			ps4_audio_log.error("sceAudioOutOutput failed: 0x%x (failure %u)", result, number);
			// This TU is -fno-exceptions: a fixed buffer instead of a heap string.
			char line[160];
			std::snprintf(line, sizeof(line), "AudioOut: sceAudioOutOutput failed 0x%x after %llu blocks; cellAudio reopens the port (failure %u)",
				static_cast<u32>(result), static_cast<unsigned long long>(total_blocks), number);
			ps4_startup_checkpoint(line);
		}
		std::lock_guard lock(m_state_cb_mutex);
		if (m_state_callback) m_state_callback(AudioStateEvent::UNSPECIFIED_ERROR);
	}
}

#endif
