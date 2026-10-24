#include "stdafx.h"
#include "Emu/IdManager.h"
#include "Emu/perf_meter.hpp"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/lv2/sys_ppu_thread.h"
#include "Emu/Cell/lv2/sys_process.h"
#include "Emu/savestate_utils.hpp"
#include "sysPrxForUser.h"
#include "util/media_utils.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
#include "libswscale/swscale.h"
}
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#include "cellPamf.h"
#include "libavcdec.h"
#include "libdivx311dec.h"
#include "libdivxdec.h"
#include "libmvcdec.h"
#include "libsjvtd.h"
#include "libsmvd2.h"
#include "libsmvd4.h"
#include "libsvc1d.h"
#include "cellVdec.h"

#include <mutex>
#include <queue>
#include <cmath>
#include "Utilities/lockless.h"
#include <variant>
#include "util/asm.hpp"

#ifdef RPCS3_PS4
#include "util/ps4_hle_memory.hpp"
#include "util/ps4_memory_governor.hpp"
#include "util/ps4_yuv_convert.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <new>
#endif

std::mutex g_mutex_avcodec_open2;

constexpr int averror_eof = AVERROR_EOF; // workaround for old-style-cast error

LOG_CHANNEL(cellVdec);

#ifdef RPCS3_PS4
// Frontend object (ps4_launcher.cpp), always linked: one startup.log line through a buffered stream (no fsync).
void ps4_startup_checkpoint(const char* stage);

// 0.33: opt-in H.264 slice threading (thread_count 2 = this thread plus one FFmpeg worker). Frame threading is never
// used: it returns each picture thread_count - 1 AUs late, which breaks the per-AU AUDONE/PICOUT order games see, and
// skip_frame changes per AU while worker threads still decode earlier AUs.
static constexpr char ps4_vdec_slice_flag[] = "/data/rpcs3/vdec-slice-threads.flag";

// Turns off the adaptive H.264 deblocking skip and the slice-edge shortcut (AV_CODEC_FLAG2_FAST).
static constexpr char ps4_vdec_full_quality_flag[] = "/data/rpcs3/vdec-full-quality.flag";

// GetPicture RGB output through swscale again instead of utils::ps4::yuv (read once per process).
static constexpr char ps4_vdec_sws_flag[] = "/data/rpcs3/vdec-sws-convert.flag";

// Opt-in: GetPicture leaves the lv2 PPU run queue while it converts a large RGB picture (read once per process).
static constexpr char ps4_vdec_release_slot_flag[] = "/data/rpcs3/vdec-release-slot.flag";

// 'VDEC perf' lines written by this process. A game that loops a video forever must not grow startup.log without
// bound: the first 360 lines (an hour of playback) use 10 s windows, later ones 60 s.
static atomic_t<u32> s_ps4_vdec_perf_lines{0};
static atomic_t<bool> s_ps4_vdec_slice_noted{false};
static atomic_t<bool> s_ps4_vdec_mpeg2_slice_noted{false};
static atomic_t<bool> s_ps4_vdec_slice_denied{false};
static atomic_t<bool> s_ps4_vdec_full_quality_noted{false};
static atomic_t<bool> s_ps4_vdec_deblock_noted{false};
static atomic_t<bool> s_ps4_vdec_oom_noted{false};
static atomic_t<bool> s_ps4_vdec_drop_noted{false};
static atomic_t<bool> s_ps4_vdec_fast_noted{false};
static atomic_t<bool> s_ps4_vdec_sws_noted{false};
static atomic_t<bool> s_ps4_vdec_release_noted{false};
static atomic_t<bool> s_ps4_vdec_open_failed_noted{false};
static atomic_t<u32> s_ps4_vdec_queue_lines{0}; // 'VDEC: picture queue' lines written by this process (at most 32)

// Failed decoder opens of this process, and those in a row since the last open that worked. A player such as libsail
// reopens a refused decoder within a fraction of a millisecond, each try building and freeing a decoder context, so
// while opens keep failing the lines of the open path go through governor::media_log_due (the first 8, then one in
// 100); the game sees the same calls and results.
static std::atomic<u32> s_ps4_vdec_open_failures{0};
static std::atomic<u32> s_ps4_vdec_failed_in_row{0};

static bool ps4_vdec_open_lines_due() noexcept
{
	const u32 n = s_ps4_vdec_failed_in_row.load(std::memory_order_relaxed);
	return !n || utils::ps4::governor::media_log_due(n);
}

// 'VDEC: consumer stopped' lines written by this process (one per decoder, at most 16)
static atomic_t<u32> s_ps4_vdec_consumer_lines{0};

static const char* ps4_vdec_deblock_name(u32 level)
{
	return level >= 2 ? "all" : level == 1 ? "nonref" : "default";
}

static const char* ps4_vdec_pix_fmt_name(int format)
{
	const char* name = format >= 0 ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(format)) : nullptr;
	return name ? name : "none";
}

// Host wall clock in microseconds; independent of the guest time base.
static u64 ps4_vdec_now_us()
{
	return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Coded slices (NAL types 1 and 5) in one H.264 Annex-B access unit. Slice threads only help a stream with several
// slices per picture. A byte > 1 cannot end a 00 00 01 start code at its own or the next two positions.
static u32 ps4_vdec_h264_slices(const u8* data, u32 size)
{
	u32 slices = 0;

	for (u32 i = 2; i + 1 < size;)
	{
		if (data[i] > 1)
		{
			i += 3;
		}
		else if (data[i] == 1 && data[i - 1] == 0 && data[i - 2] == 0)
		{
			const u32 nal_type = data[i + 1] & 0x1f;
			slices += nal_type == 1 || nal_type == 5;
			i += 3;
		}
		else
		{
			i++;
		}
	}

	return slices;
}
#endif

template<>
void fmt_class_string<CellVdecError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(CELL_VDEC_ERROR_ARG);
			STR_CASE(CELL_VDEC_ERROR_SEQ);
			STR_CASE(CELL_VDEC_ERROR_BUSY);
			STR_CASE(CELL_VDEC_ERROR_EMPTY);
			STR_CASE(CELL_VDEC_ERROR_AU);
			STR_CASE(CELL_VDEC_ERROR_PIC);
			STR_CASE(CELL_VDEC_ERROR_UNK);
			STR_CASE(CELL_VDEC_ERROR_FATAL);
		}

		return unknown;
	});
}

// The general sequence control flow has these possible transitions:
// closed -> dormant
// dormant -> ready
// dormant -> closed
// ready -> ending
// ready -> resetting
// ready -> closed
// ending -> dormant
// resetting -> ready
enum class sequence_state : u32
{
	closed    = 0, // Also called non-existent. Needs to be opened before anything can be done with it.
	dormant   = 1, // Waiting for the next sequence. The last picture and pic-item can be aqcuired in this state.
	ready     = 2, // Ready for decoding. Can also restart sequences in this state.
	ending    = 3, // Ending a sequence. Goes to dormant afterwards.
	resetting = 4, // Stops the current sequence and starts a new one. The pictures of the old sequence are flushed
	invalid   = 5, // Any other value is invalid
};

template<>
void fmt_class_string<sequence_state>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(sequence_state::closed);
			STR_CASE(sequence_state::dormant);
			STR_CASE(sequence_state::ready);
			STR_CASE(sequence_state::ending);
			STR_CASE(sequence_state::resetting);
			STR_CASE(sequence_state::invalid);
		}

		return unknown;
	});
}

vm::gvar<s32> _cell_vdec_prx_ver; // TODO: this should probably specify the VDEC module that was loaded. E.g. CELL_SYSMODULE_VDEC_MPEG2

enum class vdec_cmd_type : u32
{
	start_sequence,
	end_sequence,
	close,
	au_decode,
	framerate,
	drain, // Outputs the pictures still held by the decoder before the sequence ends
};

struct vdec_cmd
{
	explicit vdec_cmd(vdec_cmd_type _type, u64 _seq_id, u64 _id)
		: type(_type), seq_id(_seq_id), id(_id)
	{
		ensure(_type != vdec_cmd_type::au_decode);
		ensure(_type != vdec_cmd_type::framerate);
	}

	explicit vdec_cmd(vdec_cmd_type _type, u64 _seq_id, u64 _id, s32 _mode, CellVdecAuInfo _au)
		: type(_type), seq_id(_seq_id), id(_id), mode(_mode), au(std::move(_au))
	{
		ensure(_type == vdec_cmd_type::au_decode);
	}

	explicit vdec_cmd(vdec_cmd_type _type, u64 _seq_id, u64 _id, s32 _framerate)
		: type(_type), seq_id(_seq_id), id(_id), framerate(_framerate)
	{
		ensure(_type == vdec_cmd_type::framerate);
	}

	vdec_cmd_type type{};
	u64 seq_id{};
	u64 id{};
	s32 mode{};
	s32 framerate{};
	CellVdecAuInfo au{};
};

struct vdec_frame
{
	struct frame_dtor
	{
		void operator()(AVFrame* data) const
		{
			av_frame_unref(data);
			av_frame_free(&data);
		}
	};

	u64 seq_id{};
	u64 cmd_id{};

	std::unique_ptr<AVFrame, frame_dtor> avf;
	u64 dts{};
	u64 pts{};
	u64 userdata{};
	u32 frc{};
	bool pic_item_received = false;
	CellVdecPicAttr attr = CELL_VDEC_PICITEM_ATTR_NORMAL;

	AVFrame* operator ->() const
	{
		return avf.get();
	}
};

struct vdec_context final
{
	static const u32 id_base = 0xf0000000;
	static const u32 id_step = 0x00000100;
	static const u32 id_count = 1024;
	SAVESTATE_INIT_POS(24);

	u32 handle = 0;

	atomic_t<u64> seq_id = 0; // The first sequence will have the ID 1
	atomic_t<u64> next_cmd_id = 0;
	atomic_t<bool> abort_decode = false; // Used for thread interaction
	atomic_t<bool> is_running = false;   // Used for thread interaction
	atomic_t<sequence_state> seq_state = sequence_state::closed;

	const AVCodec* codec{};
	const AVCodecDescriptor* codec_desc{};
	AVCodecContext* ctx{};
	SwsContext* sws{};

	shared_mutex mutex; // Used for 'out' queue (TODO)

	const u32 type;
	const u32 mem_addr;
	const u32 mem_size;
	const vm::ptr<CellVdecCbMsg> cb_func;
	const u32 cb_arg;
	u32 mem_bias{};

	u32 frc_set{}; // Frame Rate Override
	u64 next_pts{};
	u64 next_dts{};
	atomic_t<u32> ppu_tid{};

	std::deque<vdec_frame> out_queue;
	const u32 out_max = 60;

	atomic_t<s32> au_count{0};

	lf_queue<vdec_cmd> in_cmd;

	AVRational log_time_base{}; // Used to reduce log spam
	AVRational log_framerate{}; // Used to reduce log spam

#ifdef RPCS3_PS4
	// 0.33: decode cost of the current 'VDEC perf' window. Written by the decoder thread (exec), and by
	// release_decoder once that thread is gone.
	u64 ps4_perf_start = 0; // host us when the window's first decode began; 0 = empty window
	u64 ps4_perf_last = 0;  // host us when the window's last decode ended
	u64 ps4_perf_total_us = 0;
	u64 ps4_perf_max_us = 0;
	u32 ps4_perf_aus = 0;
	u32 ps4_perf_drains = 0;
	u32 ps4_perf_pics = 0;
	u32 ps4_perf_skip_aus = 0; // AUs decoded in a B-skip or I-only mode
	u32 ps4_perf_deblock_aus = 0; // AUs decoded with the loop filter skipped (adaptive deblocking)
	u32 ps4_perf_oom_retries = 0;
	u32 ps4_perf_oom_drops = 0;
	bool ps4_slice_threads = false;

	// 0.34: adaptive H.264 deblocking. FFmpeg reads skip_loop_filter per slice, so the level can change between AUs
	// without touching picture order or the AUDONE/PICOUT sequence. Decoder thread only.
	bool ps4_deblock_adaptive = false; // H.264 without vdec-full-quality.flag
	u32 ps4_deblock_level = 0;         // 0 = default, 1 = skipped on non-reference pictures, 2 = skipped on all
	s64 ps4_decode_ema_us = -1;        // EMA (1/8) of the decode time per AU; -1 = no sample yet
	u32 ps4_deblock_above = 0;         // consecutive AUs with the EMA above 0.9x the frame interval
	u32 ps4_deblock_below = 0;         // consecutive AUs with the EMA below 0.6x the frame interval

	// YUV conversions done by cellVdecGetPicture(Ext) on the game's PPU thread (swscale, C only without x86asm).
	std::atomic<u64> ps4_conv_count{0};
	std::atomic<u64> ps4_conv_total_us{0};
	std::atomic<u64> ps4_conv_max_us{0};
	std::atomic<int> ps4_conv_out_format{-1}; // AVPixelFormat of the last conversion's output
	std::atomic<u64> ps4_conv_fast{0};        // conversions done by utils::ps4::yuv instead of swscale

	// Pipeline state of the window. starved: the decoder thread waited for an AU while a sequence was running (the
	// game or demuxer feeds late); blocked: it waited for space in the picture queue (the game takes pictures late).
	// Decoder thread only, except GetPicture's empty count.
	u64 ps4_perf_starved_us = 0;
	u64 ps4_perf_blocked_us = 0;
	u32 ps4_perf_queue_max = 0;
	u32 ps4_perf_slice_aus = 0; // H.264 AUs scanned for slices (one in four)
	u32 ps4_perf_slices = 0;
	std::atomic<u32> ps4_getpic_empty{0}; // cellVdecGetPicture(Ext) found no picture (game threads)

	// 0.40: who takes the pictures, for the line written when the decoder first waits 5 s for a consumer (a movie that
	// stops while the decoder is healthy): pictures the game took and when, and the PPU thread that took the last one.
	std::atomic<u32> ps4_pics_taken{0};
	std::atomic<u64> ps4_getpic_last_us{0};
	std::atomic<u32> ps4_getpic_last_tid{0};
	u32 ps4_pics_queued = 0;         // decoder thread only: pictures put in the queue
	bool ps4_consumer_noted = false; // decoder thread only

	// 0.38: picture queue bound (utils::ps4::hle_memory::vdec_queue_limit): the decoder thread waits while the queue
	// holds this many pictures. Written by the decoder thread, read by cellVdecGetPicture(Ext) on the game's threads.
	std::atomic<u32> ps4_queue_limit{utils::ps4::hle_memory::vdec_queue_max};
	u64 ps4_picture_bytes = 0; // decoder thread only: bytes of the last picture sized

	// 0.38: the FFmpeg context could not be allocated or opened (out of memory, no thread resources): vdecOpen
	// returns CELL_VDEC_ERROR_FATAL instead of ending the emulation; games treat a failed open as "skip the movie".
	const char* ps4_open_error = nullptr;

	// Before avcodec_open2: H.264 and MPEG-2 get one slice worker when the flag exists. Slice threads add no output
	// delay but only help streams that code several slices per picture (PS3 MPEG-2 movies code one per macroblock row).
	void ps4_configure_threads()
	{
		const bool avc = type == CELL_VDEC_CODEC_TYPE_AVC;

		if ((!avc && type != CELL_VDEC_CODEC_TYPE_MPEG2) || !fs::is_file(ps4_vdec_slice_flag))
		{
			return;
		}

		// The worker is one more host thread. Near the console's thread ceiling (about 81 per process) it is not
		// started, so it cannot take the slot a guest thread or another decoder needs.
		if (const int pressure = utils::ps4::governor::thread_pressure(); pressure >= 1)
		{
			if (!s_ps4_vdec_slice_denied.exchange(true))
			{
				const std::string line = fmt::format("VDEC: slice threads not used: %u live threads (thread pressure %d)", utils::ps4::governor::live_threads(), pressure);
				cellVdec.warning("%s", line);
				ps4_startup_checkpoint(line.c_str());
			}

			return;
		}

		ctx->thread_count = 2;
		ctx->thread_type = FF_THREAD_SLICE;
		ps4_slice_threads = true;

		// MPEG-2 slices are independent: the split is bit-exact and needs no decoder shortcut
		if (!avc)
		{
			if (!s_ps4_vdec_mpeg2_slice_noted.exchange(true))
			{
				ps4_startup_checkpoint("VDEC: mpeg2video slice threading requested by vdec-slice-threads.flag (2 threads)");
			}

			return;
		}

		// Without FLAG2_FAST, FFmpeg postpones deblocking to one serial pass after the slices (h264_slice.c), which
		// loses most of the slice-thread gain; with it, only the filtering across slice edges is skipped.
		if (!fs::is_file(ps4_vdec_full_quality_flag))
		{
			ctx->flags2 |= AV_CODEC_FLAG2_FAST;
		}

		if (!s_ps4_vdec_slice_noted.exchange(true))
		{
			ps4_startup_checkpoint("VDEC: H.264 slice threading requested by vdec-slice-threads.flag (2 threads, one extra FFmpeg worker per decoder)");
		}
	}

	// After a successful avcodec_open2.
	void ps4_configure_deblocking()
	{
		if (type != CELL_VDEC_CODEC_TYPE_AVC)
		{
			return;
		}

		ps4_deblock_adaptive = !fs::is_file(ps4_vdec_full_quality_flag);

		if (!ps4_deblock_adaptive && !s_ps4_vdec_full_quality_noted.exchange(true))
		{
			ps4_startup_checkpoint("VDEC: vdec-full-quality.flag: H.264 always fully deblocked");
		}
	}

	// Frame interval in us: the game's frame rate override, else the stream's frame rate, else 29.97 fps.
	u64 ps4_frame_interval_us() const
	{
		switch (frc_set)
		{
		case CELL_VDEC_FRC_24000DIV1001: return 41708;
		case CELL_VDEC_FRC_24: return 41667;
		case CELL_VDEC_FRC_25: return 40000;
		case CELL_VDEC_FRC_30000DIV1001: return 33367;
		case CELL_VDEC_FRC_30: return 33333;
		case CELL_VDEC_FRC_50: return 20000;
		case CELL_VDEC_FRC_60000DIV1001: return 16683;
		case CELL_VDEC_FRC_60: return 16667;
		default: break;
		}

		if (ctx && ctx->framerate.num > 0 && ctx->framerate.den > 0)
		{
			const u64 us = u64{1'000'000} * static_cast<u64>(ctx->framerate.den) / static_cast<u64>(ctx->framerate.num);

			if (us >= 8'000 && us <= 100'000)
			{
				return us;
			}
		}

		return 33367;
	}

	// Decoding behind the frame interval: skip the loop filter on non-reference pictures once the EMA stays above
	// 0.9x for 30 AUs (no drift, reference pictures stay filtered), on all pictures when it is then above 1.1x (slight
	// blockiness until the next IDR), and back to full deblocking after 60 AUs below 0.6x.
	void ps4_update_deblocking(u64 took_us)
	{
		if (!ps4_deblock_adaptive)
		{
			return;
		}

		const s64 took = static_cast<s64>(std::min<u64>(took_us, 1'000'000));
		ps4_decode_ema_us = ps4_decode_ema_us < 0 ? took : ps4_decode_ema_us + (took - ps4_decode_ema_us) / 8;

		const u64 ema = static_cast<u64>(ps4_decode_ema_us);
		const u64 interval = ps4_frame_interval_us();

		ps4_deblock_above = ema * 10 > interval * 9 ? ps4_deblock_above + 1 : 0;
		ps4_deblock_below = ema * 10 < interval * 6 ? ps4_deblock_below + 1 : 0;

		u32 level = ps4_deblock_level;

		if (ps4_deblock_above >= 30)
		{
			level = ema * 10 > interval * 11 ? 2 : std::max<u32>(level, 1);
		}
		else if (ps4_deblock_below >= 60)
		{
			level = 0;
		}

		if (level == ps4_deblock_level)
		{
			return;
		}

		const std::string line = fmt::format("VDEC: H.264 deblocking %s -> %s (handle=0x%x, decode avg %.1f ms per AU, frame interval %.1f ms)",
			ps4_vdec_deblock_name(ps4_deblock_level), ps4_vdec_deblock_name(level), handle, ema / 1e3, interval / 1e3);
		cellVdec.notice("%s", line);

		if (level && !s_ps4_vdec_deblock_noted.exchange(true))
		{
			ps4_startup_checkpoint((line + "; vdec-full-quality.flag keeps full deblocking").c_str());
		}

		ps4_deblock_level = level;
	}

	void ps4_reset_deblocking()
	{
		ps4_deblock_level = 0;
		ps4_decode_ema_us = -1;
		ps4_deblock_above = 0;
		ps4_deblock_below = 0;
	}

	// FFmpeg ran out of memory (the console heap is capped at 2 GiB; a PC never gets here). The first time in an AU:
	// release one heap reserve block through the new_handler, wait 20 ms and let the caller retry (returns true). The
	// second time: flush the decoder and end the AU without a picture (AUDONE is still sent; the next IDR resyncs).
	bool ps4_out_of_memory(const char* step, const vdec_cmd& cmd, bool& retried)
	{
		const int level = utils::ps4::governor::heap_pressure();
		const utils::ps4::governor::heap_sample heap = utils::ps4::governor::sample_heap(false);
		char line[256];

		if (!retried)
		{
			retried = true;
			ps4_perf_oom_retries++;
			std::snprintf(line, sizeof(line), "VDEC: out of memory in %s (handle=0x%x, seq_id=%llu, cmd_id=%llu, heap level %d, live %llu of %llu MiB); retrying once in 20 ms",
				step, handle, static_cast<unsigned long long>(cmd.seq_id), static_cast<unsigned long long>(cmd.id), level,
				static_cast<unsigned long long>(heap.live >> 20), static_cast<unsigned long long>(heap.cap >> 20));
			cellVdec.error("%s", line);

			if (!s_ps4_vdec_oom_noted.exchange(true))
			{
				ps4_startup_checkpoint(line);
			}

			if (const std::new_handler release = std::get_new_handler())
			{
				release();
			}

			thread_ctrl::wait_for(20'000);
			return true;
		}

		avcodec_flush_buffers(ctx);
		ps4_perf_oom_drops++;
		std::snprintf(line, sizeof(line), "VDEC: AU dropped after out of memory in %s (handle=0x%x, seq_id=%llu, cmd_id=%llu, heap level %d): decoder flushed, AUDONE without a picture",
			step, handle, static_cast<unsigned long long>(cmd.seq_id), static_cast<unsigned long long>(cmd.id), level);
		cellVdec.error("%s", line);

		if (!s_ps4_vdec_drop_noted.exchange(true))
		{
			ps4_startup_checkpoint(line);
		}

		return false;
	}

	// avcodec_send_packet with the out-of-memory handling above. 'dropped' is set when the AU ends without a picture.
	int ps4_send_packet(const AVPacket* packet, const vdec_cmd& cmd, bool& retried, bool& dropped)
	{
		int ret = avcodec_send_packet(ctx, packet);

		while (ret == AVERROR(ENOMEM))
		{
			if (!ps4_out_of_memory("avcodec_send_packet", cmd, retried))
			{
				dropped = true;
				return 0;
			}

			ret = avcodec_send_packet(ctx, packet);

			// The failed call kept the packet: the receive loop decodes it
			if (ret == AVERROR(EAGAIN))
			{
				ret = 0;
			}
		}

		return ret;
	}

	// 0.38: picture queue bound for the next picture 'frame' (decoder thread): memSize / picture bytes clamped to
	// 4..60, at most 8 at memory level 1 or above, 60 with vdec-queue-60.flag. One line per change (at most 32 per process).
	u32 ps4_update_queue_limit(const vdec_frame& frame)
	{
		namespace hle = utils::ps4::hle_memory;
		static const bool upstream = hle::flag(hle::vdec_queue_flag);

		if (frame.avf)
		{
			ps4_picture_bytes = hle::yuv420_picture_bytes(frame->linesize[0], frame->linesize[1], frame->linesize[2], frame->height);
		}

		const int level = upstream ? 0 : hle::level();
		const u32 limit = upstream ? hle::vdec_queue_max : hle::vdec_queue_limit(mem_size, ps4_picture_bytes, level);
		const u32 old = ps4_queue_limit.exchange(limit, std::memory_order_relaxed);

		if (old != limit && s_ps4_vdec_queue_lines++ < 32)
		{
			char line[256];
			std::snprintf(line, sizeof(line), "VDEC: picture queue %u -> %u pictures (handle=0x%x, memSize %u KiB, picture %llu KiB, memory level %d%s)",
				old, limit, handle, mem_size >> 10, static_cast<unsigned long long>(ps4_picture_bytes >> 10), level, upstream ? ", vdec-queue-60.flag" : "");
			cellVdec.notice("%s", line);
			ps4_startup_checkpoint(line);
		}

		return limit;
	}

	void ps4_note_decode(u64 start_us, u64 end_us, bool drain, bool skip_mode, usz pics)
	{
		const u64 took = end_us - start_us;

		if (!drain)
		{
			if (ps4_deblock_level)
			{
				ps4_perf_deblock_aus++;
			}

			ps4_update_deblocking(took);
		}

		if (!ps4_perf_start)
		{
			ps4_perf_start = start_us;

			// Waits before the window's first decode (between windows or movies) belong to no window
			ps4_perf_starved_us = 0;
		}

		ps4_perf_last = end_us;
		ps4_perf_total_us += took;
		ps4_perf_max_us = std::max(ps4_perf_max_us, took);
		ps4_perf_pics += static_cast<u32>(pics);
		(drain ? ps4_perf_drains : ps4_perf_aus)++;

		if (skip_mode && !drain)
		{
			ps4_perf_skip_aus++;
		}

		const u64 window_us = s_ps4_vdec_perf_lines < 360 ? 10'000'000 : 60'000'000;

		if (end_us - ps4_perf_start >= window_us)
		{
			ps4_report_perf("window");
		}
	}

	void ps4_note_conversion(u64 took, int out_format, bool fast)
	{
		ps4_conv_count.fetch_add(1, std::memory_order_relaxed);
		ps4_conv_total_us.fetch_add(took, std::memory_order_relaxed);
		ps4_conv_out_format.store(out_format, std::memory_order_relaxed);

		if (fast)
		{
			ps4_conv_fast.fetch_add(1, std::memory_order_relaxed);
		}

		for (u64 max = ps4_conv_max_us.load(std::memory_order_relaxed); max < took && !ps4_conv_max_us.compare_exchange_weak(max, took, std::memory_order_relaxed);)
		{
		}
	}

	// 0.40: once per decoder (at most 16 per process), when it first waited 5 s for the game to take a picture: whether
	// the game took any, how long ago and on which thread, that thread's last HLE calls (what it waits on instead), the
	// AUs in flight and the queue. Decoder thread; reads the other thread through IDM without its lock (no lock held).
	void ps4_note_consumer_stopped()
	{
		if (std::exchange(ps4_consumer_noted, true))
		{
			return;
		}

		const u32 nth = s_ps4_vdec_consumer_lines++;

		if (nth >= 16)
		{
			return;
		}

		usz queued = 0;
		{
			std::lock_guard lock{mutex};
			queued = out_queue.size();
		}

		const u32 taken = ps4_pics_taken.load(std::memory_order_relaxed);
		const u64 last_us = ps4_getpic_last_us.load(std::memory_order_relaxed);
		const u32 tid = ps4_getpic_last_tid.load(std::memory_order_relaxed);
		std::string who = "no picture taken yet";

		if (last_us)
		{
			const u64 now = ps4_vdec_now_us();
			who = fmt::format("last GetPicture %u ms ago", now > last_us ? (now - last_us) / 1000 : 0);

			if (const auto thread = idm::get_unlocked<named_thread<ppu_thread>>(tid))
			{
				fmt::append(who, " by '%s'", *thread->ppu_tname.load());

				ps4_hle_call calls[6];
				const u32 n = ps4_ppu_recent_calls(*thread, calls, static_cast<u32>(std::size(calls)));

				if (n)
				{
					who += " (its last calls:";

					for (u32 i = 0; i < n; i++)
					{
						fmt::append(who, " %s(0x%llx)", calls[i].name ? calls[i].name : "?", calls[i].ret);
					}

					who += ")";
				}
			}
			else if (tid)
			{
				fmt::append(who, " by PPU thread 0x%x (gone)", tid);
			}
		}

		const std::string line = fmt::format("VDEC: consumer stopped: pictures taken %u of %u, %s, AUs in flight %d, queue %u/%u, memory level %d (handle=0x%x)",
			taken, ps4_pics_queued, who, au_count.load(), queued, ps4_queue_limit.load(std::memory_order_relaxed), utils::ps4::governor::memory_level(), handle);
		cellVdec.error("%s", line);

		if (nth == 0)
		{
			ps4_startup_checkpoint(line.c_str());
		}
	}

	// One line per window with the decoder thread's cost: avg/max per decode call (send_packet + receive loop) and
	// busy = decode time / window wall time. Busy near 100% means the decoder thread itself is the bottleneck.
	void ps4_report_perf(const char* why)
	{
		const u32 calls = ps4_perf_aus + ps4_perf_drains;

		if (!calls || !ctx || !codec)
		{
			return;
		}

		const u64 window_us = std::max<u64>(ps4_perf_last - ps4_perf_start, 1);
		const u64 conv = ps4_conv_count.exchange(0, std::memory_order_relaxed);
		const u64 conv_total_us = ps4_conv_total_us.exchange(0, std::memory_order_relaxed);
		const u64 conv_max_us = ps4_conv_max_us.exchange(0, std::memory_order_relaxed);

		std::string stream_fps = "?";

		if (ctx->framerate.num > 0 && ctx->framerate.den > 0)
		{
			stream_fps = fmt::format("%.2f", ctx->framerate.num / static_cast<f64>(ctx->framerate.den));
		}

		// A final window of a few AUs is mostly decode time by construction; the ratio only means something over seconds
		const std::string busy = window_us >= 1'000'000 ? fmt::format("%u%%", std::min<u64>(ps4_perf_total_us * 100 / window_us, 100)) : std::string("n/a");

		// deblock= is the adaptive level now and the AUs of this window decoded with the loop filter skipped
		const std::string deblock = !ps4_deblock_adaptive ? std::string(type == CELL_VDEC_CODEC_TYPE_AVC ? "full (flag)" : "n/a") :
			fmt::format("%s (skipped on %u AUs)", ps4_vdec_deblock_name(ps4_deblock_level), ps4_perf_deblock_aus);
		const std::string oom = ps4_perf_oom_retries || ps4_perf_oom_drops ? fmt::format("; out of memory: retries=%u dropped AUs=%u", ps4_perf_oom_retries, ps4_perf_oom_drops) : std::string();

		// pace = AUs per second against the stream rate: below it with a low busy%, starved (feeder late) or blocked
		// (consumer late) names the stage that holds the movie back
		const std::string pace = window_us >= 1'000'000 ? fmt::format("%.1f", ps4_perf_aus * 1e6 / window_us) : std::string("n/a");
		const std::string slices = ps4_perf_slice_aus ? fmt::format(" slices/AU=%.1f", ps4_perf_slices / static_cast<f64>(ps4_perf_slice_aus)) : std::string();
		const u32 getpic_empty = ps4_getpic_empty.exchange(0, std::memory_order_relaxed);

		// conv= names the converter of this window's RGB pictures (utils::ps4::yuv or swscale)
		const u64 conv_fast = ps4_conv_fast.exchange(0, std::memory_order_relaxed);
		const char* const fast_name = utils::ps4::yuv::yuv420_to_rgb32_simd ? "sse" : "scalar";
		const std::string conv_kind = !conv ? std::string("n/a") : conv_fast >= conv ? std::string(fast_name) : !conv_fast ? std::string("sws") : fmt::format("%s %u sws %u", fast_name, conv_fast, conv - conv_fast);

		const std::string line = fmt::format("VDEC perf (%s): handle=0x%x codec=%s %dx%d %s stream=%s fps threads=%d%s window=%.2f s AUs=%u skip-mode=%u drains=%u pics=%u decode avg=%.2f ms max=%.2f ms busy=%s deblock=%s; pace=%s AU/s of %s fps starved=%u ms blocked=%u ms queue max=%u of %u GetPicture empty=%u%s; GetPicture conversions=%u to %s conv=%s avg=%.2f ms max=%.2f ms%s",
			why, handle, codec->name, ctx->width, ctx->height, ps4_vdec_pix_fmt_name(ctx->pix_fmt), stream_fps, ctx->thread_count, (ctx->active_thread_type & FF_THREAD_SLICE) ? " (slice)" : "",
			window_us / 1e6, ps4_perf_aus, ps4_perf_skip_aus, ps4_perf_drains, ps4_perf_pics,
			ps4_perf_total_us / 1e3 / calls, ps4_perf_max_us / 1e3, busy, deblock,
			pace, stream_fps, ps4_perf_starved_us / 1000, ps4_perf_blocked_us / 1000, ps4_perf_queue_max, ps4_queue_limit.load(std::memory_order_relaxed), getpic_empty, slices,
			conv, ps4_vdec_pix_fmt_name(ps4_conv_out_format.load(std::memory_order_relaxed)), conv_kind, conv ? conv_total_us / 1e3 / conv : 0., conv_max_us / 1e3, oom);

		cellVdec.notice("%s", line);
		ps4_startup_checkpoint(line.c_str());
		s_ps4_vdec_perf_lines++;

		ps4_perf_start = 0;
		ps4_perf_last = 0;
		ps4_perf_total_us = 0;
		ps4_perf_max_us = 0;
		ps4_perf_aus = 0;
		ps4_perf_drains = 0;
		ps4_perf_pics = 0;
		ps4_perf_skip_aus = 0;
		ps4_perf_deblock_aus = 0;
		ps4_perf_oom_retries = 0;
		ps4_perf_oom_drops = 0;
		ps4_perf_starved_us = 0;
		ps4_perf_blocked_us = 0;
		ps4_perf_queue_max = 0;
		ps4_perf_slice_aus = 0;
		ps4_perf_slices = 0;
	}
#endif

	vdec_context(s32 type, u32 /*profile*/, u32 addr, u32 size, vm::ptr<CellVdecCbMsg> func, u32 arg)
		: type(type)
		, mem_addr(addr)
		, mem_size(size)
		, cb_func(func)
		, cb_arg(arg)
	{
		switch (type)
		{
		case CELL_VDEC_CODEC_TYPE_MPEG2:
		{
			codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
			break;
		}
		case CELL_VDEC_CODEC_TYPE_AVC:
		{
			codec = avcodec_find_decoder(AV_CODEC_ID_H264);
			break;
		}
		case CELL_VDEC_CODEC_TYPE_MPEG4:
		case CELL_VDEC_CODEC_TYPE_DIVX:
		{
			codec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
			break;
		}
		default:
		{
			fmt::throw_exception("Unknown video decoder type (0x%x)", type);
		}
		}

		if (!codec)
		{
			fmt::throw_exception("avcodec_find_decoder() failed (type=0x%x)", type);
		}

		codec_desc = avcodec_descriptor_get(codec->id);

		if (!codec_desc)
		{
			fmt::throw_exception("avcodec_descriptor_get() failed (type=0x%x)", type);
		}

		ctx = avcodec_alloc_context3(codec);

		if (!ctx)
		{
#ifdef RPCS3_PS4
			ps4_open_error = "avcodec_alloc_context3() failed";
			return;
#else
			fmt::throw_exception("avcodec_alloc_context3() failed (type=0x%x)", type);
#endif
		}

		// Carry the AU userdata from each packet to the picture decoded from it, across the frame reordering
		ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

#ifdef RPCS3_PS4
		ps4_configure_threads();
#endif

		AVDictionary* opts = nullptr;

		std::lock_guard lock(g_mutex_avcodec_open2);

		int err = avcodec_open2(ctx, codec, &opts);

#ifdef RPCS3_PS4
		if (err < 0 && ps4_slice_threads)
		{
			// 0.33: the slice worker is a plain pthread; when the console is out of thread resources its creation fails
			// and so does avcodec_open2. Decode on this thread alone, as without the flag, instead of failing the open.
			// A context whose open failed must be freed, not reopened.
			if (ps4_vdec_open_lines_due())
			{
				const std::string line = fmt::format("VDEC: avcodec_open2 with slice threads failed (err=0x%x='%s'); opening single-threaded", err, utils::av_error_to_string(err));
				cellVdec.error("%s", line);
				ps4_startup_checkpoint(line.c_str());
			}

			av_dict_free(&opts);
			avcodec_free_context(&ctx);
			ps4_slice_threads = false;

			ctx = avcodec_alloc_context3(codec);

			if (!ctx)
			{
				ps4_open_error = "avcodec_alloc_context3() failed";
				return;
			}

			ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
			err = avcodec_open2(ctx, codec, &opts);
		}
#endif

		if (err || opts)
		{
			avcodec_free_context(&ctx);
			std::string dict_content;
			if (opts)
			{
				AVDictionaryEntry* tag = nullptr;
				while ((tag = av_dict_get(opts, "", tag, AV_DICT_IGNORE_SUFFIX)))
				{
					fmt::append(dict_content, "['%s': '%s']", tag->key, tag->value);
				}
			}
#ifdef RPCS3_PS4
			av_dict_free(&opts);
			if (ps4_vdec_open_lines_due())
			{
				cellVdec.error("avcodec_open2() failed (err=0x%x='%s', opts=%s)", err, utils::av_error_to_string(err), dict_content);
			}
			ps4_open_error = "avcodec_open2() failed";
			return;
#else
			fmt::throw_exception("avcodec_open2() failed (err=0x%x='%s', opts=%s)", err, utils::av_error_to_string(err), dict_content);
#endif
		}

		av_dict_free(&opts);

#ifdef RPCS3_PS4
		if (ps4_slice_threads && ps4_vdec_open_lines_due())
		{
			// What FFmpeg actually started (it falls back to thread_count=1 on its own in some cases)
			cellVdec.notice("VDEC: %s opened with thread_count=%d active_thread_type=%d", codec->name, ctx->thread_count, ctx->active_thread_type);
		}

		ps4_configure_deblocking();
#endif

		seq_state = sequence_state::dormant;
	}

	~vdec_context()
	{
		release_decoder();
	}

	// Frees the FFmpeg decoder, which may take milliseconds
	void release_decoder()
	{
#ifdef RPCS3_PS4
		// The decoder thread is gone by now (the reaper waits for the last other reference): report what the last
		// window collected, so a movie shorter than one window still gets its line.
		ps4_report_perf("closed");
#endif

		avcodec_free_context(&ctx);
		sws_freeContext(std::exchange(sws, nullptr));
	}

	static u32 freq_to_framerate_code(f64 freq)
	{
		if (std::abs(freq - 23.976) < 0.002) return CELL_VDEC_FRC_24000DIV1001;
		if (std::abs(freq - 24.000) < 0.001) return CELL_VDEC_FRC_24;
		if (std::abs(freq - 25.000) < 0.001) return CELL_VDEC_FRC_25;
		if (std::abs(freq - 29.970) < 0.002) return CELL_VDEC_FRC_30000DIV1001;
		if (std::abs(freq - 30.000) < 0.001) return CELL_VDEC_FRC_30;
		if (std::abs(freq - 50.000) < 0.001) return CELL_VDEC_FRC_50;
		if (std::abs(freq - 59.940) < 0.002) return CELL_VDEC_FRC_60000DIV1001;
		if (std::abs(freq - 60.000) < 0.001) return CELL_VDEC_FRC_60;
		return 0;
	}

	void exec(ppu_thread& ppu, u32 vid)
	{
		perf_meter<"VDEC"_u32> perf0;

		ppu_tid.release(ppu.id);

		for (auto slice = in_cmd.pop_all(); thread_ctrl::state() != thread_state::aborting; [&]
		{
			if (slice)
			{
				slice.pop_front();
			}

			if (slice || thread_ctrl::state() == thread_state::aborting)
			{
				return;
			}

#ifdef RPCS3_PS4
			// Idle while a sequence runs: the next AU has not been queued yet
			const u64 ps4_wait_start = seq_state == sequence_state::ready ? ps4_vdec_now_us() : 0;
#endif

			thread_ctrl::wait_on(in_cmd);

#ifdef RPCS3_PS4
			if (ps4_wait_start)
			{
				ps4_perf_starved_us += ps4_vdec_now_us() - ps4_wait_start;
			}
#endif

			slice = in_cmd.pop_all(); // Pop new command list
		}())
		{
			// pcmd can be nullptr
			auto* cmd = slice.get();

			if (!cmd)
			{
				continue;
			}

			switch (cmd->type)
			{
			case vdec_cmd_type::start_sequence:
			{
				std::lock_guard lock{mutex};

				if (seq_state == sequence_state::resetting)
				{
					cellVdec.trace("Reset sequence... (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
				}
				else
				{
					cellVdec.trace("Start sequence... (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
				}

				avcodec_flush_buffers(ctx);

#ifdef RPCS3_PS4
				// A new sequence (often a new movie) starts fully deblocked
				ps4_reset_deblocking();
#endif

				out_queue.clear(); // Flush image queue
				log_time_base = {};
				log_framerate = {};

				frc_set = 0; // TODO: ???
				next_pts = 0;
				next_dts = 0;

				abort_decode = false;
				is_running = true;
				break;
			}
			case vdec_cmd_type::end_sequence:
			{
				cellVdec.trace("End sequence... (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);

				{
					std::lock_guard lock{mutex};
					seq_state = sequence_state::dormant;
				}

				cellVdec.trace("Sending CELL_VDEC_MSG_TYPE_SEQDONE (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
				cb_func(ppu, vid, CELL_VDEC_MSG_TYPE_SEQDONE, CELL_OK, cb_arg);
				lv2_obj::sleep(ppu);

#ifdef RPCS3_PS4
				ps4_report_perf("end of sequence");
#endif
				break;
			}
			case vdec_cmd_type::drain:
			case vdec_cmd_type::au_decode:
			{
				AVPacket packet{};
				packet.pos = -1;

				u64 au_usrd{};

				// When draining, the command has no AU. Sending a null packet makes the decoder output the pictures it still holds.
				const bool drain = cmd->type == vdec_cmd_type::drain;

				const u32 au_mode = cmd->mode;
				const u32 au_addr = cmd->au.startAddr;
				const u32 au_size = cmd->au.size;
				const u64 au_pts = u64{cmd->au.pts.upper} << 32 | cmd->au.pts.lower;
				const u64 au_dts = u64{cmd->au.dts.upper} << 32 | cmd->au.dts.lower;
				au_usrd = cmd->au.userData;

				packet.data = vm::_ptr<u8>(au_addr);
				packet.size = au_size;
				packet.pts = au_pts != umax ? au_pts : s64{smin};
				packet.dts = au_dts != umax ? au_dts : s64{smin};
				packet.opaque = reinterpret_cast<void*>(au_usrd);

				if (next_pts == 0 && au_pts != umax)
				{
					next_pts = au_pts;
				}

				if (next_dts == 0 && au_dts != umax)
				{
					next_dts = au_dts;
				}

				const CellVdecPicAttr attr = au_mode == CELL_VDEC_DEC_MODE_NORMAL ? CELL_VDEC_PICITEM_ATTR_NORMAL : CELL_VDEC_PICITEM_ATTR_SKIPPED;

				ctx->skip_frame =
					au_mode == CELL_VDEC_DEC_MODE_NORMAL ? AVDISCARD_DEFAULT :
					au_mode == CELL_VDEC_DEC_MODE_B_SKIP ? AVDISCARD_NONREF : AVDISCARD_NONINTRA;

#ifdef RPCS3_PS4
				ctx->skip_loop_filter =
					ps4_deblock_level >= 2 ? AVDISCARD_ALL :
					ps4_deblock_level == 1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
#endif

				std::deque<vdec_frame> decoded_frames;

#ifdef RPCS3_PS4
				// Decode time only (send_packet + receive loop); the callbacks and the wait for queue space are not counted
				u64 ps4_decode_start = 0;
				u64 ps4_decode_end = 0;
				usz ps4_decoded_pics = 0;

				// Out-of-memory handling (ps4_out_of_memory): one retry per AU, then the AU ends without (more) pictures
				bool ps4_oom_retried = false;
				bool ps4_au_dropped = false;
#endif

				if (!abort_decode && seq_id == cmd->seq_id)
				{
					cellVdec.trace("AU decoding: handle=0x%x, seq_id=%d, cmd_id=%d, size=0x%x, pts=0x%llx, dts=0x%llx, userdata=0x%llx", handle, cmd->seq_id, cmd->id, au_size, au_pts, au_dts, au_usrd);

#ifdef RPCS3_PS4
					// Slices per H.264 picture for the perf line (one AU in four, outside the decode time)
					if (!drain && type == CELL_VDEC_CODEC_TYPE_AVC && packet.data && (ps4_perf_aus & 3) == 0)
					{
						ps4_perf_slices += ps4_vdec_h264_slices(packet.data, au_size);
						ps4_perf_slice_aus++;
					}

					ps4_decode_start = ps4_vdec_now_us();

					if (int ret = ps4_send_packet(drain ? nullptr : &packet, *cmd, ps4_oom_retried, ps4_au_dropped); ret < 0)
#else
					if (int ret = avcodec_send_packet(ctx, drain ? nullptr : &packet); ret < 0)
#endif
					{
						fmt::throw_exception("AU queuing error (handle=0x%x, seq_id=%d, cmd_id=%d, error=0x%x): %s", handle, cmd->seq_id, cmd->id, ret, utils::av_error_to_string(ret));
					}

					while (!abort_decode && seq_id == cmd->seq_id)
					{
#ifdef RPCS3_PS4
						if (ps4_au_dropped)
						{
							break;
						}
#endif

						// Keep receiving frames
						vdec_frame frame;
						frame.seq_id = cmd->seq_id;
						frame.cmd_id = cmd->id;
						frame.avf.reset(av_frame_alloc());

						if (!frame.avf)
						{
#ifdef RPCS3_PS4
							if (ps4_out_of_memory("av_frame_alloc", *cmd, ps4_oom_retried))
							{
								continue;
							}

							ps4_au_dropped = true;
							break;
#else
							fmt::throw_exception("av_frame_alloc() failed (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
#endif
						}

						if (int ret = avcodec_receive_frame(ctx, frame.avf.get()); ret < 0)
						{
							if (ret == AVERROR(EAGAIN) || ret == averror_eof)
							{
								break;
							}

#ifdef RPCS3_PS4
							if (ret == AVERROR(ENOMEM))
							{
								if (ps4_out_of_memory("avcodec_receive_frame", *cmd, ps4_oom_retried))
								{
									continue;
								}

								ps4_au_dropped = true;
								break;
							}
#endif

							fmt::throw_exception("AU decoding error (handle=0x%x, seq_id=%d, cmd_id=%d, error=0x%x): %s", handle, cmd->seq_id, cmd->id, ret, utils::av_error_to_string(ret));
						}

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(60, 31, 102)
						const int ticks_per_frame = ctx->ticks_per_frame;
#else
						const int ticks_per_frame = (codec_desc->props & AV_CODEC_PROP_FIELDS) ? 2 : 1;
#endif

#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58, 29, 100)
						const bool is_interlaced = frame->interlaced_frame != 0;
#else
						const bool is_interlaced = !!(frame->flags & AV_FRAME_FLAG_INTERLACED);
#endif

						if (is_interlaced)
						{
							// NPEB01838, NPUB31260
							cellVdec.todo("Interlaced frames not supported (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
						}

						if (frame->repeat_pict)
						{
							fmt::throw_exception("Repeated frames not supported (handle=0x%x, seq_id=%d, cmd_id=%d, repear_pict=0x%x)", handle, cmd->seq_id, cmd->id, frame->repeat_pict);
						}

						if (frame->pts != smin)
						{
							next_pts = frame->pts;
						}

						if (frame->pkt_dts != smin)
						{
							next_dts = frame->pkt_dts;
						}

						frame.pts = next_pts;
						frame.dts = next_dts;
						frame.userdata = reinterpret_cast<u64>(frame->opaque);
						frame.attr = attr;

						u64 amend = 0;

						if (frc_set)
						{
							switch (frc_set)
							{
							case CELL_VDEC_FRC_24000DIV1001: amend = 1001 * 90000 / 24000; break;
							case CELL_VDEC_FRC_24: amend = 90000 / 24; break;
							case CELL_VDEC_FRC_25: amend = 90000 / 25; break;
							case CELL_VDEC_FRC_30000DIV1001: amend = 1001 * 90000 / 30000; break;
							case CELL_VDEC_FRC_30: amend = 90000 / 30; break;
							case CELL_VDEC_FRC_50: amend = 90000 / 50; break;
							case CELL_VDEC_FRC_60000DIV1001: amend = 1001 * 90000 / 60000; break;
							case CELL_VDEC_FRC_60: amend = 90000 / 60; break;
							default:
							{
								fmt::throw_exception("Invalid frame rate code set (handle=0x%x, seq_id=%d, cmd_id=%d, frc=0x%x)", handle, cmd->seq_id, cmd->id, frc_set);
							}
							}

							frame.frc = frc_set;
						}
						else if (ctx->time_base.den && ctx->time_base.num)
						{
							const auto freq = 1. * ctx->time_base.den / ctx->time_base.num / ticks_per_frame;

							frame.frc = freq_to_framerate_code(freq);
							if (frame.frc)
							{
								amend = u64{90000} * ctx->time_base.num * ticks_per_frame / ctx->time_base.den;
							}
						}
						else if (ctx->framerate.den && ctx->framerate.num)
						{
							const auto freq = ctx->framerate.num / static_cast<f64>(ctx->framerate.den);

							frame.frc = freq_to_framerate_code(freq);
							if (frame.frc)
							{
								amend = u64{90000} * ctx->framerate.den / ctx->framerate.num;
							}
						}

						if (amend == 0 || frame.frc == 0)
						{
							if (log_time_base.den != ctx->time_base.den || log_time_base.num != ctx->time_base.num || log_framerate.den != ctx->framerate.den || log_framerate.num != ctx->framerate.num)
							{
								cellVdec.error("Invalid frequency (handle=0x%x, seq_id=%d, cmd_id=%d, timebase=%d/%d, tpf=%d framerate=%d/%d)", handle, cmd->seq_id, cmd->id, ctx->time_base.num, ctx->time_base.den, ticks_per_frame, ctx->framerate.num, ctx->framerate.den);
								log_time_base = ctx->time_base;
								log_framerate = ctx->framerate;
							}

							// Hack
							amend = u64{90000} / 30;
							frame.frc = CELL_VDEC_FRC_30;
						}

						next_pts += amend;
						next_dts += amend;

						cellVdec.trace("Got picture (handle=0x%x, seq_id=%d, cmd_id=%d, pts=0x%llx[0x%llx], dts=0x%llx[0x%llx])", handle, cmd->seq_id, cmd->id, frame.pts, frame->pts, frame.dts, frame->pkt_dts);

						decoded_frames.push_back(std::move(frame));
					}

#ifdef RPCS3_PS4
					ps4_decode_end = ps4_vdec_now_us();
					ps4_decoded_pics = decoded_frames.size();
#endif
				}

				if (thread_ctrl::state() != thread_state::aborting)
				{
					// Send AUDONE even if the current sequence was reset and a new sequence was started.
					// A drain command has no AU, so there is nothing to report.
					if (!drain)
					{
						cellVdec.trace("Sending CELL_VDEC_MSG_TYPE_AUDONE (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
						ensure(au_count.try_dec(0));

						cb_func(ppu, vid, CELL_VDEC_MSG_TYPE_AUDONE, CELL_OK, cb_arg);
						lv2_obj::sleep(ppu);
					}

					while (!decoded_frames.empty() && seq_id == cmd->seq_id)
					{
						// Wait until there is free space in the image queue.
						// Do this after pushing the frame to the queue. That way the game can consume the frame and we can move on.
						u32 elapsed = 0;
#ifdef RPCS3_PS4
						u64 ps4_block_start = 0;
#endif
						while (thread_ctrl::state() != thread_state::aborting && !abort_decode && seq_id == cmd->seq_id)
						{
#ifdef RPCS3_PS4
							// 0.38: bounded queue, sampled again on every 10 ms poll (the memory level can change)
							const u32 ps4_limit = ps4_update_queue_limit(decoded_frames.front());
#endif
							{
								std::lock_guard lock{mutex};

#ifdef RPCS3_PS4
								if (out_queue.size() < ps4_limit)
#else
								if (out_queue.size() <= out_max)
#endif
								{
									break;
								}
							}

#ifdef RPCS3_PS4
							if (!ps4_block_start)
							{
								ps4_block_start = ps4_vdec_now_us();
							}
#endif

							thread_ctrl::wait_for(10000);

							if (elapsed++ >= 500) // 5 seconds
							{
								cellVdec.error("Video au decode has been waiting for a consumer for 5 seconds. (handle=0x%x, seq_id=%d, cmd_id=%d, queue_size=%d)", handle, cmd->seq_id, cmd->id, out_queue.size());
#ifdef RPCS3_PS4
								ps4_note_consumer_stopped();
#endif
								elapsed = 0;
							}
						}

#ifdef RPCS3_PS4
						if (ps4_block_start)
						{
							ps4_perf_blocked_us += ps4_vdec_now_us() - ps4_block_start;
						}
#endif

						if (thread_ctrl::state() == thread_state::aborting || abort_decode || seq_id != cmd->seq_id)
						{
							break;
						}

						{
							std::lock_guard lock{mutex};
							out_queue.push_back(std::move(decoded_frames.front()));
							decoded_frames.pop_front();
#ifdef RPCS3_PS4
							ps4_perf_queue_max = std::max<u32>(ps4_perf_queue_max, static_cast<u32>(out_queue.size()));
							ps4_pics_queued++;
#endif
						}

						cellVdec.trace("Sending CELL_VDEC_MSG_TYPE_PICOUT (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
						cb_func(ppu, vid, CELL_VDEC_MSG_TYPE_PICOUT, CELL_OK, cb_arg);
						lv2_obj::sleep(ppu);
					}
				}

				if (abort_decode || seq_id != cmd->seq_id)
				{
					cellVdec.warning("AU decoding: aborted (handle=0x%x, seq_id=%d, cmd_id=%d, abort_decode=%d)", handle, cmd->seq_id, cmd->id, abort_decode.load());
				}
				else
				{
					cellVdec.trace("AU decoding: done (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
				}

#ifdef RPCS3_PS4
				// After AUDONE/PICOUT, so a window's startup.log line never delays the game's callbacks
				if (ps4_decode_start)
				{
					ps4_note_decode(ps4_decode_start, ps4_decode_end, drain, au_mode != CELL_VDEC_DEC_MODE_NORMAL, ps4_decoded_pics);
				}
#endif

				break;
			}
			case vdec_cmd_type::framerate:
			{
				frc_set = cmd->framerate;
				break;
			}
			case vdec_cmd_type::close:
			{
				std::lock_guard lock{mutex};
				out_queue.clear();
				break;
			}
			default:
				fmt::throw_exception("Unknown vdec_cmd_type (handle=0x%x, seq_id=%d, cmd_id=%d, type=%d)", handle, cmd->seq_id, cmd->id, static_cast<u32>(cmd->type));
				break;
			}

			std::lock_guard lock{mutex};
			if (seq_state == sequence_state::closed)
			{
				break;
			}
		}

		// Make sure the state is closed at the end
		std::lock_guard lock{mutex};
		seq_state = sequence_state::closed;
	}
};

// Frees the FFmpeg decoders of closed contexts on a host thread.
// Freeing a decoder takes milliseconds, which would otherwise delay the guest thread closing it.
struct vdec_context_reaper
{
	lf_queue<shared_ptr<vdec_context>> contexts;

	void operator()()
	{
		while (thread_ctrl::state() != thread_state::aborting)
		{
			thread_ctrl::wait_on(contexts);

			// Let the closing sequence of the caller complete first: freeing this much memory slows down the other threads
			thread_ctrl::wait_for(100'000);

			for (auto&& vdec : contexts.pop_all())
			{
				if (vdec.use_count() > 1)
				{
					// Still used by an HLE call in progress, retry later
					contexts.push(std::move(vdec));
					continue;
				}

				vdec->release_decoder();
			}
		}
	}

	static constexpr auto thread_name = "HLE Video Decoder Reaper"sv;
#ifdef RPCS3_PS4
	// 0.39: started by the first vdecClose (only its push wakes it)
	static constexpr bool ps4_lazy = true;
#endif
};

extern bool check_if_vdec_contexts_exist()
{
	bool context_exists = false;

	idm::select<vdec_context>([&](u32, vdec_context&)
	{
		context_exists = true;
	});

	return context_exists;
}

extern void vdecEntry(ppu_thread& ppu, u32 vid)
{
	idm::get_unlocked<vdec_context>(vid)->exec(ppu, vid);

	ppu.state += cpu_flag::exit;
}

template <VdecSceDecoderType decoder_type>
static consteval auto get_sce_decoder_ops()
{
	if constexpr (decoder_type == VdecSceDecoderType::mpeg2)
	{
		return VDEC_SCE_DECODER_OPS_MPEG2;
	}

	if constexpr (decoder_type == VdecSceDecoderType::mpeg4)
	{
		return VDEC_SCE_DECODER_OPS_MPEG4;
	}

	if constexpr (decoder_type == VdecSceDecoderType::vc1)
	{
		return VDEC_SCE_DECODER_OPS_VC1;
	}

	if constexpr (decoder_type == VdecSceDecoderType::jvt)
	{
		return VDEC_SCE_DECODER_OPS_JVT;
	}
}

template <VdecSceDecoderType decoder_type>
static std::optional<u32> get_internal_profile_level(u32 profile_level)
{
	if constexpr (decoder_type == VdecSceDecoderType::jvt)
	{
		switch (profile_level)
		{
		case CELL_VDEC_AVC_LEVEL_1P0: return 1;
		case CELL_VDEC_AVC_LEVEL_1P1: return 2;
		case CELL_VDEC_AVC_LEVEL_1P2: return 3;
		case CELL_VDEC_AVC_LEVEL_1P3: return 4;
		case CELL_VDEC_AVC_LEVEL_2P0: return 5;
		case CELL_VDEC_AVC_LEVEL_2P1: return 6;
		case CELL_VDEC_AVC_LEVEL_2P2: return 7;
		case CELL_VDEC_AVC_LEVEL_3P0: return 8;
		case CELL_VDEC_AVC_LEVEL_3P1: return 9;
		case CELL_VDEC_AVC_LEVEL_3P2: return 10;
		case CELL_VDEC_AVC_LEVEL_4P0: return 11;
		case CELL_VDEC_AVC_LEVEL_4P1: return 12;
		case CELL_VDEC_AVC_LEVEL_4P2: return 13;
		default: return std::nullopt;
		}
	}

	constexpr u32 max_profile_level =
		decoder_type == VdecSceDecoderType::mpeg2 ? CELL_VDEC_MPEG2_MP_HL
		: decoder_type == VdecSceDecoderType::mpeg4 ? CELL_VDEC_MPEG4_SP_D1_PAL
		: decoder_type == VdecSceDecoderType::jvt ? CELL_VDEC_VC1_AP_L4 : 0;

	return profile_level <= max_profile_level ? static_cast<std::optional<u32>>(profile_level + 1) : std::nullopt;
}

static bool check_frame_dimensions_mpeg2(u32 profile_level, be_t<u32>& max_decoded_frame_width, be_t<u32>& max_decoded_frame_height)
{
	const auto [max_width, max_height] = [&] -> std::pair<u32, u32>
	{
		switch (profile_level)
		{
		case SMVD2_MP_LL:  return {  352,  288 };
		case SMVD2_MP_ML:  return {  720,  576 };
		case SMVD2_MP_H14: return { 1440, 1152 };
		case SMVD2_MP_HL:  return { 1920, 1152 };
		default: fmt::throw_exception("Invalid profile level");
		}
	}();

	if (max_decoded_frame_width > max_width || max_decoded_frame_height > max_height)
	{
		return false;
	}

	if (!max_decoded_frame_width || !max_decoded_frame_height)
	{
		max_decoded_frame_width = max_width;
		max_decoded_frame_height = max_height;
	}

	return true;
}

template <VdecSceDecoderType decoder_type>
static u32 get_version(ppu_thread& ppu)
{
	const vm::var<u32> version;

	get_sce_decoder_ops<decoder_type>().get_version_number(ppu, +version);

	return *version;
}

template <VdecSceDecoderType decoder_type, typename specific_info_t>
static error_code get_memory_size(ppu_thread& ppu, u32 profile_level, const vm::var<u32>& mem_size, const specific_info_t* codec_specific_info)
{
	const std::optional profile_level_internal = get_internal_profile_level<decoder_type>(profile_level);

	if (!profile_level_internal)
	{
		return CELL_VDEC_ERROR_ARG;
	}

	const auto ret = [&] -> std::optional<error_code>
	{
		if (!codec_specific_info)
		{
			return get_sce_decoder_ops<decoder_type>().get_memory_size(ppu, +mem_size, *profile_level_internal);
		}

		if (codec_specific_info->thisSize != sizeof(specific_info_t))
		{
			return std::nullopt;
		}

		if constexpr (decoder_type == VdecSceDecoderType::mpeg4)
		{
			if (codec_specific_info->maxDecodedFrameWidth == 0 || codec_specific_info->maxDecodedFrameHeight == 0)
			{
				return ppu_execute<&smvd4GetMemorySize>(ppu, +mem_size, *profile_level_internal);
			}
		}

		const auto params
		{
			[&]
			{
				if constexpr (decoder_type == VdecSceDecoderType::mpeg2)
				{
					return vm::var<Smvd2Params>
					{{
						.unk1 = 5,
						.unk2 = 2,
						.unk3 = 0,
						.max_decoded_frame_width = static_cast<u32>(codec_specific_info->maxDecodedFrameWidth),
						.max_decoded_frame_height = static_cast<u32>(codec_specific_info->maxDecodedFrameHeight)
					}};
				}

				if constexpr (decoder_type == VdecSceDecoderType::mpeg4)
				{
					return vm::var<Smvd4Params>
					{{
						.unk1 = 5,
						.unk2 = 1,
						.unk3 = 0,
						.max_decoded_frame_width = static_cast<u32>(codec_specific_info->maxDecodedFrameWidth),
						.max_decoded_frame_height = static_cast<u32>(codec_specific_info->maxDecodedFrameHeight)
					}};
				}

				if constexpr (decoder_type == VdecSceDecoderType::vc1)
				{
					return vm::var<Svc1dParams>
					{{
						.unk1 = 5,
						.unk2 = 3,
						.unk3 = 0,
						.max_decoded_frame_width = codec_specific_info->maxDecodedFrameWidth != 0 ? static_cast<u32>(codec_specific_info->maxDecodedFrameWidth) : umax,
						.max_decoded_frame_height = codec_specific_info->maxDecodedFrameHeight != 0 ? static_cast<u32>(codec_specific_info->maxDecodedFrameHeight) : umax
					}};
				}

				if constexpr (decoder_type == VdecSceDecoderType::jvt)
				{
					return vm::var<SjvtdParams>
					{{
						.unk1 = 6,
						.unk2 = 4,
						.unk3 = 0,
						.max_decoded_frame_width = codec_specific_info->maxDecodedFrameWidth != 0 ? static_cast<s32>(codec_specific_info->maxDecodedFrameWidth) : -1,
						.max_decoded_frame_height = codec_specific_info->maxDecodedFrameHeight != 0 ? static_cast<s32>(codec_specific_info->maxDecodedFrameHeight) : -1,
						.enable_deblocking_filter = !codec_specific_info->disableDeblockingFilter,
						.unk = umax,
						.number_of_decoded_frame_buffer = codec_specific_info->numberOfDecodedFrameBuffer != 0 ? codec_specific_info->numberOfDecodedFrameBuffer : umax
					}};
				}
			}()
		};

		if constexpr (decoder_type == VdecSceDecoderType::mpeg2)
		{
			if (!check_frame_dimensions_mpeg2(*profile_level_internal, params->max_decoded_frame_width, params->max_decoded_frame_height))
			{
				return std::nullopt;
			}
		}

		return get_sce_decoder_ops<decoder_type>().get_memory_size_2(ppu, +mem_size, *profile_level_internal, +params);
	}();

	if (!ret)
	{
		return CELL_VDEC_ERROR_ARG;
	}

	if ((*ret & 0xffffffc0) == VDEC_SCE_DECODER_ERROR_BASE_MAP[std::to_underlying(decoder_type)])
	{
		return CELL_VDEC_ERROR_UNK;
	}

	if (*ret != CELL_OK || *mem_size > VDEC_SCE_DECODER_MAX_MEM_SIZE_MAP[std::to_underlying(decoder_type)][*profile_level_internal - 1])
	{
		return CELL_VDEC_ERROR_FATAL;
	}

	return CELL_OK;
}

template <VdecSceDecoderType decoder_type, typename specific_info_t>
static error_code query_attr_mpeg_vc1_jvt(ppu_thread& ppu, VdecDecoderAttr& attr, u32 profile_level, const void* codec_specific_info)
{
	attr.mem_size = 0;
	attr.unk2 = 0x20;

	const vm::var<u32> mem_size;

	if (const error_code ret = get_memory_size<decoder_type, specific_info_t>(ppu, profile_level, mem_size, static_cast<const specific_info_t*>(codec_specific_info)); ret != CELL_OK)
	{
		return ret;
	}

	attr.mem_size = *mem_size + (5 * 0x50) + 0xa8 + 0x41580 + 0x3493;
	attr.unk1 = VDEC_SCE_DECODER_UNK_MAP[std::to_underlying(decoder_type)].unk4;
	attr.cmd_depth = 4;
	attr.decoder_version = get_version<decoder_type>(ppu);

	return CELL_OK;
}

template <bool is_mvc>
static error_code query_memory_avc_mvc(ppu_thread& ppu, u32 profile_level, vm::ptr<AvcDecAttr> attr, const CellVdecAvcSpecificInfo* avc_specific_info)
{
	const vm::var<AvcDecParams> avc_params{ AvcDecParams{} };

	switch (profile_level)
	{
	case CELL_VDEC_AVC_LEVEL_1P0:
	case CELL_VDEC_AVC_LEVEL_1P1:
	case CELL_VDEC_AVC_LEVEL_1P2:
	case CELL_VDEC_AVC_LEVEL_1P3:
	case CELL_VDEC_AVC_LEVEL_2P0:
	case CELL_VDEC_AVC_LEVEL_2P1:
	case CELL_VDEC_AVC_LEVEL_2P2:
	case CELL_VDEC_AVC_LEVEL_3P0:
	case CELL_VDEC_AVC_LEVEL_3P1:
	case CELL_VDEC_AVC_LEVEL_3P2:
	case CELL_VDEC_AVC_LEVEL_4P0:
	case CELL_VDEC_AVC_LEVEL_4P1:
	case CELL_VDEC_AVC_LEVEL_4P2:
		avc_params->profile_level = profile_level;
		break;

	case CELL_VDEC_AVC_LEVEL_UNK:
		avc_params->profile_level = CELL_VDEC_AVC_LEVEL_4P2;
		avc_params->disable_deblocking_filter = true;
		break;

	default:
		return CELL_VDEC_ERROR_ARG;
	}

	if (avc_specific_info)
	{
		if (avc_specific_info->thisSize != sizeof(CellVdecAvcSpecificInfo))
		{
			return CELL_VDEC_ERROR_ARG;
		}

		avc_params->disable_deblocking_filter = avc_specific_info->disableDeblockingFilter;
		avc_params->number_of_decoded_frame_buffer = avc_specific_info->numberOfDecodedFrameBuffer;

		if (avc_specific_info->maxDecodedFrameWidth != 0 && avc_specific_info->maxDecodedFrameHeight != 0)
		{
			avc_params->max_decoded_frame_width = utils::aligned_div<u16>(avc_specific_info->maxDecodedFrameWidth, 0x10);
			avc_params->max_decoded_frame_height = utils::aligned_div<u16>(avc_specific_info->maxDecodedFrameHeight, 0x10);
		}
	}

	if (ppu_execute<is_mvc ? mvcDecQueryMemory : avcDecQueryMemory>(ppu, +avc_params, attr) != CELL_OK)
	{
		return CELL_VDEC_ERROR_FATAL;
	}

	const u32 max_mem_size = [&]
	{
		switch (profile_level)
		{
		case CELL_VDEC_AVC_LEVEL_1P0: return is_mvc ? 0x9e2980 : 0x6b6d80;
		case CELL_VDEC_AVC_LEVEL_1P1: return is_mvc ? 0xbec300 : 0x822280;
		case CELL_VDEC_AVC_LEVEL_1P2: return is_mvc ? 0xe44d00 : 0x998600;
		case CELL_VDEC_AVC_LEVEL_1P3: // Same as below
		case CELL_VDEC_AVC_LEVEL_2P0: return is_mvc ? 0xe72d00 : 0x9bad80;
		case CELL_VDEC_AVC_LEVEL_2P1: return is_mvc ? 0x13f1380 : 0xe46380;
		case CELL_VDEC_AVC_LEVEL_2P2: return is_mvc ? 0x1aba180 : 0x1499f80;
		case CELL_VDEC_AVC_LEVEL_3P0: return is_mvc ? 0x1b58780 : 0x1510a00;
		case CELL_VDEC_AVC_LEVEL_3P1: return is_mvc ? 0x28c6700 : 0x1c88380;
		case CELL_VDEC_AVC_LEVEL_3P2: return is_mvc ? 0x3208700 : 0x234c800;
		case CELL_VDEC_AVC_LEVEL_4P0: // Same as below
		case CELL_VDEC_AVC_LEVEL_4P1:
		case CELL_VDEC_AVC_LEVEL_4P2:
		case CELL_VDEC_AVC_LEVEL_UNK: return is_mvc ? 0x487ed00 : 0x335ab00;
		default: std::unreachable(); // Already checked above
		}
	}();

	return attr->mem_size > max_mem_size ? static_cast<error_code>(CELL_VDEC_ERROR_FATAL) : CELL_OK;
}

template <bool is_mvc>
static error_code query_attr_avc_mvc(ppu_thread& ppu, VdecDecoderAttr& attr, u32 profile_level, const void* avc_specific_info)
{
	const vm::var<AvcDecAttr> codec_attr;

	if (const error_code ret = query_memory_avc_mvc<is_mvc>(ppu, profile_level, codec_attr, static_cast<const CellVdecAvcSpecificInfo*>(avc_specific_info)); ret != CELL_OK)
	{
		return ret;
	}

	attr.mem_size = 0x41580 + codec_attr->mem_size + (is_mvc ? 0x12fd : 0x89fd);
	attr.unk1 = is_mvc ? 0x214 : 0x244;
	attr.unk2 = 0x30;

	const vm::var<u32> version;
	ppu_execute<is_mvc ? mvcDecGetVersion : avcDecGetVersion>(ppu, +version);

	attr.decoder_version = *version;

	const vm::var<u32[]> unk{ 2 };
	ppu_execute<is_mvc ? mvcDecQueryCharacteristics : avcDecQueryCharacteristics>(ppu, +unk);

	const vm::var<s32> sdk_ver;
	ensure(sys_process_get_sdk_version(sys_process_getpid(), sdk_ver) == CELL_OK); // Not checked on LLE

	attr.cmd_depth = unk[0] - (*sdk_ver >= 0x130000);

	return CELL_OK;
}

template <bool divx311>
static error_code query_attr_divx(ppu_thread& ppu, VdecDecoderAttr& attr, u32 profile_level, const void* divx_specific_info)
{
	attr.mem_size = 0;
	attr.unk2 = 0x20;

	const vm::var<DivxDecParams> params;

	if constexpr (!divx311)
	{
		switch (profile_level)
		{
		case CELL_VDEC_DIVX_QMOBILE:      *params = { .profile_level = 0, .max_decoded_frame_width =  176, .max_decoded_frame_height =  144, .number_of_decoded_frame_buffer = 4 }; break;
		case CELL_VDEC_DIVX_MOBILE:       *params = { .profile_level = 0, .max_decoded_frame_width =  352, .max_decoded_frame_height =  288, .number_of_decoded_frame_buffer = 4 }; break;
		case CELL_VDEC_DIVX_HOME_THEATER: *params = { .profile_level = 0, .max_decoded_frame_width =  720, .max_decoded_frame_height =  576, .number_of_decoded_frame_buffer = 4 }; break;
		case CELL_VDEC_DIVX_HD_720:       *params = { .profile_level = 0, .max_decoded_frame_width = 1280, .max_decoded_frame_height =  720, .number_of_decoded_frame_buffer = 4 }; break;
		case CELL_VDEC_DIVX_HD_1080:      *params = { .profile_level = 0, .max_decoded_frame_width = 1920, .max_decoded_frame_height = 1088, .number_of_decoded_frame_buffer = 4 }; break;
		default: return CELL_VDEC_ERROR_ARG;
		}
	}
	else
	{
		*params = { .profile_level = 0, .max_decoded_frame_width = 720, .max_decoded_frame_height = 576, .number_of_decoded_frame_buffer = 2 };
	}

	if (divx_specific_info)
	{
		const auto* const _divx_specific_info = static_cast<const CellVdecDivxSpecificInfo2*>(divx_specific_info);

		if (((divx311 || _divx_specific_info->thisSize != sizeof(CellVdecDivxSpecificInfo)) && _divx_specific_info->thisSize != sizeof(CellVdecDivxSpecificInfo2))
			|| _divx_specific_info->maxDecodedFrameWidth > (divx311 ? 1920 : +params->max_decoded_frame_width) || _divx_specific_info->maxDecodedFrameHeight > (divx311 ? 1088 : +params->max_decoded_frame_height)
			|| (divx311 && _divx_specific_info->numberOfDecodedFrameBuffer != 0 && _divx_specific_info->numberOfDecodedFrameBuffer != 2))
		{
			return CELL_VDEC_ERROR_ARG;
		}

		if (_divx_specific_info->maxDecodedFrameWidth != 0 && _divx_specific_info->maxDecodedFrameHeight != 0)
		{
			params->max_decoded_frame_width = _divx_specific_info->maxDecodedFrameWidth;
			params->max_decoded_frame_height = _divx_specific_info->maxDecodedFrameHeight;
		}

		if (_divx_specific_info->thisSize == sizeof(CellVdecDivxSpecificInfo2) && _divx_specific_info->numberOfDecodedFrameBuffer != 0)
		{
			params->number_of_decoded_frame_buffer = _divx_specific_info->numberOfDecodedFrameBuffer;
		}
	}

	const vm::var<u32> mem_size;
	const vm::var<u32> decoder_version;

	if (ppu_execute<divx311 ? &divx311DecQueryAttr : &divxDecQueryAttr>(ppu, +params, +mem_size, +decoder_version) != CELL_OK)
	{
		return CELL_VDEC_ERROR_ARG;
	}

	if (divx311 && profile_level != CELL_VDEC_DIVX_UNK)
	{
		return CELL_VDEC_ERROR_ARG;
	}

	const u32 max_mem_size = [&]
	{
		switch (profile_level)
		{
		case CELL_VDEC_DIVX_QMOBILE:      return 0x1577be;
		case CELL_VDEC_DIVX_MOBILE:       return 0x1e675e;
		case CELL_VDEC_DIVX_HOME_THEATER: return 0x4108de;
		case CELL_VDEC_DIVX_HD_720:       return 0x77df0e;
		case CELL_VDEC_DIVX_HD_1080:      return 0xf420fe;
		case CELL_VDEC_DIVX_UNK:          return 0x1ce600;
		default: std::unreachable(); // Already checked above
		}
	}();

	if (*mem_size > max_mem_size)
	{
		return CELL_VDEC_ERROR_ARG;
	}

	attr.mem_size = 0x41580 + (5 * 0x50) + 0xa8 + *mem_size + (divx311 ? 0x3500 : 0x34f8);
	attr.unk1 = divx311 ? 0x18 : 0x20;
	attr.cmd_depth = 4;
	attr.decoder_version = *decoder_version;

	return CELL_OK;
}

static inline bool check_codec_type(u32 codec_type)
{
	return codec_type <= CELL_VDEC_CODEC_TYPE_MPEG4 || (codec_type < CELL_VDEC_CODEC_TYPE_MAX && (codec_type & 1));
}

static VdecDecoderSpecificOps get_decoder_specific_ops(u32 codec_type)
{
	// TODO remaining functions

	switch (codec_type)
	{
	case CELL_VDEC_CODEC_TYPE_MPEG2:
		return
		{
			query_attr_mpeg_vc1_jvt<VdecSceDecoderType::mpeg2, CellVdecMpeg2SpecificInfo>
		};

	case CELL_VDEC_CODEC_TYPE_AVC:
		return
		{
			query_attr_avc_mvc<false>
		};

	case CELL_VDEC_CODEC_TYPE_MPEG4:
		return
		{
			query_attr_mpeg_vc1_jvt<VdecSceDecoderType::mpeg4, CellVdecMpeg4SpecificInfo>
		};

	case CELL_VDEC_CODEC_TYPE_VC1:
		return
		{
			query_attr_mpeg_vc1_jvt<VdecSceDecoderType::vc1, CellVdecVc1SpecificInfo>
		};

	case CELL_VDEC_CODEC_TYPE_DIVX:
		return
		{
			query_attr_divx<false>
		};

	case CELL_VDEC_CODEC_TYPE_JVT:
		return
		{
			query_attr_mpeg_vc1_jvt<VdecSceDecoderType::jvt, CellVdecAvcSpecificInfo>
		};

	case CELL_VDEC_CODEC_TYPE_DIVX3_11:
		return
		{
			query_attr_divx<true>
		};

	case CELL_VDEC_CODEC_TYPE_MVC:
	case CELL_VDEC_CODEC_TYPE_MVC2:
		return
		{
			query_attr_avc_mvc<true>
		};

	default:
		fmt::throw_exception("Invalid codec type");
	}
}

static inline u32 get_unk_size_2(u32 unk)
{
	return (unk * 0x14) + 0x14;
}

static inline u32 get_unk_size(u32 unk1, u32 unk2)
{
	return ((unk2 + 0x90) * unk1) + (get_unk_size_2(unk1) * 2) + 0x40;
}

error_code cellVdecQueryAttr(ppu_thread& ppu, vm::cptr<CellVdecType> type, vm::ptr<CellVdecAttr> attr)
{
#ifdef RPCS3_PS4
	if (ps4_vdec_open_lines_due())
#endif
	cellVdec.notice("cellVdecQueryAttr(type=*0x%x, attr=*0x%x)", type, attr);

	if (!type)
	{
		return CELL_VDEC_ERROR_ARG;
	}

	const vm::var<CellVdecTypeEx> type_ex{{ .codecType = type->codecType, .profileLevel = type->profileLevel, .codecSpecificInfo = vm::null }};
	return cellVdecQueryAttrEx(ppu, type_ex, attr);
}

error_code cellVdecQueryAttrEx(ppu_thread& ppu, vm::cptr<CellVdecTypeEx> type, vm::ptr<CellVdecAttr> attr)
{
#ifdef RPCS3_PS4
	if (ps4_vdec_open_lines_due())
#endif
	cellVdec.notice("cellVdecQueryAttrEx(type=*0x%x, attr=*0x%x)", type, attr);

	if (!type || !attr)
	{
		return CELL_VDEC_ERROR_ARG;
	}

	attr->memSize = 0;

	if (!check_codec_type(type->codecType))
	{
		return CELL_VDEC_ERROR_ARG;
	}

	VdecDecoderAttr decoder_attr {};

	if (get_decoder_specific_ops(type->codecType).query_attr(ppu, decoder_attr, type->profileLevel, type->codecSpecificInfo ? type->codecSpecificInfo.get_ptr() : nullptr) != CELL_OK)
	{
		return CELL_VDEC_ERROR_ARG;
	}

	const u32 unk_size = get_unk_size(decoder_attr.unk2, decoder_attr.unk1);

	u32 mem_size = decoder_attr.mem_size + 0x858 + unk_size;

	const vm::var<s32> sdk_ver;
	ensure(sys_process_get_sdk_version(sys_process_getpid(), sdk_ver) == CELL_OK); // Not checked on LLE

	const bool new_sdk = *sdk_ver >= 0x210000;

	const u32 max_mem_size = [&]
	{
		switch (type->codecType)
		{
		case CELL_VDEC_CODEC_TYPE_MPEG2:
			switch (type->profileLevel)
			{
			case CELL_VDEC_MPEG2_MP_LL:  return new_sdk ? 0x11290b : 0x2a610b;
			case CELL_VDEC_MPEG2_MP_ML:  return new_sdk ? 0x2dfb8b : 0x47110b;
			case CELL_VDEC_MPEG2_MP_H14: return new_sdk ? 0xa0270b : 0xb8f90b;
			case CELL_VDEC_MPEG2_MP_HL:  return new_sdk ? 0xd2f40b : 0xeb990b;
			default: std::unreachable(); // Already checked in VdecDecoderSpecificOps::query_attr
			}

		case CELL_VDEC_CODEC_TYPE_AVC:
			switch (type->profileLevel)
			{
			case CELL_VDEC_AVC_LEVEL_1P0: return new_sdk ? 0x7024fd : 0xa014fd;
			case CELL_VDEC_AVC_LEVEL_1P1: return new_sdk ? 0x86d9fd : 0xb6c9fd;
			case CELL_VDEC_AVC_LEVEL_1P2: return new_sdk ? 0x9e3d7d : 0xce2d7d;
			case CELL_VDEC_AVC_LEVEL_1P3: // Same as below
			case CELL_VDEC_AVC_LEVEL_2P0: return new_sdk ? 0xa064fd : 0xd054fd;
			case CELL_VDEC_AVC_LEVEL_2P1: return new_sdk ? 0xe91afd : 0x1190afd;
			case CELL_VDEC_AVC_LEVEL_2P2: return new_sdk ? 0x14e56fd : 0x17e46fd;
			case CELL_VDEC_AVC_LEVEL_3P0: return new_sdk ? 0x155c17d : 0x185b17d;
			case CELL_VDEC_AVC_LEVEL_3P1: return new_sdk ? 0x1cd3afd : 0x1fd2afd;
			case CELL_VDEC_AVC_LEVEL_3P2: return new_sdk ? 0x2397f7d : 0x2696f7d;
			case CELL_VDEC_AVC_LEVEL_4P0: // Same as below
			case CELL_VDEC_AVC_LEVEL_4P1:
			case CELL_VDEC_AVC_LEVEL_4P2:
			case CELL_VDEC_AVC_LEVEL_UNK: return new_sdk ? 0x33a627d : 0x36a527d;
			default: std::unreachable(); // Already checked in VdecDecoderSpecificOps::query_attr
			}

		case CELL_VDEC_CODEC_TYPE_MPEG4:
			switch (type->profileLevel)
			{
			case CELL_VDEC_MPEG4_SP_L1:      return new_sdk ? 0x8b78b : 0xbb70b;
			case CELL_VDEC_MPEG4_SP_L2: // Same as below
			case CELL_VDEC_MPEG4_SP_L3:      return new_sdk ? 0xefe0b : 0x11fd8b;
			case CELL_VDEC_MPEG4_SP_D1_NTSC: return new_sdk ? 0x22db0b : 0x25da8b;
			case CELL_VDEC_MPEG4_SP_VGA:     return new_sdk ? 0x1fc00b : 0x22bf8b;
			case CELL_VDEC_MPEG4_SP_D1_PAL:  return new_sdk ? 0x28570b : 0x2b568b;
			default: std::unreachable(); // Already checked in VdecDecoderSpecificOps::query_attr
			}

		case CELL_VDEC_CODEC_TYPE_VC1:
			switch (type->profileLevel)
			{
			case CELL_VDEC_VC1_SP_LL: return new_sdk ? 0x1fdefb : 0x291149;
			case CELL_VDEC_VC1_SP_ML: return new_sdk ? 0x3298fb : 0x42a727;
			case CELL_VDEC_VC1_MP_LL: return new_sdk ? 0x3d93fb : 0x42a727;
			case CELL_VDEC_VC1_MP_ML: return new_sdk ? 0x9e383b : 0xa34efd;
			case CELL_VDEC_VC1_MP_HL: return new_sdk ? 0x287197b : 0x28c4363;
			case CELL_VDEC_VC1_AP_L0: return new_sdk ? 0x3298fb : 0x42a727;
			case CELL_VDEC_VC1_AP_L1: return new_sdk ? 0x79db3b : 0xa34efd;
			case CELL_VDEC_VC1_AP_L2: return new_sdk ? 0x12073fb : 0x184b857;
			case CELL_VDEC_VC1_AP_L3: return new_sdk ? 0x202887b : 0x2b562fb;
			case CELL_VDEC_VC1_AP_L4: return new_sdk ? 0x3949a7b : 0x4d0a77b;
			default: std::unreachable(); // Already checked in VdecDecoderSpecificOps::query_attr
			}

		case CELL_VDEC_CODEC_TYPE_DIVX:
			switch (type->profileLevel)
			{
			case CELL_VDEC_DIVX_QMOBILE:      return new_sdk ? 0x19e82e : 0x1def30;
			case CELL_VDEC_DIVX_MOBILE:       return new_sdk ? 0x22d7ce : 0x26ded0;
			case CELL_VDEC_DIVX_HOME_THEATER: return new_sdk ? 0x45794e : 0x498060;
			case CELL_VDEC_DIVX_HD_720:       return new_sdk ? 0x7c4f7e : 0x805690;
			case CELL_VDEC_DIVX_HD_1080:      return new_sdk ? 0xf8916e : 0xfc9870;
			default: std::unreachable(); // Already checked in VdecDecoderSpecificOps::query_attr
			}

		case CELL_VDEC_CODEC_TYPE_JVT:
			switch (type->profileLevel)
			{
			case CELL_VDEC_AVC_LEVEL_1P0: return new_sdk ? 0x3ca7db : 0x3c27db;
			case CELL_VDEC_AVC_LEVEL_1P1: // Same as below
			case CELL_VDEC_AVC_LEVEL_1P2:
			case CELL_VDEC_AVC_LEVEL_1P3:
			case CELL_VDEC_AVC_LEVEL_2P0: return new_sdk ? 0x7ca15b : 0x7c215b;
			case CELL_VDEC_AVC_LEVEL_2P1: return new_sdk ? 0xd0055b : 0xcf855b;
			case CELL_VDEC_AVC_LEVEL_2P2: // Same as below
			case CELL_VDEC_AVC_LEVEL_3P0: return new_sdk ? 0x17ee15b : 0x17e615b;
			case CELL_VDEC_AVC_LEVEL_3P1: return new_sdk ? 0x328f6db : 0x32876db;
			case CELL_VDEC_AVC_LEVEL_3P2: return new_sdk ? 0x44e37db : 0x44db7db;
			case CELL_VDEC_AVC_LEVEL_4P0: // Same as below
			case CELL_VDEC_AVC_LEVEL_4P1:
			case CELL_VDEC_AVC_LEVEL_4P2:
			case CELL_VDEC_AVC_LEVEL_UNK: return new_sdk ? 0x6be11db : 0x6bd91db;
			default: std::unreachable(); // Already checked in VdecDecoderSpecificOps::query_attr
			}

		case CELL_VDEC_CODEC_TYPE_DIVX3_11:
			return 0x215578;

		case CELL_VDEC_CODEC_TYPE_MVC:
		case CELL_VDEC_CODEC_TYPE_MVC2:
			switch (type->profileLevel)
			{
			case CELL_VDEC_AVC_LEVEL_1P0: return 0xa2e0fd;
			case CELL_VDEC_AVC_LEVEL_1P1: return 0xc37a7d;
			case CELL_VDEC_AVC_LEVEL_1P2: return 0xe9047d;
			case CELL_VDEC_AVC_LEVEL_1P3: // Same as below
			case CELL_VDEC_AVC_LEVEL_2P0: return 0xebe47d;
			case CELL_VDEC_AVC_LEVEL_2P1: return 0x143cafd;
			case CELL_VDEC_AVC_LEVEL_2P2: return 0x1b058fd;
			case CELL_VDEC_AVC_LEVEL_3P0: return 0x1ba3efd;
			case CELL_VDEC_AVC_LEVEL_3P1: return 0x2911e7d;
			case CELL_VDEC_AVC_LEVEL_3P2: return 0x3253e7d;
			case CELL_VDEC_AVC_LEVEL_4P0: // Same as below
			case CELL_VDEC_AVC_LEVEL_4P1:
			case CELL_VDEC_AVC_LEVEL_4P2:
			case CELL_VDEC_AVC_LEVEL_UNK: return 0x48ca47d;
			default: std::unreachable(); // Already checked in VdecDecoderSpecificOps::query_attr
			}

		default:
			std::unreachable(); // Already checked above
		}
	}();

	if (mem_size > max_mem_size)
	{
		cellVdec.warning("The required memory size is greater than expected"); // LLE prints an error to stdout
	}
	else if (!new_sdk)
	{
		mem_size = max_mem_size;
	}

	*attr = { .memSize = mem_size, .cmdDepth = decoder_attr.cmd_depth, .decoderVerUpper = 0x4890000, .decoderVerLower = decoder_attr.decoder_version };

	return CELL_OK;
}

template <typename T, typename U>
static error_code vdecOpen(ppu_thread& ppu, T type, U res, vm::cptr<CellVdecCb> cb, vm::ptr<u32> handle)
{
	if (!type || !res || !cb || !handle || !cb->cbFunc)
	{
		return { CELL_VDEC_ERROR_ARG, "type=%d, res=%d, cb=%d, handle=%d, cbFunc=%d", !!type, !!res, !!cb, !!handle, cb && cb->cbFunc };
	}

	if (!res->memAddr || res->ppuThreadPriority + 0u >= 3072 || res->spuThreadPriority + 0u >= 256 || res->ppuThreadStackSize < 4096
		|| type->codecType + 0u >= 0xe)
	{
		return { CELL_VDEC_ERROR_ARG, "memAddr=%d, ppuThreadPriority=%d, spuThreadPriority=%d, ppuThreadStackSize=%d, codecType=%d",
		                              res->memAddr, res->ppuThreadPriority, res->spuThreadPriority, res->ppuThreadStackSize, type->codecType };
	}

	const void* spec = nullptr;

	if constexpr (std::is_same_v<std::decay_t<typename T::type>, CellVdecTypeEx>)
	{
		spec = type->codecSpecificInfo ? type->codecSpecificInfo.get_ptr() : nullptr;
	}

	VdecDecoderAttr attr {};
	const error_code err = get_decoder_specific_ops(type->codecType).query_attr(ppu, attr, type->profileLevel, spec);
	if (err != CELL_OK)
	{
		return err;
	}

	if (attr.mem_size > res->memSize)
	{
		return { CELL_VDEC_ERROR_ARG, "attr.memSize=%d, res->memSize=%d", attr.mem_size, res->memSize };
	}

	// Create decoder context
	shared_ptr<vdec_context> vdec;

	if (std::unique_lock lock{g_fxo->get<hle_locks_t>(), std::try_to_lock})
	{
#ifdef RPCS3_PS4
		// 0.38: built before it gets an ID, so an allocation or FFmpeg failure is returned to the game as
		// CELL_VDEC_ERROR_FATAL (as a failed thread creation below) instead of ending the emulation
		try
		{
			vdec = stx::make_shared<vdec_context>(type->codecType, type->profileLevel, res->memAddr, res->memSize, cb->cbFunc, cb->cbArg);
		}
		catch (const std::bad_alloc&)
		{
		}

		const char* const failure = !vdec ? "out of memory" : vdec->ps4_open_error ? vdec->ps4_open_error :
			idm::import_existing<vdec_context>(vdec) == id_manager::id_traits<vdec_context>::invalid ? "no free decoder ID" : nullptr;

		if (failure)
		{
			s_ps4_vdec_failed_in_row.fetch_add(1, std::memory_order_relaxed);

			if (u32 number = 0; utils::ps4::governor::media_log_due(s_ps4_vdec_open_failures, number))
			{
				const utils::ps4::governor::heap_sample heap = utils::ps4::governor::sample_heap(false);
				const std::string line = fmt::format("VDEC: open failed (%s; codec type %d, memSize %u KiB, heap live %u of %u MiB): CELL_VDEC_ERROR_FATAL returned to the game (open failure %u of this process; the first 8, then one in 100 are logged)",
					failure, type->codecType, res->memSize >> 10, heap.live >> 20, heap.cap >> 20, number);
				cellVdec.error("%s", line);

				if (!s_ps4_vdec_open_failed_noted.exchange(true))
				{
					ps4_startup_checkpoint(line.c_str());
				}
			}

			return { CELL_VDEC_ERROR_FATAL, "%s", failure };
		}
#else
		vdec = idm::make_ptr<vdec_context>(type->codecType, type->profileLevel, res->memAddr, res->memSize, cb->cbFunc, cb->cbArg);
#endif
	}
	else
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	const u32 vid = idm::last_id();

	ensure(vdec);
	vdec->handle = vid;

	// Run thread
	vm::var<u64> _tid;
	vm::var<char[]> _name = vm::make_str("HLE Video Decoder");
#ifdef RPCS3_PS4
	if (const error_code err = ppu_execute<&sys_ppu_thread_create>(ppu, +_tid, 0x10000, vid, +res->ppuThreadPriority, +res->ppuThreadStackSize, SYS_PPU_THREAD_CREATE_INTERRUPT, +_name); err != CELL_OK)
	{
		// 0.29: the decoder thread was not created (CELL_EAGAIN when the console is out of thread resources). Undo the
		// context instead of dereferencing a thread that does not exist (thrd->cmd_list below). The FFmpeg decoder is
		// freed when the last reference ('vdec') goes away. The error report below repeats an identical text at most 3
		// times per thread (then one count at the thread's end), so only the open lines need the rate above.
		idm::remove<vdec_context>(vid);
		s_ps4_vdec_failed_in_row.fetch_add(1, std::memory_order_relaxed);
		return { CELL_VDEC_ERROR_FATAL, "sys_ppu_thread_create() failed (0x%x)", static_cast<u32>(static_cast<s32>(err)) };
	}

	if (const u32 failed = s_ps4_vdec_failed_in_row.exchange(0, std::memory_order_relaxed); failed > 8)
	{
		cellVdec.notice("VDEC: decoder opened after %u failed opens in a row (their open lines were rate limited)", failed);
	}
#else
	ppu_execute<&sys_ppu_thread_create>(ppu, +_tid, 0x10000, vid, +res->ppuThreadPriority, +res->ppuThreadStackSize, SYS_PPU_THREAD_CREATE_INTERRUPT, +_name);
#endif
	*handle = vid;

	const auto thrd = idm::get_unlocked<named_thread<ppu_thread>>(static_cast<u32>(*_tid));

	thrd->cmd_list
	({
		{ ppu_cmd::set_args, 1 }, u64{vid},
		{ ppu_cmd::hle_call, FIND_FUNC(vdecEntry) },
	});

	thrd->state -= cpu_flag::stop;
	thrd->state.notify_one();

	return CELL_OK;
}

error_code cellVdecOpen(ppu_thread& ppu, vm::cptr<CellVdecType> type, vm::cptr<CellVdecResource> res, vm::cptr<CellVdecCb> cb, vm::ptr<u32> handle)
{
#ifdef RPCS3_PS4
	if (ps4_vdec_open_lines_due())
#endif
	cellVdec.warning("cellVdecOpen(type=*0x%x, res=*0x%x, cb=*0x%x, handle=*0x%x)", type, res, cb, handle);

	return vdecOpen(ppu, type, res, cb, handle);
}

error_code cellVdecOpenEx(ppu_thread& ppu, vm::cptr<CellVdecTypeEx> type, vm::cptr<CellVdecResourceEx> res, vm::cptr<CellVdecCb> cb, vm::ptr<u32> handle)
{
#ifdef RPCS3_PS4
	if (ps4_vdec_open_lines_due())
#endif
	cellVdec.warning("cellVdecOpenEx(type=*0x%x, res=*0x%x, cb=*0x%x, handle=*0x%x)", type, res, cb, handle);

	return vdecOpen(ppu, type, res, cb, handle);
}

error_code cellVdecClose(ppu_thread& ppu, u32 handle)
{
	cellVdec.warning("cellVdecClose(handle=0x%x)", handle);

	std::unique_lock lock_hle{g_fxo->get<hle_locks_t>(), std::try_to_lock};

	if (!lock_hle)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec is nullptr" };
	}

	{
		std::lock_guard lock{vdec->mutex};

		if (vdec->seq_state == sequence_state::closed)
		{
			return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
		}
	}

	const u64 seq_id = vdec->seq_id;
	const u64 cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding close cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, cmd_id);

	lv2_obj::sleep(ppu);
	vdec->abort_decode = true;
	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::close, seq_id, cmd_id));

	while (!vdec->ppu_tid)
	{
		thread_ctrl::wait_for(1000);
	}

	const u32 tid = vdec->ppu_tid.exchange(-1);

	if (tid != umax)
	{
		ppu_execute<&sys_interrupt_thread_disestablish>(ppu, tid);
	}

	vdec->seq_state = sequence_state::closed;
	vdec->mutex.lock_unlock();

	if (!idm::remove_verify<vdec_context>(handle, vdec))
	{
		// Other thread removed it beforehead
		return { CELL_VDEC_ERROR_ARG, "remove_verify failed" };
	}

	// Let the reaper free the decoder so that the caller does not pay for it
#ifdef RPCS3_PS4
	// 0.39: lazy service thread (a refused start is tried again at the next close; queued decoders wait for it)
	g_fxo->get<named_thread<vdec_context_reaper>>().ps4_start_lazy();
#endif
	g_fxo->get<named_thread<vdec_context_reaper>>().contexts.push(std::move(vdec));

	return CELL_OK;
}

error_code cellVdecStartSeq(ppu_thread& ppu, u32 handle)
{
	ppu.state += cpu_flag::wait;

	cellVdec.warning("cellVdecStartSeq(handle=0x%x)", handle);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec is nullptr" };
	}

	sequence_state old_state{};

	{
		std::lock_guard lock{vdec->mutex};
		old_state = vdec->seq_state;

		if (old_state != sequence_state::dormant && old_state != sequence_state::ready)
		{
			return { CELL_VDEC_ERROR_SEQ, old_state };
		}

		if (old_state == sequence_state::ready)
		{
			vdec->seq_state = sequence_state::resetting;
		}
	}

	const u64 seq_id = ++vdec->seq_id;
	const u64 cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding start cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, cmd_id);

	vdec->abort_decode = false;
	vdec->is_running = false;
	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::start_sequence, seq_id, cmd_id));

	std::lock_guard lock{vdec->mutex};

	if (false) // TODO: set to old state in case of error
	{
		vdec->seq_state = old_state;
	}
	else
	{
		vdec->seq_state = sequence_state::ready;
	}

	return CELL_OK;
}

error_code cellVdecEndSeq(ppu_thread& ppu, u32 handle)
{
	ppu.state += cpu_flag::wait;

	cellVdec.warning("cellVdecEndSeq(handle=0x%x)", handle);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec is nullptr" };
	}

	{
		std::lock_guard lock{vdec->mutex};

		if (vdec->seq_state != sequence_state::ready)
		{
			return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
		}

		vdec->seq_state = sequence_state::ending;
	}

	const u64 seq_id = vdec->seq_id;

	// Output the pictures still held by the decoder before ending the sequence
	const u64 drain_cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding drain cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, drain_cmd_id);

	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::drain, seq_id, drain_cmd_id));

	const u64 cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding end cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, cmd_id);

	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::end_sequence, seq_id, cmd_id));

	return CELL_OK;
}

error_code cellVdecDecodeAu(ppu_thread& ppu, u32 handle, CellVdecDecodeMode mode, vm::cptr<CellVdecAuInfo> auInfo)
{
	ppu.state += cpu_flag::wait;

	cellVdec.trace("cellVdecDecodeAu(handle=0x%x, mode=%d, auInfo=*0x%x)", handle, +mode, auInfo);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec || !auInfo || !auInfo->size || !auInfo->startAddr)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec=%d, auInfo=%d, size=%d, startAddr=0x%x", !!vdec, !!auInfo, auInfo ? auInfo->size.value() : 0, auInfo ? auInfo->startAddr.value() : 0 };
	}

	{
		std::lock_guard lock{vdec->mutex};

		if (vdec->seq_state != sequence_state::ready)
		{
			return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
		}
	}

	if (mode < 0 || mode > (CELL_VDEC_DEC_MODE_B_SKIP | CELL_VDEC_DEC_MODE_PB_SKIP))
	{
		return { CELL_VDEC_ERROR_ARG, "mode=%d", +mode };
	}

	if ((mode == (CELL_VDEC_DEC_MODE_B_SKIP | CELL_VDEC_DEC_MODE_PB_SKIP) && vdec->type != CELL_VDEC_CODEC_TYPE_VC1) ||
		(mode == CELL_VDEC_DEC_MODE_PB_SKIP && vdec->type != CELL_VDEC_CODEC_TYPE_AVC))
	{
		return { CELL_VDEC_ERROR_ARG, "mode=%d, type=%d", +mode, vdec->type };
	}

	if (!vdec->au_count.try_inc(4))
	{
		return CELL_VDEC_ERROR_BUSY;
	}

	const u64 seq_id = vdec->seq_id;
	const u64 cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding decode cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, cmd_id);

	// TODO: check info
	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::au_decode, seq_id, cmd_id, mode, *auInfo));
	return CELL_OK;
}

error_code cellVdecDecodeAuEx2(ppu_thread& ppu, u32 handle, CellVdecDecodeMode mode, vm::cptr<CellVdecAuInfoEx2> auInfo)
{
	ppu.state += cpu_flag::wait;

	cellVdec.todo("cellVdecDecodeAuEx2(handle=0x%x, mode=%d, auInfo=*0x%x)", handle, +mode, auInfo);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec || !auInfo || !auInfo->size || !auInfo->startAddr)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec=%d, auInfo=%d, size=%d, startAddr=0x%x", !!vdec, !!auInfo, auInfo ? auInfo->size.value() : 0, auInfo ? auInfo->startAddr.value() : 0 };
	}

	{
		std::lock_guard lock{vdec->mutex};

		if (vdec->seq_state != sequence_state::ready)
		{
			return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
		}
	}

	if (mode < 0 || mode > (CELL_VDEC_DEC_MODE_B_SKIP | CELL_VDEC_DEC_MODE_PB_SKIP))
	{
		return { CELL_VDEC_ERROR_ARG, "mode=%d", +mode };
	}

	if ((mode == (CELL_VDEC_DEC_MODE_B_SKIP | CELL_VDEC_DEC_MODE_PB_SKIP) && vdec->type != CELL_VDEC_CODEC_TYPE_VC1) ||
		(mode == CELL_VDEC_DEC_MODE_PB_SKIP && vdec->type != CELL_VDEC_CODEC_TYPE_AVC))
	{
		return { CELL_VDEC_ERROR_ARG, "mode=%d, type=%d", +mode, vdec->type };
	}

	if (!vdec->au_count.try_inc(4))
	{
		return CELL_VDEC_ERROR_BUSY;
	}

	CellVdecAuInfo au_info{};
	au_info.startAddr = auInfo->startAddr;
	au_info.size = auInfo->size;
	au_info.pts = auInfo->pts;
	au_info.dts = auInfo->dts;
	au_info.userData = auInfo->userData;
	au_info.codecSpecificData = auInfo->codecSpecificData;

	const u64 seq_id = vdec->seq_id;
	const u64 cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding decode cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, cmd_id);

	// TODO: check info
	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::au_decode, seq_id, cmd_id, mode, au_info));
	return CELL_OK;
}

#ifdef RPCS3_PS4
// RGB output through utils::ps4::yuv instead of swscale where both give the same bytes: YUV420P, or YUVJ420P (the
// swscale call below converts it as limited-range YUVA420P too), to ARGB/RGBA with even sizes (swscale takes its
// unscaled table converter only for an even height and leaves the last column of an odd width unwritten). No alpha
// plane, memset or swscale context is needed on this path.
static bool ps4_vdec_fast_rgb(u32 format_type, int frame_format, int w, int h)
{
	if ((format_type != CELL_VDEC_PICFMT_ARGB32_ILV && format_type != CELL_VDEC_PICFMT_RGBA32_ILV) ||
		(frame_format != AV_PIX_FMT_YUV420P && frame_format != AV_PIX_FMT_YUVJ420P) || w <= 0 || h <= 0 || (w & 1) || (h & 1))
	{
		return false;
	}

	static const bool use_sws = fs::is_file(ps4_vdec_sws_flag);

	if (use_sws && !s_ps4_vdec_sws_noted.exchange(true))
	{
		ps4_startup_checkpoint("VDEC: vdec-sws-convert.flag: GetPicture RGB conversion through swscale");
	}

	return !use_sws;
}

// Opt-in (vdec-release-slot.flag): the conversion is host work on the game's PPU thread, which otherwise keeps one of
// the few lv2 PPU run slots (ppu_threads) for its whole length. lv2_obj::sleep lets another guest thread run meanwhile;
// the caller re-enters the run queue when this HLE call returns (cpu_sleep -> lv2_obj::awake), as in cellVdecClose.
static void ps4_vdec_release_slot(ppu_thread& ppu, int w, int h)
{
	static const bool release = fs::is_file(ps4_vdec_release_slot_flag);

	if (!release || s64{w} * h < 640 * 360)
	{
		return;
	}

	if (!s_ps4_vdec_release_noted.exchange(true))
	{
		ps4_startup_checkpoint("VDEC: GetPicture releases the PPU run slot during RGB conversions of 640x360 or more (vdec-release-slot.flag)");
	}

	lv2_obj::sleep(ppu);
}
#endif

error_code cellVdecGetPictureExt(ppu_thread& ppu, u32 handle, vm::cptr<CellVdecPicFormat2> format, vm::ptr<u8> outBuff, u32 arg4)
{
	ppu.state += cpu_flag::wait;

	cellVdec.trace("cellVdecGetPictureExt(handle=0x%x, format=*0x%x, outBuff=*0x%x, arg4=*0x%x)", handle, format, outBuff, arg4);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec || !format)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec=%d, format=%d", !!vdec, !!format };
	}

	{
		std::lock_guard lock{vdec->mutex};

		if (vdec->seq_state == sequence_state::closed || vdec->seq_state > sequence_state::ending)
		{
			return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
		}
	}

	if (format->formatType > 4 || (format->formatType <= CELL_VDEC_PICFMT_RGBA32_ILV && format->colorMatrixType > CELL_VDEC_COLOR_MATRIX_TYPE_BT709))
	{
		return {CELL_VDEC_ERROR_ARG, "formatType=%d, colorMatrixType=%d", +format->formatType, +format->colorMatrixType};
	}

	if (arg4 && arg4 != 8 && arg4 != 0xc)
	{
		return { CELL_VDEC_ERROR_ARG, "arg4=0x%x", arg4 };
	}

	if (arg4 || format->unk0 || format->unk1)
	{
		cellVdec.todo("cellVdecGetPictureExt: Unknown arguments (arg4=*0x%x, unk0=0x%x, unk1=0x%x)", arg4, format->unk0, format->unk1);
	}

	vdec_frame frame;
	bool notify = false;
	u64 sequence_id{};

	{
		std::lock_guard lock(vdec->mutex);

		if (vdec->out_queue.empty())
		{
#ifdef RPCS3_PS4
			vdec->ps4_getpic_empty.fetch_add(1, std::memory_order_relaxed);
#endif
			return CELL_VDEC_ERROR_EMPTY;
		}

		frame = std::move(vdec->out_queue.front());
		sequence_id = vdec->seq_id;

		vdec->out_queue.pop_front();
#ifdef RPCS3_PS4
		vdec->ps4_pics_taken.fetch_add(1, std::memory_order_relaxed);
		vdec->ps4_getpic_last_us.store(ps4_vdec_now_us(), std::memory_order_relaxed);
		vdec->ps4_getpic_last_tid.store(ppu.id, std::memory_order_relaxed);

		// The decoder waits while the queue holds ps4_queue_limit pictures
		if (vdec->out_queue.size() + 1 == vdec->ps4_queue_limit.load(std::memory_order_relaxed))
#else
		if (vdec->out_queue.size() + 1 == vdec->out_max)
#endif
			notify = true;
	}

	if (notify)
	{
		auto vdec_ppu = idm::get_unlocked<named_thread<ppu_thread>>(vdec->ppu_tid);
		if (vdec_ppu) thread_ctrl::notify(*vdec_ppu);
	}

	if (sequence_id != frame.seq_id)
	{
		return { CELL_VDEC_ERROR_EMPTY, "sequence_id=%d, seq_id=%d", sequence_id, frame.seq_id };
	}

	if (outBuff)
	{
		const int w = frame->width;
		const int h = frame->height;

		AVPixelFormat out_f = AV_PIX_FMT_YUV420P;

		std::unique_ptr<u8[]> alpha_plane;

#ifdef RPCS3_PS4
		const bool ps4_fast = ps4_vdec_fast_rgb(format->formatType, frame->format, w, h);
#endif

		switch (const u32 type = format->formatType)
		{
#ifdef RPCS3_PS4
		// The console heap is capped at 2 GiB: a failed alpha-plane allocation fails this call (checked below), not the thread
		case CELL_VDEC_PICFMT_ARGB32_ILV: out_f = AV_PIX_FMT_ARGB; if (!ps4_fast) alpha_plane.reset(new (std::nothrow) u8[w * h]); break;
		case CELL_VDEC_PICFMT_RGBA32_ILV: out_f = AV_PIX_FMT_RGBA; if (!ps4_fast) alpha_plane.reset(new (std::nothrow) u8[w * h]); break;
#else
		case CELL_VDEC_PICFMT_ARGB32_ILV: out_f = AV_PIX_FMT_ARGB; alpha_plane.reset(new u8[w * h]); break;
		case CELL_VDEC_PICFMT_RGBA32_ILV: out_f = AV_PIX_FMT_RGBA; alpha_plane.reset(new u8[w * h]); break;
#endif
		case CELL_VDEC_PICFMT_UYVY422_ILV: out_f = AV_PIX_FMT_UYVY422; break;
		case CELL_VDEC_PICFMT_YUV420_PLANAR: out_f = AV_PIX_FMT_YUV420P; break;
		default:
		{
			fmt::throw_exception("cellVdecGetPictureExt: Unknown formatType (handle=0x%x, seq_id=%d, cmd_id=%d, type=%d)", handle, frame.seq_id, frame.cmd_id, type);
		}
		}

#ifdef RPCS3_PS4
		if ((out_f == AV_PIX_FMT_ARGB || out_f == AV_PIX_FMT_RGBA) && !alpha_plane && !ps4_fast)
		{
			return { CELL_VDEC_ERROR_FATAL, "out of memory for the %dx%d alpha plane", w, h };
		}
#endif

		// TODO: color matrix

		if (alpha_plane)
		{
			std::memset(alpha_plane.get(), format->alpha, w * h);
		}

		AVPixelFormat in_f = AV_PIX_FMT_YUV420P;

		switch (frame->format)
		{
		case AV_PIX_FMT_YUVJ420P:
			cellVdec.error("cellVdecGetPictureExt: experimental AVPixelFormat (handle=0x%x, seq_id=%d, cmd_id=%d, format=%d). This may cause suboptimal video quality.", handle, frame.seq_id, frame.cmd_id, frame->format);
			[[fallthrough]];
		case AV_PIX_FMT_YUV420P:
			in_f = alpha_plane ? AV_PIX_FMT_YUVA420P : static_cast<AVPixelFormat>(frame->format);
			break;
		default:
			fmt::throw_exception("cellVdecGetPictureExt: Unknown frame format (%d)", frame->format);
		}

		cellVdec.trace("cellVdecGetPictureExt: handle=0x%x, seq_id=%d, cmd_id=%d, w=%d, h=%d, frameFormat=%d, formatType=%d, in_f=%d, out_f=%d, alpha_plane=%d, alpha=%d, colorMatrixType=%d", handle, frame.seq_id, frame.cmd_id, w, h, frame->format, format->formatType, +in_f, +out_f, !!alpha_plane, format->alpha, format->colorMatrixType);

#ifdef RPCS3_PS4
		const u64 ps4_conv_start = ps4_vdec_now_us();

		if (ps4_fast)
		{
			const bool argb = out_f == AV_PIX_FMT_ARGB;

			if (!s_ps4_vdec_fast_noted.exchange(true))
			{
				const std::string line = fmt::format("VDEC: GetPicture %s -> %s through the PS4 %s converter (%dx%d); vdec-sws-convert.flag restores swscale",
					ps4_vdec_pix_fmt_name(frame->format), argb ? "argb" : "rgba", utils::ps4::yuv::yuv420_to_rgb32_simd ? "SSE" : "scalar", w, h);
				cellVdec.notice("%s", line);
				ps4_startup_checkpoint(line.c_str());
			}

			ps4_vdec_release_slot(ppu, w, h);

			// Same bytes as the swscale call below (pitch w * 4, constant alpha)
			utils::ps4::yuv::yuv420_to_rgb32(frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2],
				outBuff.get_ptr(), static_cast<std::ptrdiff_t>(w) * 4, w, h, argb, format->alpha);

			vdec->ps4_note_conversion(ps4_vdec_now_us() - ps4_conv_start, out_f, true);
			return CELL_OK;
		}
#endif

		vdec->sws = sws_getCachedContext(vdec->sws, w, h, in_f, w, h, out_f, SWS_POINT, nullptr, nullptr, nullptr);

#ifdef RPCS3_PS4
		// sws_getCachedContext frees the old context and returns null when it cannot allocate the new one
		if (!vdec->sws)
		{
			return { CELL_VDEC_ERROR_FATAL, "sws_getCachedContext failed (%dx%d, out of memory?)", w, h };
		}
#endif

		const u8* in_data[4] = { frame->data[0], frame->data[1], frame->data[2], alpha_plane.get() };
		const int in_line[4] = { frame->linesize[0], frame->linesize[1], frame->linesize[2], w * 1 };
		u8* out_data[4] = { outBuff.get_ptr() };
		int out_line[4] = { w * 4 }; // RGBA32 or ARGB32

		// TODO:
		// It's possible that we need to align the pitch to 128 here.
		// PS HOME seems to rely on this somehow in certain cases.

		if (!alpha_plane)
		{
			// YUV420P or UYVY422
			out_data[1] = out_data[0] + w * h;
			out_data[2] = out_data[0] + w * h * 5 / 4;

			if (const int ret = av_image_fill_linesizes(out_line, out_f, w); ret < 0)
			{
				fmt::throw_exception("cellVdecGetPictureExt: av_image_fill_linesizes failed (handle=0x%x, seq_id=%d, cmd_id=%d, ret=0x%x): %s", handle, frame.seq_id, frame.cmd_id, ret, utils::av_error_to_string(ret));
			}
		}

#ifdef RPCS3_PS4
		// RGB output (the slow swscale path); planar and UYVY output is a row copy
		if (alpha_plane)
		{
			ps4_vdec_release_slot(ppu, w, h);
		}
#endif

		sws_scale(vdec->sws, in_data, in_line, 0, h, out_data, out_line);

#ifdef RPCS3_PS4
		vdec->ps4_note_conversion(ps4_vdec_now_us() - ps4_conv_start, out_f, false);
#endif
	}

	return CELL_OK;
}

error_code cellVdecGetPicture(ppu_thread& ppu, u32 handle, vm::cptr<CellVdecPicFormat> format, vm::ptr<u8> outBuff)
{
	ppu.state += cpu_flag::wait;

	cellVdec.trace("cellVdecGetPicture(handle=0x%x, format=*0x%x, outBuff=*0x%x)", handle, format, outBuff);

	if (!format)
	{
		return { CELL_VDEC_ERROR_ARG, "format is nullptr" };
	}

	vm::var<CellVdecPicFormat2> format2;
	format2->formatType = format->formatType;
	format2->colorMatrixType = format->colorMatrixType;
	format2->alpha = format->alpha;
	format2->unk0 = 0;
	format2->unk1 = 0;

	return cellVdecGetPictureExt(ppu, handle, format2, outBuff, 0);
}

error_code cellVdecGetPicItem(ppu_thread& ppu, u32 handle, vm::pptr<CellVdecPicItem> picItem)
{
	ppu.state += cpu_flag::wait;

	cellVdec.trace("cellVdecGetPicItem(handle=0x%x, picItem=**0x%x)", handle, picItem);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec || !picItem)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec=%d, picItem=%d", !!vdec, !!picItem };
	}

	u64 sequence_id{};

	{
		std::lock_guard lock{vdec->mutex};

		if (vdec->seq_state == sequence_state::closed || vdec->seq_state > sequence_state::ending)
		{
			return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
		}

		sequence_id = vdec->seq_id;
	}

	struct all_info_t
	{
		CellVdecPicItem picItem;
		union
		{
			CellVdecAvcInfo avcInfo;
			CellVdecDivxInfo divxInfo;
			CellVdecMpeg2Info mpeg2Info;
		} picInfo;
	};

	s32 pict_type = AV_PICTURE_TYPE_NONE;
	s32 frame_width{};
	s32 frame_height{};
	u64 seq_id{};
	u64 cmd_id{};
	u64 pts{};
	u64 dts{};
	u64 usrd{};
	u32 frc = 0;
	CellVdecPicAttr attr = CELL_VDEC_PICITEM_ATTR_NORMAL;
	vm::ptr<CellVdecPicItem> info = vm::null;
	{
		std::lock_guard lock(vdec->mutex);

		for (auto& picture : vdec->out_queue)
		{
			if (!picture.pic_item_received)
			{
				picture.pic_item_received = true;
				ensure(picture.avf);
				pict_type = picture.avf->pict_type;
				frame_width = picture.avf->width;
				frame_height = picture.avf->height;
				seq_id = picture.seq_id;
				cmd_id = picture.cmd_id;
				pts = picture.pts;
				dts = picture.dts;
				usrd = picture.userdata;
				frc = picture.frc;
				attr = picture.attr;
				info.set(vdec->mem_addr + vdec->mem_bias);

				constexpr u64 size_needed = sizeof(all_info_t);

				if (vdec->mem_bias + size_needed >= vdec->mem_size / size_needed * size_needed)
				{
					vdec->mem_bias = 0;
					break;
				}

				vdec->mem_bias += size_needed;
				break;
			}
		}
	}

	if (!info || seq_id != sequence_id)
	{
		return { CELL_VDEC_ERROR_EMPTY, " info=%d, sequence_id=%d, seq_id=%d", !!info, sequence_id, seq_id };
	}

	info->codecType = vdec->type;
	info->startAddr = 0x00000123; // invalid value (no address for picture)
	const int buffer_size = av_image_get_buffer_size(vdec->ctx->pix_fmt, vdec->ctx->width, vdec->ctx->height, 1);
	ensure(buffer_size >= 0);
	info->size = utils::align<u32>(buffer_size, 128);
	info->auNum = 1;
	info->auPts[0].lower = static_cast<u32>(pts);
	info->auPts[0].upper = static_cast<u32>(pts >> 32);
	info->auPts[1].lower = CELL_CODEC_PTS_INVALID;
	info->auPts[1].upper = CELL_CODEC_PTS_INVALID;
	info->auDts[0].lower = static_cast<u32>(dts);
	info->auDts[0].upper = static_cast<u32>(dts >> 32);
	info->auDts[1].lower = CELL_CODEC_DTS_INVALID;
	info->auDts[1].upper = CELL_CODEC_DTS_INVALID;
	info->auUserData[0] = usrd;
	info->auUserData[1] = 0;
	info->status = CELL_OK;
	info->attr = attr;

	const vm::addr_t picinfo_addr{info.addr() + ::offset32(&all_info_t::picInfo)};
	info->picInfo_addr = picinfo_addr;

	if (vdec->type == CELL_VDEC_CODEC_TYPE_AVC)
	{
		const vm::ptr<CellVdecAvcInfo> avc = picinfo_addr;

		avc->horizontalSize = frame_width;
		avc->verticalSize = frame_height;

		switch (pict_type)
		{
		case AV_PICTURE_TYPE_I: avc->pictureType[0] = CELL_VDEC_AVC_PCT_I; break;
		case AV_PICTURE_TYPE_P: avc->pictureType[0] = CELL_VDEC_AVC_PCT_P; break;
		case AV_PICTURE_TYPE_B: avc->pictureType[0] = CELL_VDEC_AVC_PCT_B; break;
		default:
		{
			avc->pictureType[0] = CELL_VDEC_AVC_PCT_UNKNOWN;
			cellVdec.error("cellVdecGetPicItem(AVC): unknown pict_type value (handle=0x%x, seq_id=%d, cmd_id=%d, pct=0x%x)", handle, seq_id, cmd_id, pict_type);
			break;
		}
		}

		avc->pictureType[1] = CELL_VDEC_AVC_PCT_UNKNOWN; // ???
		avc->idrPictureFlag = false; // ???
		avc->aspect_ratio_idc = CELL_VDEC_AVC_ARI_SAR_UNSPECIFIED; // ???
		avc->sar_height = 0;
		avc->sar_width = 0;
		avc->pic_struct = CELL_VDEC_AVC_PSTR_FRAME; // ???
		avc->picOrderCount[0] = 0; // ???
		avc->picOrderCount[1] = 0;
		avc->vui_parameters_present_flag = true; // ???
		avc->frame_mbs_only_flag = true; // ??? progressive
		avc->video_signal_type_present_flag = true; // ???
		avc->video_format = CELL_VDEC_AVC_VF_COMPONENT; // ???
		avc->video_full_range_flag = false; // ???
		avc->colour_description_present_flag = true;
		avc->colour_primaries = CELL_VDEC_AVC_CP_ITU_R_BT_709_5; // ???
		avc->transfer_characteristics = CELL_VDEC_AVC_TC_ITU_R_BT_709_5;
		avc->matrix_coefficients = CELL_VDEC_AVC_MXC_ITU_R_BT_709_5; // important
		avc->timing_info_present_flag = true;

		switch (frc)
		{
		case CELL_VDEC_FRC_24000DIV1001: avc->frameRateCode = CELL_VDEC_AVC_FRC_24000DIV1001; break;
		case CELL_VDEC_FRC_24: avc->frameRateCode = CELL_VDEC_AVC_FRC_24; break;
		case CELL_VDEC_FRC_25: avc->frameRateCode = CELL_VDEC_AVC_FRC_25; break;
		case CELL_VDEC_FRC_30000DIV1001: avc->frameRateCode = CELL_VDEC_AVC_FRC_30000DIV1001; break;
		case CELL_VDEC_FRC_30: avc->frameRateCode = CELL_VDEC_AVC_FRC_30; break;
		case CELL_VDEC_FRC_50: avc->frameRateCode = CELL_VDEC_AVC_FRC_50; break;
		case CELL_VDEC_FRC_60000DIV1001: avc->frameRateCode = CELL_VDEC_AVC_FRC_60000DIV1001; break;
		case CELL_VDEC_FRC_60: avc->frameRateCode = CELL_VDEC_AVC_FRC_60; break;
		default: cellVdec.error("cellVdecGetPicItem(AVC): unknown frc value (handle=0x%x, seq_id=%d, cmd_id=%d, frc=0x%x)", handle, seq_id, cmd_id, frc);
		}

		avc->fixed_frame_rate_flag = true;
		avc->low_delay_hrd_flag = true; // ???
		avc->entropy_coding_mode_flag = true; // ???
		avc->nalUnitPresentFlags = 0; // ???
		avc->ccDataLength[0] = 0;
		avc->ccDataLength[1] = 0;
		avc->reserved[0] = 0;
		avc->reserved[1] = 0;
	}
	// TODO: handle MPEG4 properly
	else if (vdec->type == CELL_VDEC_CODEC_TYPE_MPEG4 || vdec->type == CELL_VDEC_CODEC_TYPE_DIVX)
	{
		const vm::ptr<CellVdecDivxInfo> dvx = picinfo_addr;

		switch (pict_type)
		{
		case AV_PICTURE_TYPE_I: dvx->pictureType = CELL_VDEC_DIVX_VCT_I; break;
		case AV_PICTURE_TYPE_P: dvx->pictureType = CELL_VDEC_DIVX_VCT_P; break;
		case AV_PICTURE_TYPE_B: dvx->pictureType = CELL_VDEC_DIVX_VCT_B; break;
		default: cellVdec.error("cellVdecGetPicItem(DivX): unknown pict_type value (handle=0x%x, seq_id=%d, cmd_id=%d, pct=0x%x)", handle, seq_id, cmd_id, pict_type);
		}

		dvx->horizontalSize = frame_width;
		dvx->verticalSize = frame_height;
		dvx->pixelAspectRatio = CELL_VDEC_DIVX_ARI_PAR_1_1; // ???
		dvx->parHeight = 0;
		dvx->parWidth = 0;
		dvx->colourDescription = false; // ???
		dvx->colourPrimaries = CELL_VDEC_DIVX_CP_ITU_R_BT_709; // ???
		dvx->transferCharacteristics = CELL_VDEC_DIVX_TC_ITU_R_BT_709; // ???
		dvx->matrixCoefficients = CELL_VDEC_DIVX_MXC_ITU_R_BT_709; // ???
		dvx->pictureStruct = CELL_VDEC_DIVX_PSTR_FRAME; // ???

		switch (frc)
		{
		case CELL_VDEC_FRC_24000DIV1001: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_24000DIV1001; break;
		case CELL_VDEC_FRC_24: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_24; break;
		case CELL_VDEC_FRC_25: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_25; break;
		case CELL_VDEC_FRC_30000DIV1001: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_30000DIV1001; break;
		case CELL_VDEC_FRC_30: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_30; break;
		case CELL_VDEC_FRC_50: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_50; break;
		case CELL_VDEC_FRC_60000DIV1001: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_60000DIV1001; break;
		case CELL_VDEC_FRC_60: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_60; break;
		default: cellVdec.error("cellVdecGetPicItem(DivX): unknown frc value (handle=0x%x, seq_id=%d, cmd_id=%d, frc=0x%x)", handle, seq_id, cmd_id, frc);
		}
	}
	else if (vdec->type == CELL_VDEC_CODEC_TYPE_MPEG2)
	{
		const vm::ptr<CellVdecMpeg2Info> mp2 = picinfo_addr;

		std::memset(mp2.get_ptr(), 0, sizeof(CellVdecMpeg2Info));
		mp2->horizontal_size = frame_width;
		mp2->vertical_size = frame_height;
		mp2->aspect_ratio_information = CELL_VDEC_MPEG2_ARI_SAR_1_1; // ???

		switch (frc)
		{
		case CELL_VDEC_FRC_24000DIV1001: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_24000DIV1001; break;
		case CELL_VDEC_FRC_24: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_24; break;
		case CELL_VDEC_FRC_25: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_25; break;
		case CELL_VDEC_FRC_30000DIV1001: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_30000DIV1001; break;
		case CELL_VDEC_FRC_30: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_30; break;
		case CELL_VDEC_FRC_50: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_50; break;
		case CELL_VDEC_FRC_60000DIV1001: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_60000DIV1001; break;
		case CELL_VDEC_FRC_60: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_60; break;
		default: cellVdec.error("cellVdecGetPicItem(MPEG2): unknown frc value (handle=0x%x, seq_id=%d, cmd_id=%d, frc=0x%x)", handle, seq_id, cmd_id, frc);
		}

		mp2->progressive_sequence = true; // ???
		mp2->low_delay = true; // ???
		mp2->video_format = CELL_VDEC_MPEG2_VF_UNSPECIFIED; // ???
		mp2->colour_description = false; // ???

		switch (pict_type)
		{
		case AV_PICTURE_TYPE_I: mp2->picture_coding_type[0] = CELL_VDEC_MPEG2_PCT_I; break;
		case AV_PICTURE_TYPE_P: mp2->picture_coding_type[0] = CELL_VDEC_MPEG2_PCT_P; break;
		case AV_PICTURE_TYPE_B: mp2->picture_coding_type[0] = CELL_VDEC_MPEG2_PCT_B; break;
		default: cellVdec.error("cellVdecGetPicItem(MPEG2): unknown pict_type value (handle=0x%x, seq_id=%d, cmd_id=%d, pct=0x%x)", handle, seq_id, cmd_id, pict_type);
		}

		mp2->picture_coding_type[1] = CELL_VDEC_MPEG2_PCT_FORBIDDEN; // ???
		mp2->picture_structure[0] = CELL_VDEC_MPEG2_PSTR_FRAME;
		mp2->picture_structure[1] = CELL_VDEC_MPEG2_PSTR_FRAME;

		// ...
	}

	*picItem = info;
	return CELL_OK;
}

error_code cellVdecSetFrameRate(u32 handle, CellVdecFrameRate frameRateCode)
{
	cellVdec.trace("cellVdecSetFrameRate(handle=0x%x, frameRateCode=0x%x)", handle, +frameRateCode);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	// 0x80 seems like a common prefix
	if (!vdec || (frameRateCode & 0xf8) != 0x80)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec=%d, frameRateCode=0x%x", !!vdec, +frameRateCode };
	}

	std::lock_guard lock{vdec->mutex};

	if (vdec->seq_state == sequence_state::closed || vdec->seq_state >= sequence_state::invalid)
	{
		return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
	}

	const u64 seq_id = vdec->seq_id;
	const u64 cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding framerate cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, cmd_id);

	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::framerate, seq_id, cmd_id, frameRateCode & 0x87));
	return CELL_OK;
}

error_code cellVdecOpenExt(ppu_thread& ppu, vm::cptr<CellVdecType> type, vm::cptr<CellVdecResourceExt> res, vm::cptr<CellVdecCb> cb, vm::ptr<u32> handle)
{
#ifdef RPCS3_PS4
	if (ps4_vdec_open_lines_due())
#endif
	cellVdec.warning("cellVdecOpenExt(type=*0x%x, res=*0x%x, cb=*0x%x, handle=*0x%x)", type, res, cb, handle);

	if (!res)
	{
		return { CELL_VDEC_ERROR_ARG, "res is nullptr" };
	}

	vm::var<CellVdecResource> tmp = vm::make_var<CellVdecResource>({});
	tmp->memAddr = res->memAddr;
	tmp->memSize = res->memSize;
	tmp->ppuThreadPriority = res->ppuThreadPriority;
	tmp->ppuThreadStackSize = res->ppuThreadStackSize;
	tmp->spuThreadPriority = 0;
	tmp->numOfSpus = res->numOfSpus;

	const vm::ptr<CellVdecResource> ptr = vm::cast(tmp.addr());
	return vdecOpen(ppu, type, ptr, cb, handle);
}

error_code cellVdecStartSeqExt()
{
	UNIMPLEMENTED_FUNC(cellVdec);
	return CELL_OK;
}

error_code cellVdecGetInputStatus()
{
	UNIMPLEMENTED_FUNC(cellVdec);
	return CELL_OK;
}

error_code cellVdecGetPicItemEx()
{
	UNIMPLEMENTED_FUNC(cellVdec);
	return CELL_OK;
}

error_code cellVdecGetPicItemExt()
{
	UNIMPLEMENTED_FUNC(cellVdec);
	return CELL_OK;
}

error_code cellVdecSetFrameRateExt()
{
	UNIMPLEMENTED_FUNC(cellVdec);
	return CELL_OK;
}

error_code cellVdecSetPts(u32 handle, vm::ptr<void> unk)
{
	cellVdec.error("cellVdecSetPts(handle=0x%x, unk=*0x%x)", handle, unk);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec || !unk)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec=%d, unk=%d", !!vdec, !!unk };
	}

	std::lock_guard lock{vdec->mutex};

	if (vdec->seq_state == sequence_state::closed || vdec->seq_state >= sequence_state::invalid)
	{
		return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
	}

	return CELL_OK;
}

DECLARE(ppu_module_manager::cellVdec)("libvdec", []()
{
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
	avcodec_register_all();
#endif

	REG_VAR(libvdec, _cell_vdec_prx_ver); // 0x085a7ecb

	REG_FUNC(libvdec, cellVdecQueryAttr);
	REG_FUNC(libvdec, cellVdecQueryAttrEx);
	REG_FUNC(libvdec, cellVdecOpen);
	REG_FUNC(libvdec, cellVdecOpenEx);
	REG_FUNC(libvdec, cellVdecOpenExt); // 0xef4d8ad7
	REG_FUNC(libvdec, cellVdecClose);
	REG_FUNC(libvdec, cellVdecStartSeq);
	REG_FUNC(libvdec, cellVdecStartSeqExt); // 0xebb8e70a
	REG_FUNC(libvdec, cellVdecEndSeq);
	REG_FUNC(libvdec, cellVdecDecodeAu);
	REG_FUNC(libvdec, cellVdecDecodeAuEx2);
	REG_FUNC(libvdec, cellVdecGetInputStatus);
	REG_FUNC(libvdec, cellVdecGetPicture);
	REG_FUNC(libvdec, cellVdecGetPictureExt); // 0xa21aa896
	REG_FUNC(libvdec, cellVdecGetPicItem);
	REG_FUNC(libvdec, cellVdecGetPicItemEx);
	REG_FUNC(libvdec, cellVdecGetPicItemExt); // 0x2cbd9806
	REG_FUNC(libvdec, cellVdecSetFrameRate);
	REG_FUNC(libvdec, cellVdecSetFrameRateExt); // 0xcffc42a5
	REG_FUNC(libvdec, cellVdecSetPts); // 0x3ce2e4f8

	REG_HIDDEN_FUNC(vdecEntry);
});
