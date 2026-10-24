#include "stdafx.h"
#include "Emu/emu_callbacks.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/Audio/audio_utils.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/Cell/lv2/sys_process.h"
#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/lv2/sys_mmapper.h"
#include "Emu/Cell/lv2/sys_memory.h"
#include "Emu/Memory/vm_var.h"
#include "Emu/savestate_utils.hpp"
#include "util/asm.hpp"
#include "util/vm.hpp"
#include "sysPrxForUser.h"
#include "cellAudio.h"
#include "util/video_provider.h"

#include <cmath>

#ifdef RPCS3_PS4
#include "Emu/Audio/PS4/PS4AudioBackend.h"
#include "util/ps4_audio_policy.hpp"
#include "util/ps4_hr_wait.hpp"
#include "util/ps4_safe_mode.hpp"

#include <orbis/libkernel.h>

#include <atomic>
#include <thread>
#endif

LOG_CHANNEL(cellAudio);

extern atomic_t<recording_mode> g_recording_mode;

extern void lv2_sleep(u64 timeout, ppu_thread* ppu = nullptr);

#ifdef RPCS3_PS4
// Utilities/Thread.cpp (__ORBIS__): native priority of the guest/emulator threads (700 on the console).
int ps4_thread_base_native_priority() noexcept;
// ps4_launcher.cpp, any thread: one startup.log line.
void ps4_startup_checkpoint(const char* stage);

namespace
{
	// Underrun attribution, printed by the PS4 AudioOut thread every 10 s next to its underrun count. Written by the
	// cellAudio thread only; relaxed atomics, read and reset by ps4_cell_audio_take_window().
	struct ps4_audio_counters
	{
		std::atomic<u64> periods{0};
		std::atomic<u64> game_late{0};
		std::atomic<u64> host_late{0};
		std::atomic<u64> max_wake_late_us{0};
		std::atomic<u64> in_progress_waits{0};
		std::atomic<u64> silent{0};
		std::atomic<u64> fill_silent{0}; // of 'silent': for ports the game wrote within the last 100 ms
		std::atomic<u64> untouched_waits{0};
		std::atomic<u64> wait_us{0};
		std::atomic<u64> drops{0};
		std::atomic<u64> ring_min_us{umax};
		std::atomic<u64> stretch_ratio_milli{umax};
		std::atomic<u64> poll_us{0};
		std::atomic<u8> fine_poll{0}; // state, not reset by the reader
		std::atomic<u64> reserve_waits{0};
		std::atomic<u64> late_max_us{0};
		std::atomic<u64> late_hist[utils::ps4::audio_policy::late_buckets]{};
		std::atomic<u64> idle_ports_max{0};
		std::atomic<u64> tempo_milli_sum{0};
		std::atomic<u64> tempo_periods{0};
		std::atomic<u64> rate_milli{umax}; // state, not reset by the reader: production rate x1000 (umax: not measured)
	};

	ps4_audio_counters s_ps4_audio_counters;

	static_assert(utils::ps4::audio_policy::tempo_min == RESAMPLER_MIN_FREQ_VAL);
	static_assert(std::size(ps4_cell_audio_window{}.late_hist) == utils::ps4::audio_policy::late_buckets);

	// Automatic continuity (rate-matched time stretching) engaged for this boot. Written and read by the cellAudio
	// thread (cell_audio_config::reset runs there too); cleared when that thread starts.
	std::atomic<bool> s_ps4_auto_stretch{false};

	// Port start lines copied to startup.log in this boot (the first few are enough to classify the game).
	std::atomic<u32> s_ps4_port_lines{0};

	// Set by the cellAudio thread around a rebuild of a backend that keeps failing whose lines are not due (see the
	// main loop): cell_audio_config::reset() then skips its open lines.
	std::atomic<bool> s_ps4_quiet_rebuild{false};

	void ps4_audio_count(std::atomic<u64>& counter)
	{
		counter.fetch_add(1, std::memory_order_relaxed);
	}

	// Lowest value of the window. The reader resets the counter concurrently, so a plain load/store could drop a
	// sample into the wrong window; a compare-exchange keeps the minimum exact.
	void ps4_audio_min(std::atomic<u64>& counter, u64 value)
	{
		u64 current = counter.load(std::memory_order_relaxed);
		while (value < current && !counter.compare_exchange_weak(current, value, std::memory_order_relaxed))
		{
		}
	}

	// Highest value of the window (same reasoning).
	void ps4_audio_max(std::atomic<u64>& counter, u64 value)
	{
		u64 current = counter.load(std::memory_order_relaxed);
		while (value > current && !counter.compare_exchange_weak(current, value, std::memory_order_relaxed))
		{
		}
	}

	// Writes its line to startup.log on destruction: declared before a lock_guard, the file write happens after the
	// audio mutex is released.
	struct ps4_deferred_checkpoint
	{
		std::string line;

		ps4_deferred_checkpoint() = default;
		ps4_deferred_checkpoint(const ps4_deferred_checkpoint&) = delete;
		ps4_deferred_checkpoint& operator=(const ps4_deferred_checkpoint&) = delete;

		~ps4_deferred_checkpoint()
		{
			if (!line.empty())
			{
				ps4_startup_checkpoint(line.c_str());
			}
		}
	};

	// One RPCS3.log line and startup.log checkpoint (logged to both).
	void ps4_audio_checkpoint(const std::string& line)
	{
		cellAudio.notice("%s", line);
		ps4_startup_checkpoint(line.c_str());
	}
}

ps4_cell_audio_window ps4_cell_audio_take_window() noexcept
{
	auto& c = s_ps4_audio_counters;
	ps4_cell_audio_window w;
	w.periods = c.periods.exchange(0, std::memory_order_relaxed);
	w.game_late = c.game_late.exchange(0, std::memory_order_relaxed);
	w.host_late = c.host_late.exchange(0, std::memory_order_relaxed);
	w.max_wake_late_us = c.max_wake_late_us.exchange(0, std::memory_order_relaxed);
	w.in_progress_waits = c.in_progress_waits.exchange(0, std::memory_order_relaxed);
	w.silent = c.silent.exchange(0, std::memory_order_relaxed);
	w.fill_silent = c.fill_silent.exchange(0, std::memory_order_relaxed);
	w.untouched_waits = c.untouched_waits.exchange(0, std::memory_order_relaxed);
	w.wait_us = c.wait_us.exchange(0, std::memory_order_relaxed);
	w.drops = c.drops.exchange(0, std::memory_order_relaxed);
	w.ring_min_us = c.ring_min_us.exchange(umax, std::memory_order_relaxed);
	w.stretch_ratio_milli = c.stretch_ratio_milli.exchange(umax, std::memory_order_relaxed);
	w.poll_us = c.poll_us.exchange(0, std::memory_order_relaxed);
	w.fine_poll = c.fine_poll.load(std::memory_order_relaxed);
	w.reserve_waits = c.reserve_waits.exchange(0, std::memory_order_relaxed);
	w.late_max_us = c.late_max_us.exchange(0, std::memory_order_relaxed);
	for (u32 i = 0; i < utils::ps4::audio_policy::late_buckets; i++)
	{
		w.late_hist[i] = c.late_hist[i].exchange(0, std::memory_order_relaxed);
	}
	w.idle_ports = c.idle_ports_max.exchange(0, std::memory_order_relaxed);
	w.tempo_milli_sum = c.tempo_milli_sum.exchange(0, std::memory_order_relaxed);
	w.tempo_periods = c.tempo_periods.exchange(0, std::memory_order_relaxed);
	w.rate_milli = c.rate_milli.load(std::memory_order_relaxed);
	return w;
}

u64 ps4_cell_audio_rate_milli() noexcept
{
	return s_ps4_audio_counters.rate_milli.load(std::memory_order_relaxed);
}
#endif

template <>
void fmt_class_string<CellAudioError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](CellAudioError value)
	{
		switch (value)
		{
		STR_CASE(CELL_AUDIO_ERROR_ALREADY_INIT);
		STR_CASE(CELL_AUDIO_ERROR_AUDIOSYSTEM);
		STR_CASE(CELL_AUDIO_ERROR_NOT_INIT);
		STR_CASE(CELL_AUDIO_ERROR_PARAM);
		STR_CASE(CELL_AUDIO_ERROR_PORT_FULL);
		STR_CASE(CELL_AUDIO_ERROR_PORT_ALREADY_RUN);
		STR_CASE(CELL_AUDIO_ERROR_PORT_NOT_OPEN);
		STR_CASE(CELL_AUDIO_ERROR_PORT_NOT_RUN);
		STR_CASE(CELL_AUDIO_ERROR_TRANS_EVENT);
		STR_CASE(CELL_AUDIO_ERROR_PORT_OPEN);
		STR_CASE(CELL_AUDIO_ERROR_SHAREDMEMORY);
		STR_CASE(CELL_AUDIO_ERROR_MUTEX);
		STR_CASE(CELL_AUDIO_ERROR_EVENT_QUEUE);
		STR_CASE(CELL_AUDIO_ERROR_AUDIOSYSTEM_NOT_FOUND);
		STR_CASE(CELL_AUDIO_ERROR_TAG_NOT_FOUND);
		}

		return unknown;
	});
}


cell_audio_config::cell_audio_config()
{
	raw = audio::get_raw_config();
}

void cell_audio_config::reset(bool backend_changed)
{
	if (!backend || backend_changed)
	{
		backend.reset();
		backend = g_emu_callbacks.get_audio();
	}

#ifdef RPCS3_PS4
	const bool ps4_quiet = s_ps4_quiet_rebuild.load(std::memory_order_relaxed);

	if (!ps4_quiet)
	{
		cellAudio.notice("cellAudio initializing. Backend: %s", backend->GetName());
	}
#else
	cellAudio.notice("cellAudio initializing. Backend: %s", backend->GetName());
#endif

	const AudioFreq freq = AudioFreq::FREQ_48K;
	const AudioSampleSize sample_size = raw.convert_to_s16 ? AudioSampleSize::S16 : AudioSampleSize::FLOAT;

	const auto& [req_ch_cnt, downmix] = AudioBackend::get_channel_count_and_downmixer(0); // CELL_AUDIO_OUT_PRIMARY
	f64 cb_frame_len = 0.0;
	u32 ch_cnt = 2;
	audio_channel_layout ch_layout = audio_channel_layout::stereo;

	if (backend->Open(raw.audio_device, freq, sample_size, req_ch_cnt, raw.channel_layout))
	{
		cb_frame_len = backend->GetCallbackFrameLen();
		ch_cnt = backend->get_channels();
		ch_layout = backend->get_channel_layout();
#ifdef RPCS3_PS4
		if (!ps4_quiet)
		{
			cellAudio.notice("Opened audio backend (sampling_rate=%d, sample_size=%d, channels=%d, layout=%s)", backend->get_sampling_rate(), backend->get_sample_size(), backend->get_channels(), backend->get_channel_layout());
		}
#else
		cellAudio.notice("Opened audio backend (sampling_rate=%d, sample_size=%d, channels=%d, layout=%s)", backend->get_sampling_rate(), backend->get_sample_size(), backend->get_channels(), backend->get_channel_layout());
#endif
	}
	else
	{
#ifdef RPCS3_PS4
		if (!ps4_quiet)
		{
			cellAudio.error("Failed to open audio backend. Make sure that no other application is running that might block audio access (e.g. Netflix).");
		}
#else
		cellAudio.error("Failed to open audio backend. Make sure that no other application is running that might block audio access (e.g. Netflix).");
#endif
	}

	audio_downmix = downmix;
	backend_ch_cnt = ch_cnt;
	backend_channel_layout = ch_layout;
	audio_channels = static_cast<u32>(req_ch_cnt);
	audio_sampling_rate = static_cast<u32>(freq);
	audio_block_period = AUDIO_BUFFER_SAMPLES * 1'000'000 / audio_sampling_rate;
	audio_sample_size = static_cast<u32>(sample_size);
	audio_min_buffer_duration = cb_frame_len + u32{AUDIO_BUFFER_SAMPLES} * 2.0 / audio_sampling_rate; // Add 2 blocks to allow jitter compensation

	audio_buffer_length = AUDIO_BUFFER_SAMPLES * audio_channels;

	desired_buffer_duration = std::max(static_cast<s64>(audio_min_buffer_duration * 1000), raw.desired_buffer_duration) * 1000llu;
	buffering_enabled = raw.buffering_enabled && raw.renderer != audio_renderer::null;

	minimum_block_period = audio_block_period / 2;
	maximum_block_period = (6 * audio_block_period) / 5;

	desired_full_buffers = buffering_enabled ? static_cast<u32>(desired_buffer_duration / audio_block_period) + 3 : 2;
	num_allocated_buffers = desired_full_buffers + EXTRA_AUDIO_BUFFERS;

	fully_untouched_timeout = static_cast<u64>(audio_block_period) * 2;
	partially_untouched_timeout = static_cast<u64>(audio_block_period) * 4;

	const bool raw_time_stretching_enabled = buffering_enabled && raw.enable_time_stretching && (raw.time_stretching_threshold > 0);

	time_stretching_enabled = raw_time_stretching_enabled;
	time_stretching_threshold = raw.time_stretching_threshold / 100.0f;

#ifdef RPCS3_PS4
	// Automatic continuity engaged for this boot outlives backend reopens and setting changes, which all recompute
	// the configuration here. The player's own setting wins when present (the threshold only serves the upstream
	// controller, which the PS4 one replaces).
	if (s_ps4_auto_stretch.load(std::memory_order_relaxed) && buffering_enabled && !raw_time_stretching_enabled)
	{
		time_stretching_enabled = true;
		time_stretching_threshold = 0.75f;
	}
#endif

	// Warn if audio backend does not support all requested features
	if (raw.buffering_enabled && !buffering_enabled)
	{
		cellAudio.error("Audio backend %s does not support buffering, this option will be ignored.", backend->GetName());

		if (raw.enable_time_stretching)
		{
			cellAudio.error("Audio backend %s does not support time stretching, this option will be ignored.", backend->GetName());
		}
	}
}

audio_ringbuffer::audio_ringbuffer(cell_audio_config& _cfg)
	: backend(_cfg.backend)
	, cfg(_cfg)
	, buf_sz(AUDIO_BUFFER_SAMPLES * _cfg.audio_channels)
{
	// Initialize buffers
	if (cfg.num_allocated_buffers > MAX_AUDIO_BUFFERS)
	{
		fmt::throw_exception("MAX_AUDIO_BUFFERS is too small");
	}

	for (u32 i = 0; i < cfg.num_allocated_buffers; i++)
	{
		buffer[i].reset(new float[buf_sz]{});
	}

	// Init audio dumper if enabled
	if (cfg.raw.dump_to_file)
	{
		m_dump.Open(static_cast<AudioChannelCnt>(cfg.audio_channels), static_cast<AudioFreq>(cfg.audio_sampling_rate), AudioSampleSize::FLOAT);
	}

	// Configure resampler
	resampler.set_params(static_cast<AudioChannelCnt>(cfg.audio_channels), static_cast<AudioFreq>(cfg.audio_sampling_rate));
	resampler.set_tempo(RESAMPLER_MAX_FREQ_VAL);

	const f64 buffer_dur_mult = [&]()
	{
		if (cfg.buffering_enabled)
		{
			return cfg.desired_buffer_duration / 1'000'000.0 + 0.02; // Add 20ms to buffer to keep buffering algorithm happy
		}

		return cfg.audio_min_buffer_duration;
	}();

	cb_ringbuf.set_buf_size(static_cast<u32>(cfg.backend_ch_cnt * cfg.audio_sampling_rate * cfg.audio_sample_size * buffer_dur_mult));
	backend->SetWriteCallback(std::bind(&audio_ringbuffer::backend_write_callback, this, std::placeholders::_1, std::placeholders::_2));
	backend->SetStateCallback(std::bind(&audio_ringbuffer::backend_state_callback, this, std::placeholders::_1));
}

audio_ringbuffer::~audio_ringbuffer()
{
	if (get_backend_playing())
	{
		flush();
	}

	backend->Close();
}

f32 audio_ringbuffer::set_frequency_ratio(f32 new_ratio)
{
	frequency_ratio = static_cast<f32>(resampler.set_tempo(new_ratio));

	return frequency_ratio;
}

float* audio_ringbuffer::get_buffer(u32 num) const
{
	AUDIT(num < cfg.num_allocated_buffers);
	AUDIT(buffer[num]);
	return buffer[num].get();
}

u32 audio_ringbuffer::backend_write_callback(u32 size, void *buf)
{
	if (!backend_active.observe()) backend_active = true;

	return static_cast<u32>(cb_ringbuf.pop(buf, size, true));
}

void audio_ringbuffer::backend_state_callback(AudioStateEvent event)
{
	if (event == AudioStateEvent::DEFAULT_DEVICE_MAYBE_CHANGED)
	{
		backend_device_changed = true;
	}
}

u64 audio_ringbuffer::get_timestamp()
{
	return get_system_time();
}

float* audio_ringbuffer::get_current_buffer() const
{
	return get_buffer(cur_pos);
}

u64 audio_ringbuffer::get_enqueued_samples() const
{
	AUDIT(cfg.buffering_enabled);
	const u64 ringbuf_samples = cb_ringbuf.get_used_size() / (cfg.audio_sample_size * cfg.backend_ch_cnt);

	if (cfg.time_stretching_enabled)
	{
		return ringbuf_samples + resampler.samples_available();
	}

	return ringbuf_samples;
}

u64 audio_ringbuffer::get_enqueued_playtime() const
{
	AUDIT(cfg.buffering_enabled);

	return get_enqueued_samples() * 1'000'000 / cfg.audio_sampling_rate;
}

void audio_ringbuffer::enqueue(bool enqueue_silence, bool force)
{
	AUDIT(cur_pos < cfg.num_allocated_buffers);

	// Prepare buffer
	static float silence_buffer[u32{AUDIO_MAX_CHANNELS_COUNT} * u32{AUDIO_BUFFER_SAMPLES}]{};
	float* buf = silence_buffer;

	if (!enqueue_silence)
	{
		buf = buffer[cur_pos].get();
		cur_pos = (cur_pos + 1) % cfg.num_allocated_buffers;
	}

	if (!backend_active.observe() && !force)
	{
		// backend is not ready yet
		return;
	}

	// Enqueue audio
	if (cfg.time_stretching_enabled)
	{
		resampler.put_samples(buf, AUDIO_BUFFER_SAMPLES);
	}
	else
	{
		// Since time stretching step is skipped, we can commit to buffer directly
		commit_data(buf, AUDIO_BUFFER_SAMPLES);
	}
}

void audio_ringbuffer::enqueue_silence(u32 buf_count, bool force)
{
	for (u32 i = 0; i < buf_count; i++)
	{
		enqueue(true, force);
	}
}

void audio_ringbuffer::process_resampled_data()
{
	if (!cfg.time_stretching_enabled) return;

	const auto& [buffer, samples] = resampler.get_samples(static_cast<u32>(cb_ringbuf.get_free_size() / (cfg.audio_sample_size * cfg.backend_ch_cnt)));
	commit_data(buffer, samples);
}

void audio_ringbuffer::commit_data(f32* buf, u32 sample_cnt)
{
	const u32 sample_cnt_in = sample_cnt * cfg.audio_channels;
	const u32 sample_cnt_out = sample_cnt * cfg.backend_ch_cnt;

	// Dump audio if enabled
	m_dump.WriteData(buf, sample_cnt_in * static_cast<u32>(AudioSampleSize::FLOAT));

	// Record audio if enabled
	if (g_recording_mode != recording_mode::stopped)
	{
		utils::video_provider& provider = g_fxo->get<utils::video_provider>();
		provider.present_samples(reinterpret_cast<const u8*>(buf), sample_cnt, cfg.audio_channels);
	}

	// Downmix if necessary
	AudioBackend::downmix(sample_cnt_in, cfg.audio_channels, cfg.backend_channel_layout, buf, buf);

	if (cfg.backend->get_convert_to_s16())
	{
		AudioBackend::convert_to_s16(sample_cnt_out, buf, buf);
	}

#ifdef RPCS3_PS4
	// push() without force writes all or nothing: a full ring drops the whole block.
	if (const u64 size = u64{sample_cnt_out} * cfg.audio_sample_size; size && !cb_ringbuf.push(buf, size))
	{
		ps4_audio_count(s_ps4_audio_counters.drops);
	}
#else
	cb_ringbuf.push(buf, sample_cnt_out * cfg.audio_sample_size);
#endif
}

void audio_ringbuffer::play()
{
	if (playing)
	{
		return;
	}

	playing = true;
	play_timestamp = get_timestamp();
	backend->Play();
}

void audio_ringbuffer::flush()
{
	backend->Pause();
	cb_ringbuf.writer_flush();
	resampler.flush();
	backend_active = false;
	playing = false;

	if (frequency_ratio != RESAMPLER_MAX_FREQ_VAL)
	{
		frequency_ratio = set_frequency_ratio(RESAMPLER_MAX_FREQ_VAL);
	}
}

u64 audio_ringbuffer::update(bool emu_is_paused)
{
	// Check emulator pause state
	if (emu_is_paused)
	{
		// Emulator paused
		if (playing)
		{
			flush();
		}
	}
	else
	{
		// Emulator unpaused
		play();
	}

	// Prepare timestamp
	const u64 timestamp = get_timestamp();

	// Store and return timestamp
	update_timestamp = timestamp;
	return timestamp;
}

void audio_port::tag(be_t<f32>* port_buf)
{
	// This tag will be used to make sure that the game has finished writing the audio for the next audio period
	// We use -0.0f in case games check if the buffer is empty. -0.0f == 0.0f evaluates to true, but std::signbit can be used to distinguish them
	const f32 tag = -0.0f;

	const u32 tag_first_pos = num_channels == 2 ? PORT_BUFFER_TAG_FIRST_2CH : PORT_BUFFER_TAG_FIRST_8CH;
	const u32 tag_delta = num_channels == 2 ? PORT_BUFFER_TAG_DELTA_2CH : PORT_BUFFER_TAG_DELTA_8CH;

	for (u32 tag_pos = tag_first_pos, tag_nr = 0; tag_nr < PORT_BUFFER_TAG_COUNT; tag_pos += tag_delta, tag_nr++)
	{
		port_buf[tag_pos] = tag;
		last_tag_value[tag_nr] = -0.0f;
	}

	// Mark the front right channel as well, see PORT_BUFFER_MARK_CHANNEL
	if (num_channels != 2)
	{
		for (u32 tag_nr = 0; tag_nr < PORT_BUFFER_TAG_COUNT; tag_nr++)
		{
			port_buf[mark_position(tag_nr)] = tag;
		}
	}

	prev_touched_tag_nr = -1;
}

namespace
{
	void save_audio_memory(utils::serial& ar, const shared_ptr<lv2_memory>& memory)
	{
		ar(static_cast<bool>(memory));

		if (!memory)
		{
			return;
		}

		const bool registered = memory->exists || memory->key == SYS_MMAPPER_MIO_SHM_KEY;
		ar(registered);

		if (registered)
		{
			ar(memory->key);
		}
		else
		{
			memory->save_data(ar);
		}
	}

	shared_ptr<lv2_memory> load_audio_memory(utils::serial& ar)
	{
		if (!ar.pop<bool>())
		{
			return {};
		}

		if (ar.pop<bool>())
		{
			return ensure(g_fxo->get<ipc_manager<lv2_memory, u64>>().get(ar.pop<u64>()));
		}

		auto memory = make_shared<lv2_memory>(stx::exact_t<utils::serial&>(ar));
		ensure(memory->external_refs);
		ensure((g_fxo->get<ipc_manager<lv2_memory, u64>>().add(memory->key, [&] { return memory; }).first));
		return memory;
	}

	shared_ptr<lv2_memory> acquire_audio_memory(u32 id)
	{
		return idm::get<lv2_obj, lv2_memory>(id, [](lv2_memory& memory)
		{
			memory.external_refs++;
		});
	}

	void release_audio_memory(shared_ptr<lv2_memory>& memory)
	{
		if (memory)
		{
			std::lock_guard lock(id_manager::g_mutex);
			ensure(memory->external_refs--);
			memory->release_memory();
			memory.reset();
		}
	}
}

cell_audio_thread::cell_audio_thread(utils::serial& ar)
	: cell_audio_thread()
{
	ar(init, shared_area, shared_address, shared_refs, ports);

	if (!init)
	{
		return;
	}

	ar(key_count, event_period);

	keys.resize(ar.pop<u64>());

	for (key_info& k : keys)
	{
		ar(k.start_period, k.flags, k.source);
		k.port = lv2_event_queue::load_ptr(ar, k.port, "audio");
	}

	ar(free_port_count, free_ports, free_indices);
	shared_memory = load_audio_memory(ar);
	for (auto& memory : port_memories)
	{
		memory = load_audio_memory(ar);
	}
}

void cell_audio_thread::save(utils::serial& ar)
{
	USING_SERIALIZATION_VERSION(cellAudio);

	// closed ports keep their descriptor after audio quits
	ar(init, shared_area, shared_address, shared_refs, ports);

	if (!init)
	{
		return;
	}

	ar(key_count, event_period);
	ar(static_cast<u64>(keys.size()));

	for (const key_info& k : keys)
	{
		ar(k.start_period, k.flags, k.source);
		lv2_event_queue::save_ptr(ar, k.port.get());
	}

	ar(free_port_count, free_ports, free_indices);
	save_audio_memory(ar, shared_memory);
	for (const auto& memory : port_memories)
	{
		save_audio_memory(ar, memory);
	}
}

be_t<f32>* cell_audio_thread::get_buffer(const audio_port& port, s32 offset) const
{
	// caller holds the audio mutex, port_memories keeps the buffer alive after the game unmaps it
	return reinterpret_cast<be_t<f32>*>((*port_memories[port.number]->shm.observe())->get()) + port.position(offset) * port.block_size();
}

std::tuple<u32, u32, u32, u32> cell_audio_thread::count_port_buffer_tags()
{
	std::lock_guard lock(mutex);

	AUDIT(cfg.buffering_enabled);

	u32 active = 0;
	u32 in_progress = 0;
	u32 untouched = 0;
	u32 incomplete = 0;

#ifdef RPCS3_PS4
	// Cadence of each port for the PS4 lateness policy (ps4_audio_policy.hpp): when it last wrote, and how many
	// periods in a row it delivered. Only this thread reads or writes it.
	const u64 ps4_now = get_system_time();
	m_ps4_untouched_idle = 0;
	m_ps4_untouched_regular = 0;
#endif

	for (u32 port_index = 0; port_index < ports.size(); port_index++)
	{
		audio_port& port = ports[port_index];

#ifdef RPCS3_PS4
		ps4_port_cadence& ps4_cadence = m_ps4_cadence[port_index];

		if (port.state != audio_port_state::started)
		{
			ps4_cadence.started = false;
			continue;
		}

		if (!ps4_cadence.started)
		{
			// Started since the last call: it gets idle_after_us to write its first block.
			ps4_cadence = {};
			ps4_cadence.started = true;
			ps4_cadence.touch_time = ps4_now;
		}

		ps4_cadence.untouched = false;
#else
		if (port.state != audio_port_state::started) continue;
#endif
		active++;

		if (!port.num_channels) continue;

		auto port_buf = get_buffer(port);

		// Find the last tag that has been touched
		const u32 tag_first_pos = port.num_channels == 2 ? PORT_BUFFER_TAG_FIRST_2CH : PORT_BUFFER_TAG_FIRST_8CH;
		const u32 tag_delta = port.num_channels == 2 ? PORT_BUFFER_TAG_DELTA_2CH : PORT_BUFFER_TAG_DELTA_8CH;

		u32 last_touched_tag_nr = port.prev_touched_tag_nr;
		bool retouched = false;
		bool tag_moved = false;
		for (u32 tag_pos = tag_first_pos, tag_nr = 0; tag_nr < PORT_BUFFER_TAG_COUNT; tag_pos += tag_delta, tag_nr++)
		{
			const f32 val = port_buf[tag_pos];
			f32& last_val = port.last_tag_value[tag_nr];

			if (val != last_val || (last_val == -0.0f && std::signbit(last_val) && !std::signbit(val)))
			{
				last_val = val;

				retouched |= (tag_nr <= port.prev_touched_tag_nr) && port.prev_touched_tag_nr != umax;
				last_touched_tag_nr = tag_nr;
				tag_moved = true;
			}
		}

		if (tag_moved)
		{
			// This port reaches its surround channels, so its tags track it on their own and the marks below
			// are never consulted for it.
			m_periods_without_tag[port_index] = 0;
			m_front_only_port[port_index] = false;
#ifdef RPCS3_PS4
			ps4_cadence.touch_time = ps4_now;
#endif
		}

		// Decide whether the buffer is untouched, in progress, incomplete, or complete
		if (last_touched_tag_nr == umax)
		{
			// No tag has been touched yet. That normally means the buffer is untouched, but it also happens
			// when the game filled the front channels only: no tag can ever move then, and the port would
			// stay silent forever. The marks tell the two apart. They are only consulted once this port has
			// gone long enough without moving a tag to rule out a game writing the buffer one channel at a
			// time, which looks identical in between its front right and its center pass.
			bool& front_only = m_front_only_port[port_index];

			if (!front_only && port.num_channels != 2 && m_periods_without_tag[port_index] > PORT_FRONT_ONLY_SETTLE_PERIODS)
			{
				for (u32 tag_nr = 0; tag_nr < PORT_BUFFER_TAG_COUNT; tag_nr++)
				{
					const f32 val = port_buf[port.mark_position(tag_nr)];

					if (val != -0.0f || !std::signbit(val))
					{
						front_only = true;
						break;
					}
				}

				if (front_only && !m_front_only_reported[port_index])
				{
					m_front_only_reported[port_index] = true;
					cellAudio.notice("Port %u carries front channel audio only (num_channels=%u). Its buffer tags all sit on surround channels and can never move, so the front channel marks decide whether it is silent.", port.number, port.num_channels);
				}
			}

			if (!front_only)
			{
				// nothing has been written at all, or this port has not settled yet
				untouched++;

#ifdef RPCS3_PS4
				ps4_cadence.untouched = true;

				if (ps4_now - ps4_cadence.touch_time >= utils::ps4::audio_policy::idle_after_us)
				{
					m_ps4_untouched_idle++;
				}
				else if (ps4_cadence.streak >= utils::ps4::audio_policy::regular_streak)
				{
					m_ps4_untouched_regular++;
				}
#endif
			}
			else
			{
				// The game only ever fills the front channels of this port, so the block is as complete as it
				// is ever going to get. Incomplete is the verdict that says so, and it still gets mixed.
				incomplete++;
			}
		}
		else if (last_touched_tag_nr == PORT_BUFFER_TAG_COUNT - 1)
		{
			if (retouched)
			{
				// we retouched, so wait at least once more to make sure no more tags get touched
				in_progress++;
			}

			// buffer has been completely filled
			port.prev_touched_tag_nr = last_touched_tag_nr;
		}
		else if (last_touched_tag_nr == port.prev_touched_tag_nr)
		{
			if (retouched)
			{
				// we retouched, so wait at least once more to make sure no more tags get touched
				in_progress++;
			}
			else
			{
				// hasn't been touched since the last call
				incomplete++;
			}
		}
		else
		{
			// the touched tag changed since the last call
			in_progress++;
			port.prev_touched_tag_nr = last_touched_tag_nr;
		}
	}

	return std::make_tuple(active, in_progress, untouched, incomplete);
}

void cell_audio_thread::reset_ports(s32 offset)
{
	// Memset buffer to 0 and tag
	for (u32 port_index = 0; port_index < ports.size(); port_index++)
	{
		audio_port& port = ports[port_index];

		if (port.state != audio_port_state::started) continue;

		auto* buffer = get_buffer(port, offset);
		memset(buffer, 0, port.block_size() * sizeof(float));

		if (cfg.buffering_enabled && port.num_channels)
		{
			port.tag(buffer);

			// One period is about to elapse for this port. count_port_buffer_tags() zeroes this again as
			// soon as it sees a tag move, so it counts periods during which none did.
			if (m_periods_without_tag[port_index] < 0xFF)
			{
				m_periods_without_tag[port_index]++;
			}
		}
	}
}

void cell_audio_thread::advance(u64 timestamp)
{
	ringbuffer->process_resampled_data();

	std::unique_lock lock(mutex);

	// update ports
	reset_ports(0);

	be_t<u64>* indices = nullptr;

	for (audio_port& port : ports)
	{
		if (port.state != audio_port_state::started) continue;

		port.global_counter = m_counter;
		port.active_counter++;
		port.timestamp = timestamp;

		port.cur_pos = port.position(1);

		if (!indices)
		{
			indices = reinterpret_cast<be_t<u64>*>((*shared_memory->shm.observe())->get());
		}

		indices[port.server_index * 2] = port.cur_pos;
	}

	if (cfg.buffering_enabled)
	{
		// Calculate rolling average of enqueued playtime
		m_average_playtime = cfg.period_average_alpha * ringbuffer->get_enqueued_playtime() + (1.0f - cfg.period_average_alpha) * m_average_playtime;
	}

	m_counter++;
	m_last_period_end = timestamp;
	m_dynamic_period = 0;

#ifdef RPCS3_PS4
	ps4_audio_count(s_ps4_audio_counters.periods);
#endif

	// send aftermix event (normal audio event)
	std::array<shared_ptr<lv2_event_queue>, MAX_AUDIO_EVENT_QUEUES> queues;
	u32 queue_count = 0;

	event_period++;

	for (const key_info& key_inf : keys)
	{
		if (key_inf.flags & CELL_AUDIO_EVENTFLAG_NOMIX)
		{
			continue;
		}

		if ((queues[queue_count] = key_inf.port))
		{
			u32 periods = 1;

			if (key_inf.flags & CELL_AUDIO_EVENTFLAG_DECIMATE_2)
			{
				periods *= 2;
			}

			if (key_inf.flags & CELL_AUDIO_EVENTFLAG_DECIMATE_4)
			{
				// If both flags are set periods is set to x8
				periods *= 4;
			}

			if ((event_period ^ key_inf.start_period) & (periods - 1))
			{
				// The time has not come for this key to receive event
				queues[queue_count].reset();
				continue;
			}

			event_sources[queue_count] = key_inf.source;
			event_data3[queue_count] = (key_inf.flags & CELL_AUDIO_EVENTFLAG_BEFOREMIX) ? key_inf.source : 0;
			queue_count++;
		}
	}

	lock.unlock();

	for (u32 i = 0; i < queue_count; i++)
	{
		lv2_obj::notify_all_t notify;
		queues[i]->send(event_sources[i], CELL_AUDIO_EVENT_MIX, 0, event_data3[i]);
	}
}

namespace audio
{
	cell_audio_config::raw_config get_raw_config()
	{
		return
		{
			.audio_device = g_cfg.audio.audio_device,
			.buffering_enabled = static_cast<bool>(g_cfg.audio.enable_buffering),
			.desired_buffer_duration = g_cfg.audio.desired_buffer_duration,
			.enable_time_stretching = static_cast<bool>(g_cfg.audio.enable_time_stretching),
			.time_stretching_threshold = g_cfg.audio.time_stretching_threshold,
			.convert_to_s16 = static_cast<bool>(g_cfg.audio.convert_to_s16),
			.dump_to_file = static_cast<bool>(g_cfg.audio.dump_to_file),
			.channel_layout = g_cfg.audio.channel_layout,
			.renderer = g_cfg.audio.renderer,
			.provider = g_cfg.audio.provider
		};
	}

	void configure_audio(bool force_reset)
	{
		if (g_cfg.audio.provider != audio_provider::cell_audio)
		{
			return;
		}

		if (auto& g_audio = g_fxo->get<cell_audio>(); g_fxo->is_init<cell_audio>())
		{
			// Only reboot the audio renderer if a relevant setting changed
			const cell_audio_config::raw_config new_raw = get_raw_config();

			if (const cell_audio_config::raw_config raw = g_audio.cfg.raw;
				force_reset ||
				raw.audio_device != new_raw.audio_device ||
				raw.desired_buffer_duration != new_raw.desired_buffer_duration ||
				raw.buffering_enabled != new_raw.buffering_enabled ||
				raw.time_stretching_threshold != new_raw.time_stretching_threshold ||
				raw.enable_time_stretching != new_raw.enable_time_stretching ||
				raw.convert_to_s16 != new_raw.convert_to_s16 ||
				raw.renderer != new_raw.renderer ||
				raw.dump_to_file != new_raw.dump_to_file)
			{
				std::lock_guard lock{g_audio.emu_cfg_upd_m};
				g_audio.cfg.new_raw = new_raw;
				g_audio.m_update_configuration = raw.renderer != new_raw.renderer ? audio_backend_update::ALL : audio_backend_update::PARAM;
			}
		}
	}
}

void cell_audio_thread::update_config(bool backend_changed)
{
	std::lock_guard lock(mutex);

	// Clear ringbuffer
	ringbuffer.reset();

	// Reload config
	cfg.reset(backend_changed);

	// Allocate ringbuffer
	ringbuffer.reset(new audio_ringbuffer(cfg));

	// Reset thread state
	reset_counters();
}

void cell_audio_thread::reset_counters()
{
	m_counter = 0;
	m_start_time = ringbuffer->get_timestamp();
	m_last_period_end = m_start_time;
	m_dynamic_period = 0;
	m_audio_should_restart = true;
}

cell_audio_thread::cell_audio_thread()
{
}

void cell_audio_thread::operator()()
{
#ifdef RPCS3_PS4
	// Per-boot state: this thread is created for every boot.
	s_ps4_auto_stretch.store(false, std::memory_order_relaxed);
	s_ps4_port_lines.store(0, std::memory_order_relaxed);
	s_ps4_audio_counters.fine_poll.store(0, std::memory_order_relaxed);
	s_ps4_audio_counters.rate_milli.store(umax, std::memory_order_relaxed);
#endif

	// Initialize loop variables (regardless of provider in order to initialize timestamps)
	reset_counters();

	if (cfg.raw.provider != audio_provider::cell_audio)
	{
		return;
	}

	// Init audio config
	cfg.reset();

	// Allocate ringbuffer
	ringbuffer.reset(new audio_ringbuffer(cfg));

	thread_ctrl::scoped_priority high_prio(+1);

#ifdef RPCS3_PS4
	// Read once per boot. The HR waits, the lateness policy and automatic continuity change the audio clock every game
	// sees, so each is off by default (the 0.38 paths below) and comes on with its enable flag or features.txt name
	// (util/ps4_safe_mode.hpp); the older kill flags (audio-poll-2ms, audio-upstream-skip, audio-no-stretch) still win.
	namespace ps4_sm = utils::ps4::safe_mode;
	const bool ps4_poll_2ms = fs::is_file("/data/rpcs3/audio-poll-2ms.flag");
	const bool ps4_fine_poll = fs::is_file("/data/rpcs3/audio-fine-poll.flag");
	// Off: upstream lateness handling (drop a block late by 10.7 ms, then silence without waiting).
	const bool ps4_policy = ps4_sm::enabled(ps4_sm::feature::audio_wait_policy);
	// Automatic continuity: the rate-matched tempo engages by itself when the game cannot keep real time, and also
	// serves a game's own Time Stretching setting. Off: no automatic engage, upstream controller.
	const bool ps4_continuity = ps4_sm::enabled(ps4_sm::feature::audio_continuity);
	const bool ps4_hr_rule = ps4_sm::enabled(ps4_sm::feature::audio_hr_waits);
	// Guards, on by default: the parked AudioOut thread (with it, the rebuild backoff of a failed backend below) and
	// the unbuffered clock rebase. Their kill flags give the upstream paths.
	const bool ps4_parked_out = ps4_sm::enabled(ps4_sm::feature::audio_out_parked);
	const bool ps4_unbuffered_rebase = ps4_sm::enabled(ps4_sm::feature::audio_unbuffered_rebase);

	// Orbis ends every timed wait (thread_ctrl::wait_for, sceKernelUsleep) on the next ~1 ms tick: wait_for(1000) takes
	// ~2.0 ms and wait_for(100) ~1.0 ms. Off (0.38): while cellAudio waits for the game's next block, 100 us gives the
	// ~1 ms poll the PC gets from 1000 (untouched) and 500 (in progress); audio-poll-2ms.flag restores 1000/500.
	// On: the HR equeue timer ends ~30 us after its deadline (boot timer probe), so the period waits end on time and the
	// polls for the game's block are real: 250 us untouched, 500 us in progress (the PC's quiet time). Each HR wait is
	// at most 6.5 ms and not interruptible, so a stop is still seen within one period.
	const auto ps4_rules = utils::ps4::audio_policy::rules_for(ps4_hr_rule, ps4_poll_2ms, utils::ps4::hr_wait_enabled(), ps4_policy, ps4_continuity);
	const bool ps4_hr = ps4_rules.hr;
	const u64 ps4_untouched_poll_us = ps4_rules.untouched_poll_us;
	const u64 ps4_in_progress_poll_us = ps4_rules.in_progress_poll_us;

	const auto ps4_audio_wait = [&](u64 us)
	{
		if (ps4_hr && us && us <= 6500)
		{
			utils::ps4::hr_wait_us(us);
		}
		else
		{
			thread_ctrl::wait_for(us);
		}
	};

	if (!ps4_hr_rule)
	{
		ps4_audio_checkpoint(ps4_poll_2ms
			? "cellAudio: block poll 2 ms while waiting for the game (audio-poll-2ms.flag)"
			: "cellAudio: block poll ~1 ms while waiting for the game (audio-poll-2ms.flag restores 2 ms)");
	}
	else
	{
		ps4_audio_checkpoint(ps4_hr ? "cellAudio: HR waits on (poll 250 us)"
			: "cellAudio: HR waits off (HR timer disabled or its boot probe failed): block poll ~1 ms while waiting for the game");
	}

	// Limits of the lateness policy: the upstream timeouts, then while the output holds more than 3 periods (16 ms),
	// up to 64 ms from the last period.
	const auto ps4_limits = [&]()
	{
		utils::ps4::audio_policy::limits l{};
		l.fully_untouched_timeout = cfg.fully_untouched_timeout;
		l.partially_untouched_timeout = cfg.partially_untouched_timeout;
		l.reserve_guard = u64{cfg.audio_block_period} * 3;
		l.wait_cap = 64'000;
		return l;
	};

	if (fs::is_file("/data/rpcs3/audio-auto-stretch.flag"))
	{
		// The 0.34-0.38 opt-in. Its two-window engage is gone; the file is now an alias of audio-continuity.flag.
		ps4_audio_checkpoint(ps4_continuity
			? "cellAudio: audio-auto-stretch.flag: automatic continuity on (same as audio-continuity.flag; the two-window engage of 0.34-0.38 is gone)"
			: "cellAudio: audio-auto-stretch.flag: automatic continuity stays off for this boot (audio-no-stretch.flag or safe mode)");
	}

	{
		const auto l = ps4_limits();
		ps4_audio_checkpoint(fmt::format("cellAudio: PS4 audio policy: HR waits %s, reserve %u ms, %s, %s%s",
			ps4_hr ? "on" : "off", (cfg.desired_buffer_duration + 500) / 1000,
			!cfg.buffering_enabled ? "buffering off: no waits for the game" :
			ps4_policy ? fmt::format("wait up to %u ms while reserve > %u ms, idle after %u ms", l.wait_cap / 1000, (l.reserve_guard + 500) / 1000, utils::ps4::audio_policy::idle_after_us / 1000) :
			fmt::format("upstream skip after %.1f ms", l.fully_untouched_timeout / 1000.0),
			!cfg.buffering_enabled ? "continuity off" :
			!ps4_continuity ? (cfg.time_stretching_enabled ? "continuity off (upstream Time Stretching set for this game)" : "continuity off") :
			cfg.time_stretching_enabled ? "continuity on (Time Stretching set for this game)" : "continuity auto",
			!ps4_hr_rule && !ps4_policy && !ps4_continuity ? " (0.38 default)" : ""));
	}

	if (fs::is_file("/data/rpcs3/audio-high-priority.flag"))
	{
		// One step above the raised RSX thread: equal priorities are round-robin, so a woken cellAudio thread can
		// queue behind a busy RSX for a quantum. high_prio restores the base priority when this thread ends.
		const int base = ps4_thread_base_native_priority();

		if (base - 16 >= 256 && base <= 767 && scePthreadSetprio(scePthreadSelf(), base - 16) == 0)
		{
			ps4_audio_checkpoint(fmt::format("cellAudio: audio threads at priority %d (audio-high-priority.flag)", base - 16));
		}
		else
		{
			ps4_audio_checkpoint(fmt::format("cellAudio: audio-high-priority.flag ignored (base priority %d could not be raised two steps)", base));
		}
	}

	if (ps4_fine_poll)
	{
		s_ps4_audio_counters.fine_poll.store(1, std::memory_order_relaxed);
		ps4_audio_checkpoint("cellAudio: fine block poll armed (audio-fine-poll.flag): yields at the base priority instead of the ~1 ms timed wait while the game paces the audio, at most 150 ms per second");
	}

	// Local 10 s window for the fine poll decision (the shared counters belong to the AudioOut line).
	const bool ps4_window_on = ps4_fine_poll;
#endif

	while (Emu.IsPausedOrReady())
	{
		thread_ctrl::wait_for(5000);
	}

	u32 untouched_expected = 0;

	u32 loop_count = 0;

#ifdef RPCS3_PS4
	// Time (same clock as 'timestamp') this thread asked to wake up at in its last period wait; 0 = none pending
	u64 ps4_wake_target = 0;
	// Time of the first wait for the game in the current period (same clock); 0 = no wait yet
	u64 ps4_wait_start = 0;

	struct ps4_local_window
	{
		u64 start = 0; // 0 = not started
		u64 periods = 0;
		u64 silent = 0; // of 'periods': silence enqueued without waiting, at the dynamic period (faster than real time)
		u64 host_late = 0;
		u64 wait_us = 0;
		bool ports = false; // a started port was seen
		bool valid = true;  // false after a backend failure inside the window
	};

	ps4_local_window ps4_win{};
	bool ps4_assist = false;
	u64 ps4_budget_start = 0;
	u64 ps4_budget_used = 0;

	// Time of the first wait for an untouched block in the current period (same clock); 0 = none
	u64 ps4_untouched_since = 0;
	// A wait of the current period went past the upstream timeout, allowed by the output reserve
	bool ps4_reserve_waited = false;

	// Backend rebuilds while it failed: time of the last one (same clock), rebuilds of the current failure episode,
	// failure episodes of this boot
	u64 ps4_last_rebuild = 0;
	u32 ps4_rebuilds = 0;
	u32 ps4_failures = 0;

	// Unbuffered period clock: more than 8 periods behind (a boot wait, a pause or a host stall) it is rebased so the
	// next period is due now, instead of firing every missed period back to back as MIX events (the unbuffered ring
	// drops them, and the game gets a burst of events for audio nobody hears). At most one RPCS3.log line per 10 s; the
	// first of the boot also goes to startup.log.
	u64 ps4_rebase_line_at = 0;
	u32 ps4_rebases_unlogged = 0;
	bool ps4_rebase_noted = false;
	const auto ps4_rebase_unbuffered = [&](u64 now)
	{
		namespace ap = utils::ps4::audio_policy;

		if (!ps4_unbuffered_rebase)
		{
			return;
		}

		const u64 behind = ap::unbuffered_behind_us(m_counter, m_start_time, now, cfg.audio_block_period);

		if (!ap::unbuffered_rebase_due(behind, cfg.audio_block_period))
		{
			return;
		}

		m_start_time = ap::unbuffered_rebased_start(m_counter, now, cfg.audio_block_period);

		if (ps4_rebase_line_at && now - ps4_rebase_line_at < 10'000'000)
		{
			ps4_rebases_unlogged++;
			return;
		}

		ps4_rebase_line_at = now;
		const std::string line = fmt::format("cellAudio: unbuffered audio clock rebased after %u ms (boot wait, pause or stall); audio-unbuffered-catchup.flag keeps the catch-up%s",
			behind / 1000, ps4_rebases_unlogged ? fmt::format(" (%u more rebase(s) since the last line)", ps4_rebases_unlogged) : std::string());
		ps4_rebases_unlogged = 0;

		if (!std::exchange(ps4_rebase_noted, true))
		{
			ps4_audio_checkpoint(line);
		}
		else
		{
			cellAudio.notice("%s", line);
		}
	};

	// Set by the buffering branch for the period about to end: output fill against its target (now and averaged),
	// untouched idle ports, and what a mix of it carries.
	enum class ps4_period : u8
	{
		none,   // buffering off: nothing measured
		silent, // no port, or every untouched port idle: the game was not expected to deliver
		late,   // given up: the game's block is lost
		game,   // mixed with at least one written port
	};

	f64 ps4_fill = 1.0;
	f64 ps4_fill_avg = 1.0;
	u32 ps4_idle_now = 0;

	utils::ps4::audio_policy::rate_tracker ps4_rate{};
	utils::ps4::audio_policy::continuity_trigger ps4_trigger{};
	// Rate-matched tempo, moved by at most tempo_slew per period; the resampler gets it in tempo_apply steps.
	f64 ps4_tempo = 1.0;

	// Right before each advance(): ends this period's wait for the game, counts the period in the local window and in
	// the AudioOut line, updates the ports' cadence and the production rate, engages automatic continuity and moves
	// the tempo.
	const auto ps4_period_done = [&](u64 now, ps4_period kind)
	{
		namespace ap = utils::ps4::audio_policy;
		auto& c = s_ps4_audio_counters;
		const u64 period_us = now - m_last_period_end;

		if (ps4_wait_start)
		{
			const u64 waited = now - ps4_wait_start;
			c.wait_us.fetch_add(waited, std::memory_order_relaxed);
			ps4_win.wait_us += waited;
			ps4_wait_start = 0;
		}

		const bool waited = ps4_untouched_since != 0;

		if (waited)
		{
			const u64 late = now - ps4_untouched_since;
			ps4_audio_count(c.late_hist[ap::late_bucket(late)]);
			ps4_audio_max(c.late_max_us, late);
			ps4_untouched_since = 0;
		}

		if (ps4_reserve_waited)
		{
			ps4_audio_count(c.reserve_waits);
			ps4_reserve_waited = false;
		}

		ps4_win.periods++;

		if (kind != ps4_period::none)
		{
			ps4_audio_max(c.idle_ports_max, ps4_idle_now);

			for (ps4_port_cadence& cadence : m_ps4_cadence)
			{
				if (cadence.started)
				{
					cadence.streak = cadence.untouched ? 0 : static_cast<u8>(std::min<u32>(cadence.streak + 1u, 0xffu));
				}
			}

			if (kind == ps4_period::silent)
			{
				// Only periods in which the game was expected to deliver count toward an engage.
				ps4_trigger = {};
			}
			else
			{
				ps4_rate.add(period_us, kind == ps4_period::game, waited, cfg.audio_block_period);

				if (ps4_rate.valid())
				{
					c.rate_milli.store(static_cast<u64>(ps4_rate.rate(cfg.audio_block_period) * 1000.0 + 0.5), std::memory_order_relaxed);
				}

				if (ps4_continuity && cfg.buffering_enabled && !cfg.time_stretching_enabled &&
					ps4_trigger.add(period_us, ps4_rate.valid(), ps4_rate.rate(cfg.audio_block_period), ps4_fill_avg))
				{
					// Only this thread reads cfg in the ring paths, so the switch needs no ring reset: the resampler was
					// set up with the ring (tempo 1.0) and takes over from the next enqueue. It stays on for the boot.
					s_ps4_auto_stretch.store(true, std::memory_order_relaxed);
					cfg.time_stretching_enabled = true;
					cfg.time_stretching_threshold = 0.75f;
					ps4_tempo = ringbuffer->get_frequency_ratio();
					ps4_audio_checkpoint(fmt::format("cellAudio: automatic continuity engaged (rate %.2f, fill %.2f)", ps4_rate.rate(cfg.audio_block_period), ps4_fill_avg));
				}
			}

			if (ps4_continuity && cfg.time_stretching_enabled)
			{
				ps4_tempo = ap::tempo_step(ps4_tempo, ap::tempo_target(ps4_rate.rate(cfg.audio_block_period), ps4_fill));

				if (ap::tempo_should_apply(ps4_tempo, ringbuffer->get_frequency_ratio()))
				{
					ringbuffer->set_frequency_ratio(static_cast<f32>(ps4_tempo));
				}

				ps4_audio_min(c.stretch_ratio_milli, static_cast<u64>(ringbuffer->get_frequency_ratio() * 1000.0f + 0.5f));
			}
		}

		c.tempo_milli_sum.fetch_add(static_cast<u64>(ringbuffer->get_frequency_ratio() * 1000.0f + 0.5f), std::memory_order_relaxed);
		ps4_audio_count(c.tempo_periods);
	};

	// Read-only twin of the tag test in count_port_buffer_tags(): did any buffer tag of a started port move since
	// that function last ran? Leaves the per-port tag state to count_port_buffer_tags(), so it still sees the move.
	const auto ps4_tags_moved = [&]() -> bool
	{
		std::lock_guard lock(mutex);

		for (const audio_port& port : ports)
		{
			if (port.state != audio_port_state::started || !port.num_channels) continue;

			const auto port_buf = get_buffer(port);
			const u32 tag_first_pos = port.num_channels == 2 ? PORT_BUFFER_TAG_FIRST_2CH : PORT_BUFFER_TAG_FIRST_8CH;
			const u32 tag_delta = port.num_channels == 2 ? PORT_BUFFER_TAG_DELTA_2CH : PORT_BUFFER_TAG_DELTA_8CH;

			for (u32 tag_pos = tag_first_pos, tag_nr = 0; tag_nr < PORT_BUFFER_TAG_COUNT; tag_pos += tag_delta, tag_nr++)
			{
				const f32 val = port_buf[tag_pos];
				const f32 last_val = port.last_tag_value[tag_nr];

				if (val != last_val || (last_val == -0.0f && std::signbit(last_val) && !std::signbit(val)))
				{
					return true;
				}
			}
		}

		return false;
	};

	// audio-fine-poll.flag, while the game paces the audio: instead of a timed wait (which ends on the next ~1 ms
	// Orbis tick) poll by yielding at the process base priority, so the guest threads keep their cores. 'touch':
	// return as soon as a buffer tag moves (checked every ~100 us) or at 'until'; otherwise wait until 'until'
	// (the in-progress quiet time). The raised priority is restored before returning, so before any mix. At most
	// 150 ms of polling per second. Returns false when not used: the caller does its timed wait.
	const auto ps4_fine_wait = [&](u64 now, u64 until, bool touch) -> bool
	{
		if (!ps4_assist)
		{
			return false;
		}

		if (now - ps4_budget_start >= 1'000'000)
		{
			ps4_budget_start = now;
			ps4_budget_used = 0;
		}

		if (ps4_budget_used >= 150'000)
		{
			return false;
		}

		const auto self = scePthreadSelf();
		const int base = ps4_thread_base_native_priority();
		int prio = 0;
		int saved = -1;

		if (base >= 256 && base <= 767 && scePthreadGetprio(self, &prio) == 0 && prio < base && scePthreadSetprio(self, base) == 0)
		{
			saved = prio;
		}

		u64 t = now;

		for (u64 next_check = now + 100;;)
		{
			std::this_thread::yield();
			t = get_system_time();

			if (t >= until || thread_ctrl::state() == thread_state::aborting)
			{
				break;
			}

			if (touch && t >= next_check)
			{
				next_check = t + 100;

				if (ps4_tags_moved())
				{
					break;
				}
			}
		}

		if (saved >= 0)
		{
			scePthreadSetprio(self, saved);
		}

		const u64 used = t > now ? t - now : 0;
		ps4_budget_used += used;
		s_ps4_audio_counters.poll_us.fetch_add(used, std::memory_order_relaxed);
		return true;
	};
#endif

	// Main cellAudio loop
	while (thread_ctrl::state() != thread_state::aborting)
	{
		loop_count++;

		const audio_backend_update update_req = m_update_configuration.observe();
		if (update_req != audio_backend_update::NONE)
		{
			cellAudio.warning("Updating cell_audio_thread configuration");
			{
				std::lock_guard lock{emu_cfg_upd_m};
				cfg.raw = cfg.new_raw;
				m_update_configuration = audio_backend_update::NONE;
			}
			update_config(update_req == audio_backend_update::ALL);
		}

		const bool emu_paused = Emu.IsPaused();
		const u64 timestamp = ringbuffer->update(emu_paused || m_backend_failed);

		if (emu_paused)
		{
			m_audio_should_restart = true;
			ringbuffer->flush();
#ifdef RPCS3_PS4
			ps4_wake_target = 0;
			ps4_wait_start = 0;
			ps4_untouched_since = 0;
			ps4_reserve_waited = false;
			// A window that spans a pause says nothing about the game's pace.
			ps4_win.start = 0;
#endif
			thread_ctrl::wait_for(10000);
			continue;
		}

#ifdef RPCS3_PS4
		if (ps4_wake_target)
		{
			// Woken more than one audio period after the requested time: the host ran this thread late (scheduling
			// or a long sleep tick), whatever the game did.
			if (timestamp > ps4_wake_target)
			{
				const u64 late = timestamp - ps4_wake_target;
				auto& c = s_ps4_audio_counters;

				if (late > cfg.audio_block_period)
				{
					ps4_audio_count(c.host_late);
					ps4_win.host_late++;
				}

				if (late > c.max_wake_late_us.load(std::memory_order_relaxed))
				{
					c.max_wake_late_us.store(late, std::memory_order_relaxed);
				}
			}

			ps4_wake_target = 0;
		}

		if (ps4_window_on)
		{
			if (!ps4_win.start)
			{
				ps4_win = {};
				ps4_win.start = timestamp;
			}
			else if (const u64 elapsed = timestamp - ps4_win.start; elapsed >= 10'000'000)
			{
				// Scaled to 10 s like the AudioOut line: 1875 periods = real time.
				const u64 periods = ps4_win.periods * 10'000'000 / elapsed;
				const u64 wait_ms = ps4_win.wait_us * 10'000 / elapsed;
				const bool clean = ps4_win.valid && ps4_win.ports && ps4_win.host_late == 0 && cfg.buffering_enabled;

				// The decisions count game periods: silent ones run faster than real time while the ring is short, so
				// a game that falls behind still showed more than 1875 periods (Wolverine: 2076-2150 with 1172-1414
				// silent). A window with under 500 game periods carries too little game audio to judge its pace.
				const u64 game = (ps4_win.periods - std::min(ps4_win.silent, ps4_win.periods)) * 10'000'000 / elapsed;
				const bool game_audio = game >= 500;

				if (ps4_fine_poll)
				{
					// On when the game paced the audio; stays on while cellAudio still waits for it (the poll itself
					// brings the periods back toward 1875).
					const bool assist = clean && ((game_audio && game <= 1800) || (ps4_assist && wait_ms >= 500));

					if (assist != ps4_assist)
					{
						cellAudio.notice("Fine block poll %s (game periods %u, periods %u of 1875, waited %u ms per 10 s, host late %u)", assist ? "on" : "off", game, periods, wait_ms, ps4_win.host_late);
					}

					ps4_assist = assist;
					s_ps4_audio_counters.fine_poll.store(assist ? 2 : 1, std::memory_order_relaxed);
				}

				ps4_win = {};
				ps4_win.start = timestamp;
			}
		}
#endif

		// TODO: (no idea how much of this is already implemented)
		// The hardware heartbeat interval of libaudio is ~5.3ms.
		// As soon as one interval starts, libaudio waits for ~2.6ms (half of the interval) before it mixes the audio.
		// There are 2 different types of games:
		// - Normal games:
		//     Once the audio was mixed, we send the CELL_AUDIO_EVENT_MIX event and the game can process audio.
		// - Latency sensitive games:
		//     If CELL_AUDIO_EVENTFLAG_BEFOREMIX is specified, we immediately send the CELL_AUDIO_EVENT_MIX event and the game can process audio.
		//     We then have to wait for a maximum of ~2.6ms for cellAudioSendAck and then mix immediately.

		const u64 time_since_last_period = timestamp - m_last_period_end;

		// Handle audio restart
		if (m_audio_should_restart)
		{
#ifdef RPCS3_PS4
			// The first periods after the boot wait or a pause: the same rebase, before the alignment below
			if (!cfg.buffering_enabled)
			{
				ps4_rebase_unbuffered(timestamp);
			}
#endif
			// align to 5.(3)ms on global clock - some games seem to prefer this
			const s64 audio_period_alignment_delta = (timestamp - m_start_time) % cfg.audio_block_period;
			if (audio_period_alignment_delta > cfg.period_comparison_margin)
			{
				thread_ctrl::wait_for(audio_period_alignment_delta - cfg.period_comparison_margin);
			}

			if (cfg.buffering_enabled)
			{
				// Restart algorithm
				cellAudio.trace("restarting audio");
				ringbuffer->enqueue_silence(cfg.desired_full_buffers, true);
				finish_port_volume_stepping();
				m_average_playtime = static_cast<f32>(ringbuffer->get_enqueued_playtime());
				untouched_expected = 0;
			}

			m_audio_should_restart = false;
#ifdef RPCS3_PS4
			ps4_wait_start = 0;
			ps4_untouched_since = 0;
			ps4_reserve_waited = false;
			// A new or flushed ring starts at tempo 1.0; the engage count starts over (the rate is the game's, kept).
			ps4_tempo = ringbuffer->get_frequency_ratio();
			ps4_trigger = {};
#endif
			continue;
		}

		bool operational = ringbuffer->get_operational_status();

#ifdef RPCS3_PS4
		// With the parked AudioOut thread a backend that failed (most often a refused output thread at the console's
		// thread cap, which can last minutes) is rebuilt at most once per 2 s instead of every 128 loops, and the lines
		// of a rebuild are logged for the first rebuild of a failure episode, then for every 64th.
		// audio-thread-per-play.flag: the upstream rebuild.
		if (!operational && loop_count % 128 == 0 && (!ps4_parked_out || utils::ps4::audio_rebuild_due(timestamp, ps4_last_rebuild)))
		{
			if (ps4_parked_out)
			{
				ps4_last_rebuild = timestamp;
				s_ps4_quiet_rebuild.store(!utils::ps4::audio_recovery_line_due(++ps4_rebuilds), std::memory_order_relaxed);
			}

			update_config(true);
			s_ps4_quiet_rebuild.store(false, std::memory_order_relaxed);
			operational = ringbuffer->get_operational_status();
		}
#else
		if (!operational && loop_count % 128 == 0)
		{
			update_config(true);
			operational = ringbuffer->get_operational_status();
		}
#endif

		if (ringbuffer->device_changed())
		{
			cellAudio.warning("Default device changed, attempting to switch...");
			update_config(false);

			if (operational != ringbuffer->get_operational_status())
			{
				continue;
			}
		}

#ifdef RPCS3_PS4
		if (!m_backend_failed && !operational)
		{
			// One failure episode: the first of the boot and every 64th are logged (a backend that keeps failing and
			// recovering would otherwise print both lines every time)
			if (!ps4_parked_out || ++ps4_failures == 1)
			{
				cellAudio.error("Backend stopped unexpectedly (likely device change). Attempting to recover...");
			}
			else if (utils::ps4::audio_recovery_line_due(ps4_failures))
			{
				cellAudio.error("Backend stopped unexpectedly (likely device change). Attempting to recover... (failure %u of this boot; logged for the first and every 64th)", ps4_failures);
			}

			m_backend_failed = true;
		}
		else if (m_backend_failed && operational)
		{
			if (!ps4_parked_out || utils::ps4::audio_recovery_line_due(ps4_failures))
			{
				cellAudio.success("Backend recovered");
			}

			ps4_rebuilds = 0;
			m_backend_failed = false;
		}
#else
		if (!m_backend_failed && !operational)
		{
			cellAudio.error("Backend stopped unexpectedly (likely device change). Attempting to recover...");
			m_backend_failed = true;
		}
		else if (m_backend_failed && operational)
		{
			cellAudio.success("Backend recovered");
			m_backend_failed = false;
		}
#endif

#ifdef RPCS3_PS4
		if (m_backend_failed)
		{
			ps4_win.valid = false;
		}

		// Output ring fill read below (buffering only), kept for the window minimum if this iteration mixes.
		u64 ps4_ring_us = umax;
		// What a mix of this iteration carries (set by the buffering branch).
		ps4_period ps4_mix_kind = ps4_period::none;
#endif

		if (!cfg.buffering_enabled)
		{
#ifdef RPCS3_PS4
			ps4_rebase_unbuffered(timestamp);
#endif
			const u64 period_end = (m_counter * cfg.audio_block_period) + m_start_time;
			const s64 time_left = period_end - timestamp;

			if (time_left > cfg.period_comparison_margin)
			{
#ifdef RPCS3_PS4
				ps4_wake_target = timestamp + (time_left - cfg.period_comparison_margin);
				ps4_audio_wait(time_left - cfg.period_comparison_margin);
#else
				thread_ctrl::wait_for(time_left - cfg.period_comparison_margin);
#endif
				continue;
			}
		}
		else
		{
			const u64 enqueued_samples = ringbuffer->get_enqueued_samples();
			const f32 frequency_ratio = ringbuffer->get_frequency_ratio();
			const u64 enqueued_playtime = ringbuffer->get_enqueued_playtime();
			const u64 enqueued_buffers = enqueued_samples / AUDIO_BUFFER_SAMPLES;

			const auto tag_info = count_port_buffer_tags();
			const u32 active_ports = std::get<0>(tag_info);
			const u32 in_progress  = std::get<1>(tag_info);
			const u32 untouched    = std::get<2>(tag_info);
			const u32 incomplete   = std::get<3>(tag_info);

#ifdef RPCS3_PS4
			ps4_ring_us = enqueued_playtime;
			ps4_idle_now = m_ps4_untouched_idle;
			ps4_mix_kind = untouched < active_ports ? ps4_period::game : ps4_period::silent;

			if (active_ports)
			{
				ps4_win.ports = true;
			}
#endif

			// Ratio between the rolling average of the audio period, and the desired audio period
			const f32 average_playtime_ratio = m_average_playtime / cfg.audio_buffer_length;

			// Use the above average ratio to decide how much buffer we should be aiming for
			f32 desired_duration_adjusted = cfg.desired_buffer_duration + (cfg.audio_block_period / 2.0f);
			if (average_playtime_ratio < 1.0f)
			{
				desired_duration_adjusted /= std::max(average_playtime_ratio, 0.25f);
			}

#ifdef RPCS3_PS4
			ps4_fill = enqueued_playtime / desired_duration_adjusted;
			ps4_fill_avg = m_average_playtime / desired_duration_adjusted;

			// With automatic continuity the rate-matched tempo (once per period, ps4_period_done) replaces this
			// controller, which halves the tempo while the ring is low.
			if (cfg.time_stretching_enabled && !ps4_continuity)
#else
			if (cfg.time_stretching_enabled)
#endif
			{
				//  1.0 means exactly as desired
				// <1.0 means not as full as desired
				// >1.0 means more full than desired
				const f32 desired_duration_rate = enqueued_playtime / desired_duration_adjusted;

				// update frequency ratio if necessary
				if (desired_duration_rate < cfg.time_stretching_threshold)
				{
					const f32 normalized_desired_duration_rate = desired_duration_rate / cfg.time_stretching_threshold;

					// change frequency ratio in steps
					const f32 req_time_stretching_step = (normalized_desired_duration_rate + frequency_ratio) / 2.0f;
					if (std::abs(req_time_stretching_step - frequency_ratio) > cfg.time_stretching_step)
					{
						ringbuffer->set_frequency_ratio(req_time_stretching_step);
					}
				}
				else if (frequency_ratio != RESAMPLER_MAX_FREQ_VAL)
				{
					ringbuffer->set_frequency_ratio(RESAMPLER_MAX_FREQ_VAL);
				}

#ifdef RPCS3_PS4
				ps4_audio_min(s_ps4_audio_counters.stretch_ratio_milli, static_cast<u64>(ringbuffer->get_frequency_ratio() * 1000.0f + 0.5f));
#endif
			}

			//  1.0 means exactly as desired
			// <1.0 means not as full as desired
			// >1.0 means more full than desired
			const f32 desired_duration_rate = enqueued_playtime / desired_duration_adjusted;

			if (desired_duration_rate >= 1.0f)
			{
				// more full than desired
				const f32 multiplier = 1.0f / desired_duration_rate;
				m_dynamic_period = cfg.maximum_block_period - static_cast<u64>((cfg.maximum_block_period - cfg.audio_block_period) * multiplier);
			}
			else
			{
				// not as full as desired
				const f32 multiplier = desired_duration_rate * desired_duration_rate; // quite aggressive, but helps more times than it hurts
				m_dynamic_period = cfg.minimum_block_period + static_cast<u64>((cfg.audio_block_period - cfg.minimum_block_period) * multiplier);
			}

			const s64 time_left = m_dynamic_period - time_since_last_period;
			if (time_left > cfg.period_comparison_margin)
			{
#ifdef RPCS3_PS4
				ps4_wake_target = timestamp + (time_left - cfg.period_comparison_margin);
				ps4_audio_wait(time_left - cfg.period_comparison_margin);
#else
				thread_ctrl::wait_for(time_left - cfg.period_comparison_margin);
#endif
				continue;
			}

#ifdef RPCS3_PS4
			if (ps4_policy)
			{
				// Lateness policy (ps4_audio_policy.hpp): wait for a late block while the output still holds more than
				// the guard, never fill an active port's period with silence without waiting for it, and mix the
				// written ports of a period whatever a late sibling does. With the policy off (the default) only the
				// upstream checks below run.
				namespace ap = utils::ps4::audio_policy;

				ap::port_counts ps4_ports{};
				ps4_ports.active = active_ports;
				ps4_ports.untouched = untouched;
				ps4_ports.idle = m_ps4_untouched_idle;
				ps4_ports.regular = m_ps4_untouched_regular;
				ps4_ports.in_progress = in_progress;

				const ap::limits ps4_lim = ps4_limits();
				const ap::decision ps4_decision = ap::decide(ps4_ports, time_since_last_period, enqueued_playtime, ps4_lim, static_cast<bool>(g_cfg.audio.disable_sampling_skip));

				switch (ps4_decision.what)
				{
				case ap::action::silence:
				{
					// No port, or every untouched port idle for 100 ms: nothing to wait for
					cellAudio.trace("enqueuing silence: untouched=%u/%u (idle %u), enqueued_buffers=%llu", untouched, active_ports, m_ps4_untouched_idle, enqueued_buffers);
					ps4_audio_count(s_ps4_audio_counters.silent);
					ps4_win.silent++;
					ps4_period_done(timestamp, ps4_period::silent);
					ringbuffer->enqueue_silence();
					untouched_expected = 0;
					advance(timestamp);
					continue;
				}
				case ap::action::skip:
				{
					// Late past the limits with no reserve left: nothing is enqueued, the game's block is lost
					cellAudio.trace("advancing time: untouched=%u/%u (idle %u), enqueued_buffers=%llu", untouched, active_ports, m_ps4_untouched_idle, enqueued_buffers);
					ps4_audio_count(s_ps4_audio_counters.game_late);
					ps4_period_done(timestamp, ps4_period::late);
					untouched_expected = 0;
					advance(timestamp);
					continue;
				}
				case ap::action::wait_untouched:
				{
					ps4_audio_count(s_ps4_audio_counters.untouched_waits);

					if (!ps4_wait_start)
					{
						ps4_wait_start = timestamp;
					}

					if (!ps4_untouched_since)
					{
						ps4_untouched_since = timestamp;
					}

					ps4_reserve_waited |= ps4_decision.past_timeout;

					const u64 ps4_timeout_at = m_last_period_end + 1 + (ps4_decision.past_timeout ? ps4_lim.wait_cap :
						untouched == active_ports ? ps4_lim.fully_untouched_timeout : ps4_lim.partially_untouched_timeout);

					if (ps4_fine_wait(timestamp, std::clamp<u64>(ps4_timeout_at, timestamp + 100, timestamp + 1500), true))
					{
						continue;
					}

					ps4_wake_target = timestamp + ps4_untouched_poll_us;
					ps4_audio_wait(ps4_untouched_poll_us);
					continue;
				}
				case ap::action::wait_in_progress:
				{
					ps4_audio_count(s_ps4_audio_counters.in_progress_waits);

					if (!ps4_wait_start)
					{
						ps4_wait_start = timestamp;
					}

					// The PC's 500 us quiet time: count_port_buffer_tags() treats a buffer whose tags did not move
					// since its previous call as finished, so polling faster would mix half-written buffers.
					if (ps4_fine_wait(timestamp, timestamp + 500, false))
					{
						continue;
					}

					ps4_wake_target = timestamp + ps4_in_progress_poll_us;
					ps4_audio_wait(ps4_in_progress_poll_us);
					continue;
				}
				case ap::action::mix:
				{
					// Nothing left to wait for: with untouched_expected equal to untouched, the upstream checks below
					// fall through to the mix (decide() never mixes with every port untouched or one in progress).
					untouched_expected = untouched;
					break;
				}
				}
			}
#endif

			// Fast path for 0 ports active
			if (active_ports == 0)
			{
				// no need to mix, just enqueue silence and advance time
				cellAudio.trace("enqueuing silence: no active ports, enqueued_buffers=%llu", enqueued_buffers);
#ifdef RPCS3_PS4
				ps4_audio_count(s_ps4_audio_counters.silent);
				ps4_win.silent++;
				ps4_period_done(timestamp, ps4_period::silent);
#endif
				ringbuffer->enqueue_silence();
				untouched_expected = 0;
				advance(timestamp);
				continue;
			}

			// Wait until buffers have been touched
			//cellAudio.error("active=%u, in_progress=%u, untouched=%u, incomplete=%u", active_ports, in_progress, untouched, incomplete);
			if (untouched > untouched_expected)
			{
				// Games may sometimes "skip" audio periods entirely if they're falling behind (a sort of "frameskip" for audio)
				// As such, if the game doesn't touch buffers for too long we advance time hoping the game recovers
				if (
					(untouched == active_ports && time_since_last_period > cfg.fully_untouched_timeout) ||
					(time_since_last_period > cfg.partially_untouched_timeout) || g_cfg.audio.disable_sampling_skip
				   )
				{
					// There's no audio in the buffers, simply advance time and hope the game recovers
					cellAudio.trace("advancing time: untouched=%u/%u (expected=%u), enqueued_buffers=%llu", untouched, active_ports, untouched_expected, enqueued_buffers);
#ifdef RPCS3_PS4
					// Nothing is enqueued for this period: the output runs short because the game was late
					ps4_audio_count(s_ps4_audio_counters.game_late);
					ps4_period_done(timestamp, ps4_period::late);
#endif
					untouched_expected = untouched;
					advance(timestamp);
					continue;
				}

				cellAudio.trace("waiting: untouched=%u/%u (expected=%u), enqueued_buffers=%llu", untouched, active_ports, untouched_expected, enqueued_buffers);
#ifdef RPCS3_PS4
				ps4_audio_count(s_ps4_audio_counters.untouched_waits);

				if (!ps4_wait_start)
				{
					ps4_wait_start = timestamp;
				}

				if (!ps4_untouched_since)
				{
					ps4_untouched_since = timestamp;
				}

				// The fine poll stops by itself at the untouched timeout that ends this wait in the branch above.
				const u64 ps4_timeout_at = m_last_period_end + 1 + (untouched == active_ports ? cfg.fully_untouched_timeout : cfg.partially_untouched_timeout);

				if (ps4_fine_wait(timestamp, std::clamp<u64>(ps4_timeout_at, timestamp + 100, timestamp + 1500), true))
				{
					continue;
				}

				// The host_late counter compares the wake-up with this target: the poll's overshoot (~30 us with the HR
				// timer, ~0.9 ms on the tick) stays far below its one-period threshold.
				ps4_wake_target = timestamp + ps4_untouched_poll_us;
				ps4_audio_wait(ps4_untouched_poll_us);
#else
				thread_ctrl::wait_for(1000);
#endif
				continue;
			}

			// Fast-path for when there is no audio in the buffers
			if (untouched == active_ports)
			{
				// There's no audio in the buffers, simply advance time
				cellAudio.trace("enqueuing silence: untouched=%u/%u (expected=%u), enqueued_buffers=%llu", untouched, active_ports, untouched_expected, enqueued_buffers);
#ifdef RPCS3_PS4
				ps4_audio_count(s_ps4_audio_counters.silent);

				// Silence for a port the game wrote within the last 100 ms is a gap inside its audio (the upstream
				// skip-then-silence burst), not an idle port: counted for the AudioOut line
				if (m_ps4_untouched_idle < untouched)
				{
					ps4_audio_count(s_ps4_audio_counters.fill_silent);
				}

				ps4_win.silent++;
				ps4_period_done(timestamp, ps4_period::silent);
#endif
				ringbuffer->enqueue_silence();
				untouched_expected = untouched;
				advance(timestamp);
				continue;
			}

			// Wait for buffer(s) to be completely filled
			if (in_progress > 0)
			{
				cellAudio.trace("waiting: in_progress=%u/%u, enqueued_buffers=%u", in_progress, active_ports, enqueued_buffers);
#ifdef RPCS3_PS4
				ps4_audio_count(s_ps4_audio_counters.in_progress_waits);

				if (!ps4_wait_start)
				{
					ps4_wait_start = timestamp;
				}

				// The fine poll keeps the PC's 500 us quiet time here: count_port_buffer_tags() treats a buffer whose
				// tags did not move since its previous call as finished, so polling faster would mix half-written
				// buffers more often.
				if (ps4_fine_wait(timestamp, timestamp + 500, false))
				{
					continue;
				}

				ps4_wake_target = timestamp + ps4_in_progress_poll_us;
				ps4_audio_wait(ps4_in_progress_poll_us);
#else
				thread_ctrl::wait_for(500);
#endif
				continue;
			}

			//cellAudio.error("active=%u, untouched=%u, in_progress=%d, incomplete=%d, enqueued_buffers=%u", active_ports, untouched, in_progress, incomplete, enqueued_buffers);

			// Store number of untouched buffers for future reference
			untouched_expected = untouched;

			// Log if we enqueued untouched/incomplete buffers
			if (untouched > 0 || incomplete > 0)
			{
				cellAudio.trace("enqueueing: untouched=%u/%u (expected=%u), incomplete=%u/%u enqueued_buffers=%llu", untouched, active_ports, untouched_expected, incomplete, active_ports, enqueued_buffers);
			}
		}

		// Mix
		float* buf = ringbuffer->get_current_buffer();

		switch (cfg.audio_channels)
		{
		case 2:
			switch (cfg.audio_downmix)
			{
			case AudioChannelCnt::SURROUND_7_1:
				mix<AudioChannelCnt::STEREO, AudioChannelCnt::SURROUND_7_1>(buf);
				break;
			case AudioChannelCnt::STEREO:
				mix<AudioChannelCnt::STEREO, AudioChannelCnt::STEREO>(buf);
				break;
			case AudioChannelCnt::SURROUND_5_1:
				mix<AudioChannelCnt::STEREO, AudioChannelCnt::SURROUND_5_1>(buf);
				break;
			}
			break;

		case 6:
			switch (cfg.audio_downmix)
			{
			case AudioChannelCnt::SURROUND_7_1:
				mix<AudioChannelCnt::SURROUND_5_1, AudioChannelCnt::SURROUND_7_1>(buf);
				break;
			case AudioChannelCnt::STEREO:
				mix<AudioChannelCnt::SURROUND_5_1, AudioChannelCnt::STEREO>(buf);
				break;
			case AudioChannelCnt::SURROUND_5_1:
				mix<AudioChannelCnt::SURROUND_5_1, AudioChannelCnt::SURROUND_5_1>(buf);
				break;
			}
			break;

		case 8:
			switch (cfg.audio_downmix)
			{
			case AudioChannelCnt::SURROUND_7_1:
				mix<AudioChannelCnt::SURROUND_7_1, AudioChannelCnt::SURROUND_7_1>(buf);
				break;
			case AudioChannelCnt::STEREO:
				mix<AudioChannelCnt::SURROUND_7_1, AudioChannelCnt::STEREO>(buf);
				break;
			case AudioChannelCnt::SURROUND_5_1:
				mix<AudioChannelCnt::SURROUND_7_1, AudioChannelCnt::SURROUND_5_1>(buf);
				break;
			}
			break;

		default:
			fmt::throw_exception("Unsupported channel count in cell_audio_config: %d", cfg.audio_channels);
		}

#ifdef RPCS3_PS4
		if (ps4_ring_us != umax)
		{
			ps4_audio_min(s_ps4_audio_counters.ring_min_us, ps4_ring_us);
		}
#endif

		// Enqueue
		ringbuffer->enqueue();

		// Advance time
#ifdef RPCS3_PS4
		ps4_period_done(timestamp, ps4_mix_kind);
#endif
		advance(timestamp);
	}

	// Destroy ringbuffer
	ringbuffer.reset();

#ifdef RPCS3_PS4
	s_ps4_audio_counters.rate_milli.store(umax, std::memory_order_relaxed);
#endif

	// Destroy the audio backend on this thread (the one that created it in cfg.reset()).
	// The backend's ctor calls CoInitializeEx here on Windows; releasing it on a different
	// thread (g_fxo->clear() runs on the GUI thread during Kill()) would land the matching
	// CoUninitialize on the GUI thread, draining its OLE reference and silently breaking the
	// main window's file drag&drop. Keep COM init/teardown balanced on this thread.
	cfg.backend.reset();
}

audio_port* cell_audio_thread::open_port()
{
	if (!free_port_count)
	{
		return nullptr;
	}

	const u32 index = --free_port_count;
	auto& port = ports[free_ports[index]];
	port.server_index = free_indices[index];
	port.state = audio_port_state::opened;
	m_front_only_reported[port.number] = false;
	m_periods_without_tag[port.number] = 0;
	m_front_only_port[port.number] = false;
	return &port;
}

error_code cell_audio_thread::allocate_port(ppu_thread& ppu, audio_port& port)
{
	const u32 size = std::max<u32>(0x10000, utils::align(port.size, 0x10000));
	auto* ct = g_ps3_process_info.sdk_ver > 0x21ffff ? &g_fxo->get<lv2_memory_container>() : nullptr;

	for (u64 key = SYS_MMAPPER_MIO_SHM_KEY + 2;; key++)
	{
		if (g_fxo->get<ipc_manager<lv2_memory, u64>>().get(key))
		{
			continue;
		}

		const auto result = lv2_obj::create<lv2_memory>(SYS_SYNC_PROCESS_SHARED, key, SYS_SYNC_NEWLY_CREATED, [&]()
		{
			return make_shared<lv2_memory>(size, 0x10000, 0x200, key, true, ct);
		});

		if (result + 0u == CELL_EEXIST)
		{
			continue;
		}

		if (result)
		{
			close_port(ppu, port);
			return CELL_AUDIO_ERROR_SHAREDMEMORY;
		}

		const u32 memory_id = idm::last_id<lv2_memory>();
		const vm::var<u32> address;
		auto memory = acquire_audio_memory(memory_id);

		if (!memory || sys_mmapper_search_and_map(ppu, shared_area, memory_id, 0x40000, address))
		{
			sys_mmapper_free_shared_memory(ppu, memory_id);
			release_audio_memory(memory);
			close_port(ppu, port);
			return CELL_AUDIO_ERROR_SHAREDMEMORY;
		}

		port_memories[port.number] = std::move(memory);
		port.addr = vm::cast(*address);
		port.mapped = true;
		port.index = vm::cast(shared_address + port.server_index * 16);
		std::memset(port.addr.get_ptr(), 0, size);
		break;
	}

	return CELL_OK;
}

void cell_audio_thread::close_port(ppu_thread& ppu, audio_port& port)
{
	port.state = audio_port_state::closed;
	const vm::var<u32> memory_id;

	if (port.mapped && !sys_mmapper_unmap_shared_memory(ppu, port.addr.addr(), memory_id))
	{
		// an extra mapping can keep the handle alive after the port closes
		sys_mmapper_free_shared_memory(ppu, *memory_id);
	}

	port.mapped = false;
	release_audio_memory(port_memories[port.number]);
	free_ports[free_port_count] = port.number;
	free_indices[free_port_count] = port.server_index;
	free_port_count++;
}

void cell_audio_thread::release_shared_memory(ppu_thread& ppu)
{
	if (--shared_refs || !shared_address)
	{
		return;
	}

	const vm::var<u32> memory_id;

	if (!sys_mmapper_unmap_shared_memory(ppu, shared_address, memory_id))
	{
		sys_mmapper_free_shared_memory(ppu, *memory_id);
	}
}

template <AudioChannelCnt channels, AudioChannelCnt downmix>
void cell_audio_thread::mix(float* out_buffer, s32 offset)
{
	std::lock_guard lock(mutex);

	AUDIT(out_buffer != nullptr);

	constexpr u32 out_channels = static_cast<u32>(channels);
	constexpr u32 out_buffer_sz = out_channels * AUDIO_BUFFER_SAMPLES;

	const float master_volume = audio::get_volume();

	// Reset out_buffer
	std::memset(out_buffer, 0, out_buffer_sz * sizeof(float));

	// mixing
	for (audio_port& port : ports)
	{
		if (port.state != audio_port_state::started) continue;

		auto buf = get_buffer(port, offset);

		static constexpr float minus_3db = 0.707f; // value taken from https://www.dolby.com/us/en/technologies/a-guide-to-dolby-metadata.pdf
		float m = master_volume;

		// part of cellAudioSetPortLevel functionality
		// spread port volume changes over 13ms
		auto step_volume = [master_volume, &m](audio_port& port)
		{
			const audio_port::level_set_t param = port.level_set.load();

			if (param.inc != 0.0f)
			{
				port.level += param.inc;
				const bool dec = param.inc < 0.0f;

				if ((!dec && param.value - port.level <= 0.0f) || (dec && param.value - port.level >= 0.0f))
				{
					port.level = param.value;
					port.level_set.compare_and_swap(param, { param.value, 0.0f });
				}
			}

			m = port.level * master_volume;
		};

		if (port.num_channels == 2)
		{
			for (u32 out = 0, in = 0; out < out_buffer_sz; out += out_channels, in += 2)
			{
				step_volume(port);

				const float left  = buf[in + 0] * m;
				const float right = buf[in + 1] * m;

				out_buffer[out + 0] += left;
				out_buffer[out + 1] += right;
			}
		}
		else if (port.num_channels == 8)
		{
			for (u32 out = 0, in = 0; out < out_buffer_sz; out += out_channels, in += 8)
			{
				step_volume(port);

				const float left       = buf[in + 0] * m;
				const float right      = buf[in + 1] * m;
				const float center     = buf[in + 2] * m;
				const float low_freq   = buf[in + 3] * m;
				const float side_left  = buf[in + 4] * m;
				const float side_right = buf[in + 5] * m;
				const float rear_left  = buf[in + 6] * m;
				const float rear_right = buf[in + 7] * m;

				if constexpr (downmix == AudioChannelCnt::STEREO)
				{
					// Don't mix in the lfe as per dolby specification and based on documentation
					const float mid = center * 0.5f;
					out_buffer[out + 0] += left * minus_3db + mid + side_left * 0.5f + rear_left * 0.5f;
					out_buffer[out + 1] += right * minus_3db + mid + side_right * 0.5f + rear_right * 0.5f;
				}
				else if constexpr (downmix == AudioChannelCnt::SURROUND_5_1)
				{
					out_buffer[out + 0] += left;
					out_buffer[out + 1] += right;

					if constexpr (out_channels >= 6) // Only mix the surround channels into the output if surround output is configured
					{
						out_buffer[out + 2] += center;
						out_buffer[out + 3] += low_freq;

						if constexpr (out_channels == 6)
						{
							out_buffer[out + 4] += side_left + rear_left;
							out_buffer[out + 5] += side_right + rear_right;
						}
						else // When using 7.1 ouput, out_buffer[out + 4] and out_buffer[out + 5] are the rear channels, so the side channels need to be mixed into [out + 6] and [out + 7]
						{
							out_buffer[out + 6] += side_left + rear_left;
							out_buffer[out + 7] += side_right + rear_right;
						}
					}
				}
				else
				{
					out_buffer[out + 0] += left;
					out_buffer[out + 1] += right;

					if constexpr (out_channels >= 6) // Only mix the surround channels into the output if surround output is configured
					{
						out_buffer[out + 2] += center;
						out_buffer[out + 3] += low_freq;

						if constexpr (out_channels == 6)
						{
							out_buffer[out + 4] += side_left;
							out_buffer[out + 5] += side_right;
						}
						else
						{
							out_buffer[out + 4] += rear_left;
							out_buffer[out + 5] += rear_right;
							out_buffer[out + 6] += side_left;
							out_buffer[out + 7] += side_right;
						}
					}
				}
			}
		}
		else if (!port.num_channels)
		{
			for (u32 out = 0, in = 0; out < out_buffer_sz; out += out_channels, in++)
			{
				step_volume(port);

				const float sample = buf[in] * m;
				out_buffer[out + 0] += sample;
				out_buffer[out + 1] += sample;
			}
		}
		else
		{
			fmt::throw_exception("Unknown channel count (port=%u, channel=%d)", port.number, port.num_channels);
		}
	}
}

void cell_audio_thread::finish_port_volume_stepping()
{
	std::lock_guard lock(mutex);

	// part of cellAudioSetPortLevel functionality
	for (audio_port& port : ports)
	{
		if (port.state != audio_port_state::started) continue;

		const audio_port::level_set_t param = port.level_set.load();
		port.level = param.value;
		port.level_set.compare_and_swap(param, { param.value, 0.0f });
	}
}

error_code cellAudioInit(ppu_thread& ppu)
{
	cellAudio.warning("cellAudioInit()");

	const std::unique_lock savestate_lock{ g_fxo->get<hle_locks_t>(), std::try_to_lock };

	if (!savestate_lock)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	auto& g_audio = g_fxo->get<cell_audio>();
	auto lock = lv2_host_lock(g_audio.mutex);

	if (g_audio.init)
	{
		return CELL_AUDIO_ERROR_ALREADY_INIT;
	}

	if (!g_audio.shared_refs++)
	{
		g_audio.shared_address = 0;
		const vm::var<u32> memory_id;
		const auto result = sys_mmapper_allocate_shared_memory(ppu, SYS_MMAPPER_MIO_SHM_KEY, 0x10000, 0x200, memory_id);

		if (result)
		{
			g_audio.release_shared_memory(ppu);

			// mio and audio both release this on an already open handle
			if (result + 0u == CELL_EEXIST)
			{
				g_audio.release_shared_memory(ppu);
				return CELL_AUDIO_ERROR_TRANS_EVENT;
			}

			return result;
		}

		const vm::var<u32> area;
		const vm::var<u32> address;
		auto memory = acquire_audio_memory(*memory_id);

		if (!memory)
		{
			g_audio.release_shared_memory(ppu);
			return CELL_ESRCH;
		}

		auto mapping_result = sys_mmapper_get_shared_memory_area(ppu, 0x20full, +area);

		if (!mapping_result)
		{
			mapping_result = sys_mmapper_search_and_map(ppu, *area, *memory_id, 0x40000, address);
		}

		if (mapping_result)
		{
			sys_mmapper_free_shared_memory(ppu, *memory_id);
			release_audio_memory(memory);
			g_audio.release_shared_memory(ppu);
			return mapping_result;
		}

		g_audio.shared_memory = std::move(memory);
		g_audio.shared_area = *area;
		g_audio.shared_address = *address;
	}
	else
	{
		g_audio.release_shared_memory(ppu);
		return CELL_AUDIO_ERROR_TRANS_EVENT;
	}

	const u32 port_count = g_ps3_process_info.sdk_ver <= 0x35ffff ? CELL_AUDIO_MAX_PORT : CELL_AUDIO_MAX_PORT_2;
	g_audio.free_port_count = port_count;

	for (u32 i = 0; i < AUDIO_PORT_COUNT; i++)
	{
		g_audio.ports[i].state = audio_port_state::closed;
		g_audio.ports[i].mapped = false;
		g_audio.ports[i].number = i;
		// closed ports keep their buffer fields and their slot in the new index mapping
		g_audio.ports[i].index = vm::cast(g_audio.shared_address + g_audio.ports[i].server_index * 16);

		if (i < port_count)
		{
			g_audio.free_ports[i] = port_count - i - 1;
			g_audio.free_indices[i] = port_count - i;
		}
	}

	g_audio.init = 1;
	return CELL_OK;
}

error_code cellAudioQuit(ppu_thread& ppu)
{
	cellAudio.warning("cellAudioQuit()");

	const std::unique_lock savestate_lock{ g_fxo->get<hle_locks_t>(), std::try_to_lock };

	if (!savestate_lock)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	auto& g_audio = g_fxo->get<cell_audio>();
	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	for (auto& port : g_audio.ports)
	{
		if (port.state != audio_port_state::closed)
		{
			g_audio.close_port(ppu, port);
		}
	}

	g_audio.release_shared_memory(ppu);
	release_audio_memory(g_audio.shared_memory);
	// NOTE: Do not clear event queues here. They are handled independently.
	g_audio.init = 0;
	return CELL_OK;
}

error_code cellAudioPortOpen(ppu_thread& ppu, vm::ptr<CellAudioPortParam> audioParam, vm::ptr<u32> portNum)
{
	cellAudio.warning("cellAudioPortOpen(audioParam=*0x%x, portNum=*0x%x)", audioParam, portNum);

	const std::unique_lock savestate_lock{ g_fxo->get<hle_locks_t>(), std::try_to_lock };

	if (!savestate_lock)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	auto& g_audio = g_fxo->get<cell_audio>();

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (!audioParam || !portNum)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	const u64 num_channels = audioParam->nChannel;
	const u64 num_blocks = audioParam->nBlock;
	const u64 attr = audioParam->attr;

	// check attributes
	if (num_channels != CELL_AUDIO_PORT_2CH &&
		num_channels != CELL_AUDIO_PORT_8CH &&
		num_channels != 0)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (num_blocks != 2 &&
		num_blocks != 4 &&
		num_blocks != CELL_AUDIO_BLOCK_8 &&
		num_blocks != CELL_AUDIO_BLOCK_16 &&
		num_blocks != CELL_AUDIO_BLOCK_32)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	// list unsupported flags
	if (attr & CELL_AUDIO_PORTATTR_BGM)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_BGM");
	}
	if (attr & CELL_AUDIO_PORTATTR_OUT_SECONDARY)
	{
#ifdef RPCS3_PS4
		// Mortal Kombat Komplete opens and closes such a port every ~11 ms in a probe loop (19351 lines in one session);
		// the first 3 per process say it all
		static atomic_t<u32> s_ps4_n = 0;
		if (s_ps4_n++ < 3)
#endif
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_SECONDARY");
	}
	if (attr & CELL_AUDIO_PORTATTR_OUT_PERSONAL_0)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_PERSONAL_0");
	}
	if (attr & CELL_AUDIO_PORTATTR_OUT_PERSONAL_1)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_PERSONAL_1");
	}
	if (attr & CELL_AUDIO_PORTATTR_OUT_PERSONAL_2)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_PERSONAL_2");
	}
	if (attr & CELL_AUDIO_PORTATTR_OUT_PERSONAL_3)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_PERSONAL_3");
	}
	if (attr & CELL_AUDIO_PORTATTR_OUT_NO_ROUTE)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_NO_ROUTE");
	}
	if (attr & 0xFFFFFFFFF0EFEFEEULL)
	{
		cellAudio.todo("cellAudioPortOpen(): unknown attributes (0x%llx)", attr);
	}

	// Waiting for VSH and doing some more things
	lv2_sleep(200);

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	// Open audio port
	audio_port* port = g_audio.open_port();

	if (!port)
	{
		return CELL_AUDIO_ERROR_PORT_FULL;
	}

	port->num_channels   = ::narrow<u32>(num_channels);
	port->num_blocks     = ::narrow<u32>(num_blocks);
	port->attr           = attr;
	port->size           = ::narrow<u32>(std::max<u64>(1, num_channels) * num_blocks * AUDIO_BUFFER_SAMPLES * sizeof(f32));
	port->cur_pos        = 0;
	port->global_counter = g_audio.m_counter;
	port->active_counter = 0;
	port->timestamp      = get_guest_system_time(g_audio.m_last_period_end);

	if (attr & CELL_AUDIO_PORTATTR_INITLEVEL)
	{
		port->level = audioParam->level;
	}
	else
	{
		port->level = 1.0f;
	}

	port->level_set.store({ port->level, 0.0f });

	if (auto result = g_audio.allocate_port(ppu, *port))
	{
		return result;
	}

	*portNum = port->number;
	return CELL_OK;
}

error_code cellAudioGetPortConfig(u32 portNum, vm::ptr<CellAudioPortConfig> portConfig)
{
	cellAudio.trace("cellAudioGetPortConfig(portNum=%d, portConfig=*0x%x)", portNum, portConfig);

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (!portConfig || portNum >= AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	audio_port& port = g_audio.ports[portNum];

	portConfig->readIndexAddr = port.index;

	switch (audio_port_state state = port.state.load())
	{
	case audio_port_state::closed:
		portConfig->status = CELL_AUDIO_STATUS_CLOSE;
		break;
	case audio_port_state::opened:
		portConfig->status = CELL_AUDIO_STATUS_READY;
		break;
	case audio_port_state::started:
		portConfig->status = CELL_AUDIO_STATUS_RUN;
		break;
	default:
		cellAudio.error("Invalid port state (%d: %d)", portNum, static_cast<u32>(state));
		return CELL_AUDIO_ERROR_AUDIOSYSTEM;
	}

	portConfig->nChannel = port.num_channels;
	portConfig->nBlock   = port.num_blocks;
	portConfig->portSize = port.size;
	portConfig->portAddr = port.addr.addr();
	return CELL_OK;
}

error_code cellAudioPortStart(u32 portNum)
{
	cellAudio.warning("cellAudioPortStart(portNum=%d)", portNum);

	auto& g_audio = g_fxo->get<cell_audio>();

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (portNum >= AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	// Waiting for VSH
	lv2_sleep(30);

#ifdef RPCS3_PS4
	// Destroyed after 'lock' below: the startup.log write happens outside the audio mutex.
	ps4_deferred_checkpoint ps4_port_line;
#endif

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	auto& port = g_audio.ports[portNum];

	switch (audio_port_state state = port.state)
	{
	case audio_port_state::closed: return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	case audio_port_state::started: return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;
	case audio_port_state::opened:
		std::memset(port.addr.get_ptr(), 0, port.size);
		port.state = audio_port_state::started;
#ifdef RPCS3_PS4
		{
			// 2 or 4 blocks mark a game that writes just in time; no notify queue marks one that polls the read
			// index instead of waiting for the mix event.
			std::string line = fmt::format("cellAudio: port %u started: %u channels, %u blocks, attr 0x%llx, notify queues %u",
				port.number, port.num_channels, port.num_blocks, port.attr, g_audio.keys.size());
			cellAudio.notice("%s", line);

			if (s_ps4_port_lines.fetch_add(1, std::memory_order_relaxed) < 8)
			{
				ps4_port_line.line = std::move(line);
			}
		}
#endif
		return CELL_OK;
	default: fmt::throw_exception("Invalid port state (%d: %d)", portNum, static_cast<u32>(state));
	}
}

error_code cellAudioPortClose(ppu_thread& ppu, u32 portNum)
{
	cellAudio.warning("cellAudioPortClose(portNum=%d)", portNum);

	const std::unique_lock savestate_lock{ g_fxo->get<hle_locks_t>(), std::try_to_lock };

	if (!savestate_lock)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (portNum >= AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	auto& port = g_audio.ports[portNum];

	if (port.state == audio_port_state::closed)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	}

	g_audio.close_port(ppu, port);
	return CELL_OK;
}

error_code cellAudioPortStop(u32 portNum)
{
	cellAudio.warning("cellAudioPortStop(portNum=%d)", portNum);

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (portNum >= AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	switch (audio_port_state state = g_audio.ports[portNum].state.compare_and_swap(audio_port_state::started, audio_port_state::opened))
	{
	case audio_port_state::closed: return CELL_AUDIO_ERROR_PORT_NOT_RUN;
	case audio_port_state::started: return CELL_OK;
	case audio_port_state::opened: return CELL_AUDIO_ERROR_PORT_NOT_RUN;
	default: fmt::throw_exception("Invalid port state (%d: %d)", portNum, static_cast<u32>(state));
	}
}

error_code cellAudioGetPortTimestamp(u32 portNum, u64 tag, vm::ptr<u64> stamp)
{
	cellAudio.trace("cellAudioGetPortTimestamp(portNum=%d, tag=0x%llx, stamp=*0x%x)", portNum, tag, stamp);

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (portNum >= AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	audio_port& port = g_audio.ports[portNum];

	if (port.state == audio_port_state::closed)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	}

	if (port.global_counter < tag)
	{
		return CELL_AUDIO_ERROR_TAG_NOT_FOUND;
	}

	const u64 delta_tag = port.global_counter - tag;
	const u64 delta_tag_stamp = delta_tag * g_audio.cfg.audio_block_period;

	// Apparently no error is returned if stamp is null
	*stamp = get_guest_system_time(port.timestamp - delta_tag_stamp);

	return CELL_OK;
}

error_code cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, vm::ptr<u64> tag)
{
	cellAudio.trace("cellAudioGetPortBlockTag(portNum=%d, blockNo=0x%llx, tag=*0x%x)", portNum, blockNo, tag);

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (portNum >= AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	audio_port& port = g_audio.ports[portNum];

	if (port.state == audio_port_state::closed)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	}

	if (blockNo >= port.num_blocks)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	// Apparently no error is returned if tag is null
	*tag = port.global_counter + blockNo - port.cur_pos;

	return CELL_OK;
}

error_code cellAudioSetPortLevel(u32 portNum, float level)
{
	cellAudio.trace("cellAudioSetPortLevel(portNum=%d, level=%f)", portNum, level);

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (portNum >= AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	audio_port& port = g_audio.ports[portNum];

	if (port.state == audio_port_state::closed)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	}

	if (level >= 0.0f)
	{
		port.level_set.exchange({ level, (port.level - level) / 624.0f });
	}
	else
	{
		cellAudio.todo("cellAudioSetPortLevel(%d): negative level value (%f)", portNum, level);
	}

	return CELL_OK;
}

static error_code AudioCreateNotifyEventQueue(ppu_thread& ppu, vm::ptr<u32> id, vm::ptr<u64> key, u32 queue_type)
{
	vm::var<sys_event_queue_attribute_t> attr;
	attr->protocol = SYS_SYNC_FIFO;
	attr->type     = queue_type;
	attr->name_u64 = 0;

	for (u64 i = 0; i < MAX_AUDIO_EVENT_QUEUES; i++)
	{
		// Create an event queue "bruteforcing" an available key
		const u64 key_value = 0x80004d494f323221ull + i;

		// This originally reads from a global sdk value set by cellAudioInit
		// So check initialization as well
		const u32 queue_depth = g_fxo->get<cell_audio>().init && g_ps3_process_info.sdk_ver <= 0x35FFFF ? 2 : 8;

		if (CellError res{sys_event_queue_create(ppu, id, attr, key_value, queue_depth) + 0u})
		{
			if (res != CELL_EEXIST)
			{
				return res;
			}
		}
		else
		{
			*key = key_value;
			return CELL_OK;
		}
	}

	return CELL_AUDIO_ERROR_EVENT_QUEUE;
}

error_code cellAudioCreateNotifyEventQueue(ppu_thread& ppu, vm::ptr<u32> id, vm::ptr<u64> key)
{
	cellAudio.warning("cellAudioCreateNotifyEventQueue(id=*0x%x, key=*0x%x)", id, key);

	return AudioCreateNotifyEventQueue(ppu, id, key, SYS_PPU_QUEUE);
}

error_code cellAudioCreateNotifyEventQueueEx(ppu_thread& ppu, vm::ptr<u32> id, vm::ptr<u64> key, u32 iFlags)
{
	cellAudio.warning("cellAudioCreateNotifyEventQueueEx(id=*0x%x, key=*0x%x, iFlags=0x%x)", id, key, iFlags);

	if (iFlags & ~CELL_AUDIO_CREATEEVENTFLAG_SPU)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	const u32 queue_type = (iFlags & CELL_AUDIO_CREATEEVENTFLAG_SPU) ? SYS_SPU_QUEUE : SYS_PPU_QUEUE;
	return AudioCreateNotifyEventQueue(ppu, id, key, queue_type);
}

error_code AudioSetNotifyEventQueue(ppu_thread& ppu, u64 key, u32 iFlags)
{
	auto& g_audio = g_fxo->get<cell_audio>();

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	// Waiting for VSH
	lv2_sleep(20, &ppu);

	// Dirty hack for sound: confirm the creation of _mxr000 event queue by _cellsurMixerMain thread
	constexpr u64 c_mxr000 = 0x8000cafe02460300;

	if (key == c_mxr000 || key == 0)
	{
		const bool has_sur_mixer_thread = idm::select<named_thread<ppu_thread>>([&](u32 id, named_thread<ppu_thread>& test_ppu)
		{
			// Confirm thread existence
			if (id == ppu.id)
			{
				return false;
			}

			const auto ptr = test_ppu.ppu_tname.load();

			if (!ptr)
			{
				return false;
			}

			return *ptr == "_cellsurMixerMain"sv;
		}).ret;

		bool was_mxr000_queue_found = false;

		for (usz count = 0; has_sur_mixer_thread && count < 100; count++)
		{
			if (lv2_event_queue::find(c_mxr000))
			{
				was_mxr000_queue_found = true;
				break;
			}

			if (ppu.is_stopped())
			{
				ppu.state += cpu_flag::again;
				return {};
			}

			(count < 3 ? cellAudio.warning : cellAudio.error)("AudioSetNotifyEventQueue(): Waiting for _mxr000. x%d", count);

			lv2_sleep(50'000, &ppu);
		}

		if (key == 0 && was_mxr000_queue_found)
		{
			// Correct key value argument
			key = c_mxr000;
		}
	}

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	auto q = lv2_event_queue::find(key);

	if (!q)
	{
		return CELL_AUDIO_ERROR_TRANS_EVENT;
	}

	for (auto i = g_audio.keys.cbegin(); i != g_audio.keys.cend();) // check for duplicates
	{
		if (i->port == q)
		{
			return CELL_AUDIO_ERROR_TRANS_EVENT;
		}

		if (!lv2_obj::check(i->port))
		{
			// Cleanup, avoid cases where there are multiple ports with the same key
			i = g_audio.keys.erase(i);
		}
		else
		{
			i++;
		}
	}

	// Set unique source associated with the key
	g_audio.keys.push_back
	({
		.start_period = g_audio.event_period,
		.flags = iFlags,
		.source = ((process_getpid() + u64{}) << 32) + lv2_event_port::id_base + (g_audio.key_count++ * lv2_event_port::id_step),
		.ack_timestamp = 0,
		.port = std::move(q)
	});

	g_audio.key_count %= lv2_event_port::id_count;

	return CELL_OK;
}

error_code cellAudioSetNotifyEventQueue(ppu_thread& ppu, u64 key)
{
	ppu.state += cpu_flag::wait;

	cellAudio.warning("cellAudioSetNotifyEventQueue(key=0x%llx)", key);

	return AudioSetNotifyEventQueue(ppu, key, 0);
}

error_code cellAudioSetNotifyEventQueueEx(ppu_thread& ppu, u64 key, u32 iFlags)
{
	ppu.state += cpu_flag::wait;

	cellAudio.todo("cellAudioSetNotifyEventQueueEx(key=0x%llx, iFlags=0x%x)", key, iFlags);

	if (iFlags & (~0u >> 5))
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	return AudioSetNotifyEventQueue(ppu, key, iFlags);
}

error_code AudioRemoveNotifyEventQueue(u64 key, u32 iFlags)
{
	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	for (auto i = g_audio.keys.cbegin(); i != g_audio.keys.cend(); i++)
	{
		if (lv2_obj::check(i->port) && i->port->key == key)
		{
			if (i->flags != iFlags)
			{
				break;
			}

			g_audio.keys.erase(i);

			return CELL_OK;
		}
	}

	return CELL_AUDIO_ERROR_TRANS_EVENT;
}

error_code cellAudioRemoveNotifyEventQueue(u64 key)
{
	cellAudio.warning("cellAudioRemoveNotifyEventQueue(key=0x%llx)", key);

	return AudioRemoveNotifyEventQueue(key, 0);
}

error_code cellAudioRemoveNotifyEventQueueEx(u64 key, u32 iFlags)
{
	cellAudio.todo("cellAudioRemoveNotifyEventQueueEx(key=0x%llx, iFlags=0x%x)", key, iFlags);

	if (iFlags & (~0u >> 5))
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	return AudioRemoveNotifyEventQueue(key, iFlags);
}

error_code cellAudioAddData(u32 portNum, vm::ptr<float> src, u32 samples, float volume)
{
	cellAudio.trace("cellAudioAddData(portNum=%d, src=*0x%x, samples=%d, volume=%f)", portNum, src, samples, volume);

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (portNum >= AUDIO_PORT_COUNT || !src || !src.aligned() || samples != CELL_AUDIO_BLOCK_SAMPLES)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	const audio_port& port = g_audio.ports[portNum];

	if (!port.num_channels)
	{
		return CELL_OK;
	}

	const auto dst = port.get_vm_ptr();

	volume = std::isfinite(volume) ? std::clamp(volume, -16.f, 16.f) : 0.f;

	for (u32 i = 0; i < samples * port.num_channels; i++)
	{
		dst[i] += src[i] * volume; // mix all channels
	}

	return CELL_OK;
}

error_code cellAudioAdd2chData(u32 portNum, vm::ptr<float> src, u32 samples, float volume)
{
	cellAudio.trace("cellAudioAdd2chData(portNum=%d, src=*0x%x, samples=%d, volume=%f)", portNum, src, samples, volume);

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (portNum >= AUDIO_PORT_COUNT || !src || !src.aligned() || samples != CELL_AUDIO_BLOCK_SAMPLES)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	const audio_port& port = g_audio.ports[portNum];

	const auto dst = port.get_vm_ptr();

	volume = std::isfinite(volume) ? std::clamp(volume, -16.f, 16.f) : 0.f;

	if (port.num_channels == 2)
	{
		for (u32 i = 0; i < samples; i++)
		{
			dst[i * 2 + 0] += src[i * 2 + 0] * volume; // mix L ch
			dst[i * 2 + 1] += src[i * 2 + 1] * volume; // mix R ch
		}
	}
	else if (port.num_channels == 8)
	{
		for (u32 i = 0; i < samples; i++)
		{
			dst[i * 8 + 0] += src[i * 2 + 0] * volume; // mix L ch
			dst[i * 8 + 1] += src[i * 2 + 1] * volume; // mix R ch
			//dst[i * 8 + 2] += 0.0f; // center
			//dst[i * 8 + 3] += 0.0f; // LFE
			//dst[i * 8 + 4] += 0.0f; // rear L
			//dst[i * 8 + 5] += 0.0f; // rear R
			//dst[i * 8 + 6] += 0.0f; // side L
			//dst[i * 8 + 7] += 0.0f; // side R
		}
	}
	else
	{
		cellAudio.error("cellAudioAdd2chData(portNum=%d): invalid port.channel value (%d)", portNum, port.num_channels);
	}

	return CELL_OK;
}

error_code cellAudioAdd6chData(u32 portNum, vm::ptr<float> src, float volume)
{
	cellAudio.trace("cellAudioAdd6chData(portNum=%d, src=*0x%x, volume=%f)", portNum, src, volume);

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (portNum >= AUDIO_PORT_COUNT || !src || !src.aligned())
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	const audio_port& port = g_audio.ports[portNum];

	const auto dst = port.get_vm_ptr();

	volume = std::isfinite(volume) ? std::clamp(volume, -16.f, 16.f) : 0.f;

	if (port.num_channels == 8)
	{
		for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES; i++)
		{
			// Channel order in src is Front Left, Center, Front Right, Surround Left, Surround Right, LFE
			dst[i * 8 + 0] += src[i * 6 + 0] * volume; // mix L ch
			dst[i * 8 + 1] += src[i * 6 + 2] * volume; // mix R ch
			dst[i * 8 + 2] += src[i * 6 + 1] * volume; // mix center
			dst[i * 8 + 3] += src[i * 6 + 5] * volume; // mix LFE
			dst[i * 8 + 4] += src[i * 6 + 3] * volume; // mix rear L
			dst[i * 8 + 5] += src[i * 6 + 4] * volume; // mix rear R
			//dst[i * 8 + 6] += 0.0f; // side L
			//dst[i * 8 + 7] += 0.0f; // side R
		}
	}
	else
	{
		cellAudio.error("cellAudioAdd6chData(portNum=%d): invalid port.channel value (%d)", portNum, port.num_channels);
	}

	return CELL_OK;
}

error_code cellAudioMiscSetAccessoryVolume(u32 devNum, float volume)
{
	cellAudio.todo("cellAudioMiscSetAccessoryVolume(devNum=%d, volume=%f)", devNum, volume);

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	// TODO

	return CELL_OK;
}

error_code cellAudioSendAck(u64 data3)
{
	cellAudio.trace("cellAudioSendAck(data3=0x%llx)", data3);

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	// TODO: error checks

	for (cell_audio_thread::key_info& k : g_audio.keys)
	{
		if (k.source == data3)
		{
			k.ack_timestamp = get_system_time();
			break;
		}
	}

	return CELL_OK;
}

error_code cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
	cellAudio.todo("cellAudioSetPersonalDevice(iPersonalStream=%d, iDevice=%d)", iPersonalStream, iDevice);

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (iPersonalStream < 0 || iPersonalStream >= 4 ||
		(iDevice != CELL_AUDIO_PERSONAL_DEVICE_PRIMARY && (iDevice < 0 || iDevice >= 5)))
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	// TODO

	return CELL_OK;
}

error_code cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
	cellAudio.todo("cellAudioUnsetPersonalDevice(iPersonalStream=%d)", iPersonalStream);

	auto& g_audio = g_fxo->get<cell_audio>();

	auto lock = lv2_host_lock(g_audio.mutex);

	if (!g_audio.init)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (iPersonalStream < 0 || iPersonalStream >= 4)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	// TODO

	return CELL_OK;
}

DECLARE(ppu_module_manager::cellAudio)("cellAudio", []()
{
	REG_FUNC(cellAudio, cellAudioInit);
	REG_FUNC(cellAudio, cellAudioPortClose);
	REG_FUNC(cellAudio, cellAudioPortStop);
	REG_FUNC(cellAudio, cellAudioGetPortConfig);
	REG_FUNC(cellAudio, cellAudioPortStart);
	REG_FUNC(cellAudio, cellAudioQuit);
	REG_FUNC(cellAudio, cellAudioPortOpen);
	REG_FUNC(cellAudio, cellAudioSetPortLevel);
	REG_FUNC(cellAudio, cellAudioCreateNotifyEventQueue);
	REG_FUNC(cellAudio, cellAudioCreateNotifyEventQueueEx);
	REG_FUNC(cellAudio, cellAudioMiscSetAccessoryVolume);
	REG_FUNC(cellAudio, cellAudioSetNotifyEventQueue);
	REG_FUNC(cellAudio, cellAudioSetNotifyEventQueueEx);
	REG_FUNC(cellAudio, cellAudioGetPortTimestamp);
	REG_FUNC(cellAudio, cellAudioAdd2chData);
	REG_FUNC(cellAudio, cellAudioAdd6chData);
	REG_FUNC(cellAudio, cellAudioAddData);
	REG_FUNC(cellAudio, cellAudioGetPortBlockTag);
	REG_FUNC(cellAudio, cellAudioRemoveNotifyEventQueue);
	REG_FUNC(cellAudio, cellAudioRemoveNotifyEventQueueEx);
	REG_FUNC(cellAudio, cellAudioSendAck);
	REG_FUNC(cellAudio, cellAudioSetPersonalDevice);
	REG_FUNC(cellAudio, cellAudioUnsetPersonalDevice);
});
