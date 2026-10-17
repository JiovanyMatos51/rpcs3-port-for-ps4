#pragma once

#ifdef RPCS3_PS4

#include "Emu/Audio/AudioBackend.h"

#include <atomic>
#include <thread>
#include <pthread.h>

// Underrun attribution from the cellAudio thread (cellAudio.cpp), counted since the previous call.
struct ps4_cell_audio_window
{
	u64 periods = 0;           // audio periods advanced
	u64 game_late = 0;         // periods advanced while the game's port buffers were still untouched (game late)
	u64 host_late = 0;         // cellAudio wake-ups more than one period after the requested time (host late)
	u64 max_wake_late_us = 0;  // largest such wake-up delay
	u64 in_progress_waits = 0; // waits for a port buffer the game was still writing
	u64 silent = 0;            // silent blocks enqueued (no started port, or nothing written in the period)
	u64 untouched_waits = 0;   // waits for a port buffer the game had not touched yet
	u64 wait_us = 0;           // time from the first wait of a period to its advance (cellAudio waiting for the game)
	u64 drops = 0;             // mixed blocks dropped because the output ring was full
	u64 ring_min_us = umax;    // lowest output ring fill seen when a period was mixed; umax: nothing mixed
	u64 stretch_ratio_milli = umax; // lowest time stretching ratio x1000; umax: time stretching off in the window
	u64 poll_us = 0;           // time spent in the fine block poll (audio-fine-poll.flag)
	u8 fine_poll = 0;          // fine block poll: 0 flag absent, 1 armed but off, 2 on (current state)
	u64 reserve_waits = 0;     // periods that waited for the game past the upstream timeout, on the output reserve
	u64 late_max_us = 0;       // longest wait for an untouched block in one period (from the first wait to the advance)
	u64 late_hist[5] = {};     // those waits per period: < 5, 5-10, 10-20, 20-40, >= 40 ms
	u64 idle_ports = 0;        // most started ports idle (no write for 100 ms) at a period end
	u64 tempo_milli_sum = 0;   // resampler tempo x1000 summed over the periods (1000 = no stretch)
	u64 tempo_periods = 0;
	u64 rate_milli = umax;     // game audio production rate x1000 over the last ~2 s (current state); umax: not measured
};

// Reads and resets the counters. Any thread, lock-free.
ps4_cell_audio_window ps4_cell_audio_take_window() noexcept;

// The game's audio production rate x1000 (1000 = real time) over the last ~2 s; umax while not measured. Any thread,
// lock-free, resets nothing (for status lines other than the AudioOut one).
u64 ps4_cell_audio_rate_milli() noexcept;

class PS4AudioBackend final : public AudioBackend
{
public:
	PS4AudioBackend() = default;
	~PS4AudioBackend() override;

	std::string_view GetName() const override { return "PS4 AudioOut"; }
	bool Open(std::string_view dev_id, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout) override;
	void Close() override;
	f64 GetCallbackFrameLen() override { return 256.0 / 48000.0; }
	bool IsPlaying() override { return m_running.load(); }
	void Play() override;
	void Pause() override;
	bool Operational() override { return m_handle >= 0 && !m_failed.load(); }

private:
	void output_loop();

	std::atomic<bool> m_running{false};
	// Set when sceAudioOutOutput failed and the output thread ended (output device change, rest mode). cellAudio
	// ignores the state event, but it polls Operational(): false makes it close this backend and open a new port.
	// Cleared by Open().
	std::atomic<bool> m_failed{false};
	// Read in Open(), used by the output thread it starts. Short blocks are concealed unless
	// /data/rpcs3/audio-no-conceal.flag exists; /data/rpcs3/audio-high-priority.flag runs the output thread one
	// priority step above the RSX thread.
	bool m_conceal = true;
	bool m_high_priority = false;
	// 0.28: a native thread, not std::thread: libc++ throws when the console is out of thread resources and
	// this TU is -fno-exceptions (std::terminate). On failure the audio stays silent instead.
	pthread_t m_thread{};
	bool m_thread_valid = false;
	int m_handle = -1;
};

#endif
