#include "stdafx.h"
#include "Emu/perf_meter.hpp"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/lv2/sys_ppu_thread.h"
#include "Emu/savestate_utils.hpp"
#include "sysPrxForUser.h"
#include "util/asm.hpp"
#include "util/media_utils.h"

#include "cellAtracXdec.h"

#ifdef RPCS3_PS4
#include "util/ps4_hle_memory.hpp"

#include <atomic>
#include <chrono>
#endif

vm::gvar<CellAdecCoreOps> g_cell_adec_core_ops_atracx2ch;
vm::gvar<CellAdecCoreOps> g_cell_adec_core_ops_atracx6ch;
vm::gvar<CellAdecCoreOps> g_cell_adec_core_ops_atracx8ch;
vm::gvar<CellAdecCoreOps> g_cell_adec_core_ops_atracx;

LOG_CHANNEL(cellAtracXdec);

#ifdef RPCS3_PS4
// ATRAC3plus decode cost of the whole process: FFmpeg runs on each decoder's PPU thread, inside the guest scheduler's
// run slots. One RPCS3.log line per 60 s says how much of that time the decoders take. Measurement only.
static std::atomic<u64> s_ps4_atx_aus{0};
static std::atomic<u64> s_ps4_atx_total_us{0};
static std::atomic<u64> s_ps4_atx_max_us{0};
static std::atomic<u64> s_ps4_atx_window_start_us{0};
static std::atomic<s32> s_ps4_atx_decoders{0};

static u64 ps4_atx_now_us()
{
	return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void ps4_atx_note_decode(u64 took_us)
{
	s_ps4_atx_aus.fetch_add(1, std::memory_order_relaxed);
	s_ps4_atx_total_us.fetch_add(took_us, std::memory_order_relaxed);

	for (u64 max = s_ps4_atx_max_us.load(std::memory_order_relaxed); max < took_us && !s_ps4_atx_max_us.compare_exchange_weak(max, took_us, std::memory_order_relaxed);)
	{
	}

	const u64 now = ps4_atx_now_us();
	u64 start = s_ps4_atx_window_start_us.load(std::memory_order_relaxed);

	if (!start)
	{
		s_ps4_atx_window_start_us.compare_exchange_strong(start, now, std::memory_order_relaxed);
		return;
	}

	// One decoder thread closes the window; the others keep counting into the next one
	if (now - start < 60'000'000 || !s_ps4_atx_window_start_us.compare_exchange_strong(start, now, std::memory_order_relaxed))
	{
		return;
	}

	const u64 aus = s_ps4_atx_aus.exchange(0, std::memory_order_relaxed);
	const u64 total_us = s_ps4_atx_total_us.exchange(0, std::memory_order_relaxed);
	const u64 max_us = s_ps4_atx_max_us.exchange(0, std::memory_order_relaxed);

	cellAtracXdec.notice("ATRAC3plus: %.1f AUs/s, decode avg %u us max %u us, %d decoders", aus * 1e6 / static_cast<f64>(now - start), aus ? total_us / aus : 0, max_us,
		s_ps4_atx_decoders.load(std::memory_order_relaxed));
}

static std::atomic<bool> s_ps4_atx_oom_noted{false};

static void ps4_atx_note_out_of_memory(u64 bytes)
{
	utils::ps4::hle_memory::note_out_of_memory(s_ps4_atx_oom_noted, "cellAtracXdec", bytes);
}
#endif

template <>
void fmt_class_string<CellAtracXdecError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](CellAtracXdecError value)
	{
		switch (value)
		{
		STR_CASE(CELL_ADEC_ERROR_ATX_OK); // CELL_ADEC_ERROR_ATX_OFFSET, CELL_ADEC_ERROR_ATX_NONE
		STR_CASE(CELL_ADEC_ERROR_ATX_BUSY);
		STR_CASE(CELL_ADEC_ERROR_ATX_EMPTY);
		STR_CASE(CELL_ADEC_ERROR_ATX_ATSHDR);
		STR_CASE(CELL_ADEC_ERROR_ATX_NON_FATAL);
		STR_CASE(CELL_ADEC_ERROR_ATX_NOT_IMPLE);
		STR_CASE(CELL_ADEC_ERROR_ATX_PACK_CE_OVERFLOW);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILLEGAL_NPROCQUS);
		STR_CASE(CELL_ADEC_ERROR_ATX_FATAL);
		STR_CASE(CELL_ADEC_ERROR_ATX_ENC_OVERFLOW);
		STR_CASE(CELL_ADEC_ERROR_ATX_PACK_CE_UNDERFLOW);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDCT);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GAINADJ);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDSF);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_SPECTRA);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDWL);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GHWAVE);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_SHEADER);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDWL_A);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDWL_B);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDWL_C);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDWL_D);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDWL_E);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDSF_A);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDSF_B);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDSF_C);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDSF_D);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDCT_A);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GC_NGC);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GC_IDLEV_A);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GC_IDLOC_A);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GC_IDLEV_B);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GC_IDLOC_B);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_SN_NWVS);
		STR_CASE(CELL_ADEC_ERROR_ATX_FATAL_HANDLE);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_SAMPLING_FREQ);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_CH_CONFIG_INDEX);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_NBYTES);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_BLOCK_NUM);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_BLOCK_ID);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_CHANNELS);
		STR_CASE(CELL_ADEC_ERROR_ATX_UNINIT_BLOCK_SPECIFIED);
		STR_CASE(CELL_ADEC_ERROR_ATX_POSCFG_PRESENT);
		STR_CASE(CELL_ADEC_ERROR_ATX_BUFFER_OVERFLOW);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILL_BLK_TYPE_ID);
		STR_CASE(CELL_ADEC_ERROR_ATX_UNPACK_CHANNEL_BLK_FAILED);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILL_BLK_ID_USED_1);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILL_BLK_ID_USED_2);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILLEGAL_ENC_SETTING);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILLEGAL_DEC_SETTING);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_NSAMPLES);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILL_SYNCWORD);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILL_SAMPLING_FREQ);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILL_CH_CONFIG_INDEX);
		STR_CASE(CELL_ADEC_ERROR_ATX_RAW_DATA_FRAME_SIZE_OVER);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_ENHANCE_LENGTH_OVER);
		STR_CASE(CELL_ADEC_ERROR_ATX_SPU_INTERNAL_FAIL);
		}

		return unknown;
	});
}

constexpr u32 atracXdecGetSpursMemSize(u32 nch_in)
{
	switch (nch_in)
	{
	case 1: return 0x6000;
	case 2: return 0x6000;
	case 3: return 0x12880;
	case 4: return 0x19c80;
	case 5: return -1;
	case 6: return 0x23080;
	case 7: return 0x2a480;
	case 8: return 0x2c480;
	default: return -1;
	}
}

void AtracXdecDecoder::alloc_avcodec()
{
#ifdef RPCS3_PS4
	// 0.38: an allocation FFmpeg cannot get leaves the pointer null; the decode retries it at the next access unit and
	// reports CELL_ADEC_ERROR_ATX_FATAL meanwhile. Only what is missing is allocated (no leak on the retries); the
	// thread entry clears the pointers first, like upstream overwrote them.
	codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
	if (!codec)
	{
		fmt::throw_exception("avcodec_find_decoder() failed");
	}

	if (!packet)
	{
		packet = av_packet_alloc();
	}

	if (!frame)
	{
		frame = av_frame_alloc();
	}

	if (!packet || !frame)
	{
		ps4_atx_note_out_of_memory(packet ? sizeof(AVFrame) : sizeof(AVPacket));
	}
#else
	codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
	if (!codec)
	{
		fmt::throw_exception("avcodec_find_decoder() failed");
	}

	packet = av_packet_alloc();
	if (!packet)
	{
		fmt::throw_exception("av_packet_alloc() failed");
	}

	frame = av_frame_alloc();
	if (!frame)
	{
		fmt::throw_exception("av_frame_alloc() failed");
	}
#endif
}

void AtracXdecDecoder::free_avcodec()
{
	if (packet)
	{
		av_packet_free(&packet);
	}
	if (frame)
	{
		av_frame_free(&frame);
	}
	if (ctx)
	{
		avcodec_free_context(&ctx);
	}
}

void AtracXdecDecoder::init_avcodec()
{
	if (ctx)
	{
		avcodec_free_context(&ctx);
	}

#ifdef RPCS3_PS4
	// 0.38: ctx stays null when FFmpeg runs out of memory (see alloc_avcodec)
	if (!codec || !packet || !frame)
	{
		return;
	}

	ctx = avcodec_alloc_context3(codec);
	if (!ctx)
	{
		ps4_atx_note_out_of_memory(sizeof(AVCodecContext));
		return;
	}
#else
	ctx = avcodec_alloc_context3(codec);
	if (!ctx)
	{
		fmt::throw_exception("avcodec_alloc_context3() failed");
	}
#endif

	// Allows FFmpeg to output directly into guest memory
	ctx->opaque = this;
	ctx->thread_type = FF_THREAD_SLICE; // Silences a warning by FFmpeg about requesting frame threading with a custom get_buffer2(). Default is FF_THREAD_FRAME & FF_THREAD_SLICE
	ctx->get_buffer2 = [](AVCodecContext* s, AVFrame* frame, int /*flags*/) -> int
	{
		for (s32 i = 0; i < frame->ch_layout.nb_channels; i++)
		{
			frame->data[i] = static_cast<AtracXdecDecoder*>(s->opaque)->work_mem.get_ptr() + ATXDEC_MAX_FRAME_LENGTH + ATXDEC_SAMPLES_PER_FRAME * sizeof(f32) * i;
			frame->linesize[i] = ATXDEC_SAMPLES_PER_FRAME * sizeof(f32);
		}

		frame->buf[0] = av_buffer_create(frame->data[0], ATXDEC_SAMPLES_PER_FRAME * sizeof(f32) * frame->ch_layout.nb_channels, [](void*, uint8_t*){}, nullptr, 0);
#ifdef RPCS3_PS4
		if (!frame->buf[0])
		{
			return AVERROR(ENOMEM);
		}
#endif
		return 0;
	};
	ctx->block_align = nbytes;
	ctx->ch_layout.nb_channels = nch_in;
	ctx->sample_rate = sampling_freq;

	if (int err = avcodec_open2(ctx, codec, nullptr); err)
	{
#ifdef RPCS3_PS4
		if (err == AVERROR(ENOMEM))
		{
			avcodec_free_context(&ctx);
			ps4_atx_note_out_of_memory(0);
			return;
		}
#endif
		fmt::throw_exception("avcodec_open2() failed (err=0x%x='%s')", err, utils::av_error_to_string(err));
	}

#ifdef RPCS3_PS4
	// The previous sequence's reference (no-op free callback) is dropped instead of leaked
	av_buffer_unref(&packet->buf);
#endif
	packet->data = work_mem.get_ptr();
	packet->size = nbytes;
	packet->buf = av_buffer_create(work_mem.get_ptr(), nbytes, [](void*, uint8_t*){}, nullptr, 0);
}

error_code AtracXdecDecoder::set_config_info(u32 sampling_freq, u32 ch_config_idx, u32 nbytes)
{
	cellAtracXdec.notice("AtracXdecDecoder::set_config_info(sampling_freq=%d, ch_config_idx=%d, nbytes=0x%x)", sampling_freq, ch_config_idx, nbytes);

	this->sampling_freq = sampling_freq;
	this->ch_config_idx = ch_config_idx;
	this->nbytes = nbytes;
	this->nbytes_128_aligned = utils::align(nbytes, 0x80);
	this->nch_in = ch_config_idx <= 4 ? ch_config_idx : ch_config_idx + 1;

	if (ch_config_idx > 7u)
	{
		this->config_is_set = false;
		return { 0x80004005, "AtracXdecDecoder::set_config_info() failed: Invalid channel configuration: %d", ch_config_idx };
	}

	this->nch_blocks = ATXDEC_NCH_BLOCKS_MAP[ch_config_idx];

	// These checks are performed on the LLE SPU thread
	if (ch_config_idx == 0u)
	{
		this->config_is_set = false;
		return { 0x80004005, "AtracXdecDecoder::set_config_info() failed: Invalid channel configuration: %d", ch_config_idx };
	}

	if (sampling_freq != 48000u && sampling_freq != 44100u) // 32kHz is not supported, even though official docs claim it is
	{
		this->config_is_set = false;
		return { 0x80004005, "AtracXdecDecoder::set_config_info() failed: Invalid sample rate: %d", sampling_freq };
	}

	if (nbytes == 0u || nbytes > ATXDEC_MAX_FRAME_LENGTH)
	{
		this->config_is_set = false;
		return { 0x80004005, "AtracXdecDecoder::set_config_info() failed: Invalid frame length: 0x%x", nbytes };
	}

	this->config_is_set = true;
	return CELL_OK;
}

error_code AtracXdecDecoder::init_decode(u32 bw_pcm, u32 nch_out)
{
	if (bw_pcm < CELL_ADEC_ATRACX_WORD_SZ_16BIT || (bw_pcm > CELL_ADEC_ATRACX_WORD_SZ_32BIT && bw_pcm != CELL_ADEC_ATRACX_WORD_SZ_FLOAT))
	{
		return { 0x80004005, "AtracXdecDecoder::init_decode() failed: Invalid PCM output format" };
	}

	this->bw_pcm = bw_pcm;
	this->nch_out = nch_out; // Not checked for invalid values on LLE
	this->pcm_output_size = (bw_pcm == CELL_ADEC_ATRACX_WORD_SZ_16BIT ? sizeof(s16) : sizeof(f32)) * nch_in * ATXDEC_SAMPLES_PER_FRAME;

	init_avcodec();
	return CELL_OK;
}

error_code AtracXdecDecoder::parse_ats_header(vm::cptr<u8> au_start_addr)
{
	const auto ats = std::bit_cast<AtracXdecAtsHeader>(vm::read64(au_start_addr.addr()));

	if (ats.sync_word != 0x0fd0)
	{
		return { CELL_ADEC_ERROR_ATX_ATSHDR, "AtracXdecDecoder::parse_ats_header() failed: Invalid sync word: 0x%x", ats.sync_word };
	}

	const u8 sample_rate_idx = ats.params >> 13;
	const u8 ch_config_idx = ats.params >> 10 & 7;
	const u16 nbytes = ((ats.params & 0x3ff) + 1) * 8;

	if (ch_config_idx == 0u)
	{
		return { CELL_ADEC_ERROR_ATX_ATSHDR, "AtracXdecDecoder::parse_ats_header() failed: Invalid channel configuration: %d", ch_config_idx };
	}

	u32 sampling_freq;
	switch (sample_rate_idx)
	{
	case 1: sampling_freq = 44100; break;
	case 2: sampling_freq = 48000; break;
	default: return { CELL_ADEC_ERROR_ATX_ATSHDR, "AtracXdecDecoder::parse_ats_header() failed: Invalid sample rate index: %d", sample_rate_idx };
	}

	return set_config_info(sampling_freq, ch_config_idx, nbytes); // Cannot return error here, values were already checked
}

void AtracXdecContext::exec(ppu_thread& ppu)
{
	perf_meter<"ATXDEC"_u64> perf0;

	// Savestates
	if (decoder.config_is_set)
	{
		decoder.init_avcodec();
	}

	switch (savestate)
	{
	case atracxdec_state::initial: break;
	case atracxdec_state::waiting_for_cmd: goto label1_wait_for_cmd_state;
	case atracxdec_state::checking_run_thread_1: goto label2_check_run_thread_1_state;
	case atracxdec_state::executing_cmd: goto label3_execute_cmd_state;
	case atracxdec_state::waiting_for_output: goto label4_wait_for_output_state;
	case atracxdec_state::checking_run_thread_2: goto label5_check_run_thread_2_state;
	case atracxdec_state::decoding: goto label6_decode_state;
	}

	for (;;cmd_counter++)
	{
		cellAtracXdec.trace("Command counter: %llu, waiting for next command...", cmd_counter);

		for (;;)
		{
			savestate = atracxdec_state::initial;

			ensure(lv2_syscall<sys_mutex_lock>(ppu, queue_mutex, 0) == CELL_OK);

			if (ppu.state & cpu_flag::again)
			{
				return;
			}

			if (!cmd_queue.empty())
			{
				break;
			}

			savestate = atracxdec_state::waiting_for_cmd;
			label1_wait_for_cmd_state:

			ensure(lv2_syscall<sys_cond_wait>(ppu, queue_not_empty, 0) == CELL_OK);

			if (ppu.state & cpu_flag::again)
			{
				return;
			}

			ensure(lv2_syscall<sys_mutex_unlock>(ppu, queue_mutex) == CELL_OK);
		}

		cmd_queue.pop(cmd);

		ensure(lv2_syscall<sys_mutex_unlock>(ppu, queue_mutex) == CELL_OK);

		savestate = atracxdec_state::checking_run_thread_1;
		label2_check_run_thread_1_state:

		ensure(lv2_syscall<sys_mutex_lock>(ppu, run_thread_mutex, 0) == CELL_OK);

		if (ppu.state & cpu_flag::again)
		{
			return;
		}

		if (!run_thread)
		{
			ensure(lv2_syscall<sys_mutex_unlock>(ppu, run_thread_mutex) == CELL_OK);
			return;
		}

		ensure(lv2_syscall<sys_mutex_unlock>(ppu, run_thread_mutex) == CELL_OK);

		savestate = atracxdec_state::executing_cmd;
		label3_execute_cmd_state:

		cellAtracXdec.trace("Command type: %d", static_cast<u32>(cmd.type.get()));

		switch (cmd.type)
		{
		case AtracXdecCmdType::start_seq:
		{
			first_decode = true;
			skip_next_frame = true;

			// Skip if access units contain an ATS header, the parameters are included in the header and we need to wait for the first decode command to parse them
			if (cmd.atracx_param.au_includes_ats_hdr_flg == CELL_ADEC_ATRACX_ATS_HDR_NOTINC)
			{
				if (decoder.set_config_info(cmd.atracx_param.sampling_freq, cmd.atracx_param.ch_config_idx, cmd.atracx_param.nbytes) == static_cast<s32>(0x80004005))
				{
					break;
				}

				if (decoder.init_decode(cmd.atracx_param.bw_pcm, cmd.atracx_param.nch_out) == static_cast<s32>(0x80004005))
				{
					break;
				}
			}

			atracx_param = cmd.atracx_param;
			break;
		}
		case AtracXdecCmdType::end_seq:
		{
			// Block savestate creation during callbacks
			std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

			if (!savestate_lock.owns_lock())
			{
				ppu.state += cpu_flag::again;
				return;
			}

			// Doesn't do anything else
			notify_seq_done.cbFunc(ppu, notify_seq_done.cbArg);
			break;
		}
		case AtracXdecCmdType::decode_au:
		{
			ensure(!!cmd.au_start_addr); // Not checked on LLE

			cellAtracXdec.trace("Waiting for output to be consumed...");

			ensure(lv2_syscall<sys_mutex_lock>(ppu, output_mutex, 0) == CELL_OK);

			if (ppu.state & cpu_flag::again)
			{
				return;
			}

			while (output_locked)
			{
				savestate = atracxdec_state::waiting_for_output;
				label4_wait_for_output_state:

				ensure(lv2_syscall<sys_cond_wait>(ppu, output_consumed, 0) == CELL_OK);

				if (ppu.state & cpu_flag::again)
				{
					return;
				}
			}

			cellAtracXdec.trace("Output consumed");

			savestate = atracxdec_state::checking_run_thread_2;
			label5_check_run_thread_2_state:

			ensure(lv2_syscall<sys_mutex_lock>(ppu, run_thread_mutex, 0) == CELL_OK);

			if (ppu.state & cpu_flag::again)
			{
				return;
			}

			if (!run_thread)
			{
				ensure(lv2_syscall<sys_mutex_unlock>(ppu, run_thread_mutex) == CELL_OK);
				ensure(lv2_syscall<sys_mutex_unlock>(ppu, output_mutex) == CELL_OK);
				return;
			}

			ensure(lv2_syscall<sys_mutex_unlock>(ppu, run_thread_mutex) == CELL_OK);

			savestate = atracxdec_state::decoding;
			label6_decode_state:

			u32 error = CELL_OK;

			// Only the first valid ATS header after starting a sequence is parsed. It is ignored on all subsequent access units
			if (first_decode && atracx_param.au_includes_ats_hdr_flg == CELL_ADEC_ATRACX_ATS_HDR_INC)
			{
				// Block savestate creation during callbacks
				std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

				if (!savestate_lock.owns_lock())
				{
					ppu.state += cpu_flag::again;
					return;
				}

				if (error = decoder.parse_ats_header(cmd.au_start_addr); error != CELL_OK)
				{
					notify_error.cbFunc(ppu, error, notify_error.cbArg);
				}
				else if (decoder.init_decode(atracx_param.bw_pcm, atracx_param.nch_out) != CELL_OK)
				{
					notify_error.cbFunc(ppu, CELL_ADEC_ERROR_ATX_FATAL, notify_error.cbArg);
				}
			}

			// LLE does not initialize the output address if parsing the ATS header fails
			vm::ptr<void> output = vm::null;
			u32 decoded_samples_num = 0;

			if (error != CELL_ADEC_ERROR_ATX_ATSHDR)
			{
				// The LLE SPU thread would crash if you attempt to decode without a valid configuration
				ensure(decoder.config_is_set, "Attempted to decode with invalid configuration");

				output.set(work_mem.addr() + atracXdecGetSpursMemSize(decoder.nch_in));

				const auto au_start_addr = atracx_param.au_includes_ats_hdr_flg == CELL_ADEC_ATRACX_ATS_HDR_INC ? cmd.au_start_addr.get_ptr() + sizeof(AtracXdecAtsHeader) : cmd.au_start_addr.get_ptr();
				std::memcpy(work_mem.get_ptr(), au_start_addr, decoder.nbytes);

#ifdef RPCS3_PS4
				const u64 ps4_decode_start = ps4_atx_now_us();

				// 0.38: a decoder FFmpeg could not allocate is retried here; while it is missing, or when FFmpeg runs out
				// of memory decoding, the access unit ends with CELL_ADEC_ERROR_ATX_FATAL (error callback, empty PCM
				// output) instead of ending the emulation
				if (!decoder.ctx)
				{
					decoder.alloc_avcodec();
					decoder.init_avcodec();
				}

				const bool ps4_decoder_ready = decoder.ctx && decoder.packet && decoder.frame;

				if (!ps4_decoder_ready)
				{
					error = CELL_ADEC_ERROR_ATX_FATAL;
				}
				else if (int err = avcodec_send_packet(decoder.ctx, decoder.packet); err)
				{
					if (err == AVERROR(ENOMEM))
					{
						ps4_atx_note_out_of_memory(decoder.nbytes);
						error = CELL_ADEC_ERROR_ATX_FATAL;
						avcodec_free_context(&decoder.ctx); // Rebuilt at the next access unit
					}
					else if (err == AVERROR(EAGAIN) || err == averror_eof || err == AVERROR(EINVAL))
					{
						// These errors should never occur
						fmt::throw_exception("avcodec_send_packet() failed (err=0x%x='%s')", err, utils::av_error_to_string(err));
					}
					else
					{
						// Game sent invalid data
						cellAtracXdec.error("avcodec_send_packet() failed (err=0x%x='%s')", err, utils::av_error_to_string(err));
						error = CELL_ADEC_ERROR_ATX_NON_FATAL; // Not accurate, FFmpeg doesn't provide detailed errors like LLE
					}

					av_frame_unref(decoder.frame);
				}
				else if ((err = avcodec_receive_frame(decoder.ctx, decoder.frame)))
				{
					if (err != AVERROR(ENOMEM))
					{
						fmt::throw_exception("avcodec_receive_frame() failed (err=0x%x='%s')", err, utils::av_error_to_string(err));
					}

					ps4_atx_note_out_of_memory(ATXDEC_SAMPLES_PER_FRAME * sizeof(f32) * decoder.nch_in);
					error = CELL_ADEC_ERROR_ATX_FATAL;
					av_frame_unref(decoder.frame);
					avcodec_free_context(&decoder.ctx); // A half-consumed packet must not leave the next send with EAGAIN
				}

				ps4_atx_note_decode(ps4_atx_now_us() - ps4_decode_start);

				decoded_samples_num = ps4_decoder_ready ? decoder.frame->nb_samples : 0;
#else
				if (int err = avcodec_send_packet(decoder.ctx, decoder.packet); err)
				{
					// These errors should never occur
					if (err == AVERROR(EAGAIN) || err == averror_eof || err == AVERROR(EINVAL) || err == AVERROR(ENOMEM))
					{
						fmt::throw_exception("avcodec_send_packet() failed (err=0x%x='%s')", err, utils::av_error_to_string(err));
					}

					// Game sent invalid data
					cellAtracXdec.error("avcodec_send_packet() failed (err=0x%x='%s')", err, utils::av_error_to_string(err));
					error = CELL_ADEC_ERROR_ATX_NON_FATAL; // Not accurate, FFmpeg doesn't provide detailed errors like LLE

					av_frame_unref(decoder.frame);
				}
				else if ((err = avcodec_receive_frame(decoder.ctx, decoder.frame)))
				{
					fmt::throw_exception("avcodec_receive_frame() failed (err=0x%x='%s')", err, utils::av_error_to_string(err));
				}

				decoded_samples_num = decoder.frame->nb_samples;
#endif
				ensure(decoded_samples_num == 0u || decoded_samples_num == ATXDEC_SAMPLES_PER_FRAME);

				// The first frame after starting a new sequence or after an error is replaced with silence
				if (skip_next_frame && error == CELL_OK)
				{
					skip_next_frame = false;
					decoded_samples_num = 0;
					std::memset(output.get_ptr(), 0, ATXDEC_SAMPLES_PER_FRAME * (decoder.bw_pcm & 0x7full) * decoder.nch_out);
				}

				// Convert FFmpeg output to LLE output
				const auto output_f32 = vm::static_ptr_cast<f32>(output).get_ptr();
				const auto output_s16 = vm::static_ptr_cast<s16>(output).get_ptr();
				const auto output_s32 = vm::static_ptr_cast<s32>(output).get_ptr();
				const u8* const ch_map = ATXDEC_AVCODEC_CH_MAP[decoder.ch_config_idx - 1];
				const u32 nch_in = decoder.nch_in;

#ifdef RPCS3_PS4
				// No samples to convert; the frame may be missing after a memory failure (alloc_avcodec)
				if (decoded_samples_num)
#endif
				switch (decoder.bw_pcm)
				{
				case CELL_ADEC_ATRACX_WORD_SZ_FLOAT:
					for (u32 channel_idx = 0; channel_idx < nch_in; channel_idx++)
					{
						const f32* samples = reinterpret_cast<f32*>(decoder.frame->data[channel_idx]);

						for (u32 in_sample_idx = 0, out_sample_idx = ch_map[channel_idx]; in_sample_idx < decoded_samples_num; in_sample_idx++, out_sample_idx += nch_in)
						{
							const f32 sample = samples[in_sample_idx];

							if (sample >= std::bit_cast<f32>(std::bit_cast<u32>(1.f) - 1))
							{
								output_f32[out_sample_idx] = std::bit_cast<be_t<f32>>("\x3f\x7f\xff\xff"_u32); // Prevents an unnecessary endian swap
							}
							else if (sample <= -1.f)
							{
								output_f32[out_sample_idx] = -1.f;
							}
							else
							{
								output_f32[out_sample_idx] = sample;
							}
						}
					}
					break;

				case CELL_ADEC_ATRACX_WORD_SZ_16BIT:
					for (u32 channel_idx = 0; channel_idx < nch_in; channel_idx++)
					{
						const f32* samples = reinterpret_cast<f32*>(decoder.frame->data[channel_idx]);

						for (u32 in_sample_idx = 0, out_sample_idx = ch_map[channel_idx]; in_sample_idx < decoded_samples_num; in_sample_idx++, out_sample_idx += nch_in)
						{
							const f32 sample = samples[in_sample_idx];

							if (sample >= 1.f)
							{
								output_s16[out_sample_idx] = INT16_MAX;
							}
							else if (sample <= -1.f)
							{
								output_s16[out_sample_idx] = INT16_MIN;
							}
							else
							{
								output_s16[out_sample_idx] = static_cast<s16>(std::floor(sample * 0x8000u));
							}
						}
					}
					break;

				case CELL_ADEC_ATRACX_WORD_SZ_24BIT:
					for (u32 channel_idx = 0; channel_idx < nch_in; channel_idx++)
					{
						const f32* samples = reinterpret_cast<f32*>(decoder.frame->data[channel_idx]);

						for (u32 in_sample_idx = 0, out_sample_idx = ch_map[channel_idx]; in_sample_idx < decoded_samples_num; in_sample_idx++, out_sample_idx += nch_in)
						{
							const f32 sample = samples[in_sample_idx];

							if (sample >= 1.f)
							{
								output_s32[out_sample_idx] = 0x007fffff;
							}
							else if (sample <= -1.f)
							{
								output_s32[out_sample_idx] = 0x00800000;
							}
							else
							{
								output_s32[out_sample_idx] = static_cast<s32>(std::floor(sample * 0x00800000u)) & 0x00ffffff;
							}
						}
					}
					break;

				case CELL_ADEC_ATRACX_WORD_SZ_32BIT:
					for (u32 channel_idx = 0; channel_idx < nch_in; channel_idx++)
					{
						const f32* samples = reinterpret_cast<f32*>(decoder.frame->data[channel_idx]);

						for (u32 in_sample_idx = 0, out_sample_idx = ch_map[channel_idx]; in_sample_idx < decoded_samples_num; in_sample_idx++, out_sample_idx += nch_in)
						{
							const f32 sample = samples[in_sample_idx];

							if (sample >= 1.f)
							{
								output_s32[out_sample_idx] = INT32_MAX;
							}
							else if (sample <= -1.f)
							{
								output_s32[out_sample_idx] = INT32_MIN;
							}
							else
							{
								output_s32[out_sample_idx] = static_cast<s32>(std::floor(sample * 0x80000000u));
							}
						}
					}
				}

				first_decode = false;

				if (error != CELL_OK)
				{
					// Block savestate creation during callbacks
					std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

					if (!savestate_lock.owns_lock())
					{
						ppu.state += cpu_flag::again;
						return;
					}

					skip_next_frame = true;
					notify_error.cbFunc(ppu, error, notify_error.cbArg);
				}
			}

			// Block savestate creation during callbacks
			std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

			if (!savestate_lock.owns_lock())
			{
				ppu.state += cpu_flag::again;
				return;
			}

			// au_done and pcm_out callbacks are always called after a decode command, even if an error occurred
			// The output always has to be consumed as well
			notify_au_done.cbFunc(ppu, cmd.pcm_handle, notify_au_done.cbArg);

			output_locked = true;
			ensure(lv2_syscall<sys_mutex_unlock>(ppu, output_mutex) == CELL_OK);

			const u32 output_size = decoded_samples_num * (decoder.bw_pcm & 0x7fu) * decoder.nch_out;

			const vm::var<CellAdecAtracXInfo> bsi_info{{ decoder.sampling_freq, decoder.ch_config_idx, decoder.nbytes }};

			const AdecCorrectPtsValueType correct_pts_type = [&]
			{
				switch (decoder.sampling_freq)
				{
				case 32000u: return ADEC_CORRECT_PTS_VALUE_TYPE_ATRACX_32000Hz;
				case 44100u: return ADEC_CORRECT_PTS_VALUE_TYPE_ATRACX_44100Hz;
				case 48000u: return ADEC_CORRECT_PTS_VALUE_TYPE_ATRACX_48000Hz;
				default:     return ADEC_CORRECT_PTS_VALUE_TYPE_UNSPECIFIED;
				}
			}();

			notify_pcm_out.cbFunc(ppu, cmd.pcm_handle, output, output_size, notify_pcm_out.cbArg, vm::make_var<vm::bcptr<void>>(bsi_info), correct_pts_type, error);
			break;
		}
		default:
			fmt::throw_exception("Invalid command");
		}
	}
}

template <AtracXdecCmdType type>
error_code AtracXdecContext::send_command(ppu_thread& ppu, auto&&... args)
{
	auto& savestate = *ppu.optional_savestate_state;
	const bool signal = savestate.try_read<bool>().second;
	savestate.clear();

	if (!signal)
	{
		ensure(lv2_syscall<sys_mutex_lock>(ppu, queue_mutex, 0) == CELL_OK);

		if (ppu.state & cpu_flag::again)
		{
			return {};
		}

		if constexpr (type == AtracXdecCmdType::close)
		{
			// Close command is only sent if the queue is empty on LLE
			if (!cmd_queue.empty())
			{
				ensure(lv2_syscall<sys_mutex_unlock>(ppu, queue_mutex) == CELL_OK);
				return {};
			}
		}

		if (cmd_queue.full())
		{
			ensure(lv2_syscall<sys_mutex_unlock>(ppu, queue_mutex) == CELL_OK);
			return CELL_ADEC_ERROR_ATX_BUSY;
		}

		cmd_queue.emplace(std::forward<AtracXdecCmdType>(type), std::forward<decltype(args)>(args)...);

		ensure(lv2_syscall<sys_mutex_unlock>(ppu, queue_mutex) == CELL_OK);
	}

	ensure(lv2_syscall<sys_cond_signal>(ppu, queue_not_empty) == CELL_OK);

	if (ppu.state & cpu_flag::again)
	{
		savestate(true);
	}

	return CELL_OK;
}

void atracXdecEntry(ppu_thread& ppu, vm::ptr<AtracXdecContext> atxdec)
{
#ifdef RPCS3_PS4
	atxdec->decoder.packet = nullptr;
	atxdec->decoder.frame = nullptr;
#endif
	atxdec->decoder.alloc_avcodec();

#ifdef RPCS3_PS4
	// Decoders open now, for the 60 s line; also released when exec leaves by an exception
	struct ps4_atx_decoder_count
	{
		ps4_atx_decoder_count() { s_ps4_atx_decoders.fetch_add(1, std::memory_order_relaxed); }
		~ps4_atx_decoder_count() { s_ps4_atx_decoders.fetch_sub(1, std::memory_order_relaxed); }
	};

	{
		const ps4_atx_decoder_count ps4_count;
		atxdec->exec(ppu);
	}
#else
	atxdec->exec(ppu);
#endif

	atxdec->decoder.free_avcodec();

	if (ppu.state & cpu_flag::again)
	{
		// For savestates, save argument
		ppu.syscall_args[0] = atxdec.addr();

		return;
	}

	ppu_execute<&sys_ppu_thread_exit>(ppu, CELL_OK);
}

template <u32 nch_in>
error_code _CellAdecCoreOpGetMemSize_atracx(vm::ptr<CellAdecAttr> attr)
{
	cellAtracXdec.notice("_CellAdecCoreOpGetMemSize_atracx<nch_in=%d>(attr=*0x%x)", nch_in, attr);

	ensure(!!attr); // Not checked on LLE

	constexpr u32 mem_size =
		sizeof(AtracXdecContext) + 0x7f
		+ ATXDEC_SPURS_STRUCTS_SIZE + 0x1d8
		+ atracXdecGetSpursMemSize(nch_in)
		+ ATXDEC_SAMPLES_PER_FRAME * sizeof(f32) * nch_in;

	attr->workMemSize = utils::align(mem_size, 0x80);

	return CELL_OK;
}

error_code _CellAdecCoreOpOpenExt_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle, vm::ptr<AdecNotifyAuDone> notifyAuDone, vm::ptr<void> notifyAuDoneArg, vm::ptr<AdecNotifyPcmOut> notifyPcmOut, vm::ptr<void> notifyPcmOutArg,
	vm::ptr<AdecNotifyError> notifyError, vm::ptr<void> notifyErrorArg, vm::ptr<AdecNotifySeqDone> notifySeqDone, vm::ptr<void> notifySeqDoneArg, vm::cptr<CellAdecResource> res, vm::cptr<CellAdecResourceSpurs> spursRes)
{
	std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

	if (!savestate_lock.owns_lock())
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	cellAtracXdec.notice("_CellAdecCoreOpOpenExt_atracx(handle=*0x%x, notifyAuDone=*0x%x, notifyAuDoneArg=*0x%x, notifyPcmOut=*0x%x, notifyPcmOutArg=*0x%x, notifyError=*0x%x, notifyErrorArg=*0x%x, notifySeqDone=*0x%x, notifySeqDoneArg=*0x%x, res=*0x%x, spursRes=*0x%x)",
		handle, notifyAuDone, notifyAuDoneArg, notifyPcmOut, notifyPcmOutArg, notifyError, notifyErrorArg, notifySeqDone, notifySeqDoneArg, res, spursRes);

	ensure(!!handle && !!res); // Not checked on LLE
	ensure(handle.aligned(0x80)); // On LLE, this functions doesn't check the alignment or aligns the address itself. The address should already be aligned to 128 bytes by cellAdec
	ensure(!!notifyAuDone && !!notifyAuDoneArg && !!notifyPcmOut && !!notifyPcmOutArg && !!notifyError && !!notifyErrorArg && !!notifySeqDone && !!notifySeqDoneArg); // These should always be set by cellAdec

	write_to_ptr_unsafe(handle.get_ptr(), AtracXdecContext(notifyAuDone, notifyAuDoneArg, notifyPcmOut, notifyPcmOutArg, notifyError, notifyErrorArg, notifySeqDone, notifySeqDoneArg,
		vm::bptr<u8>::make(handle.addr() + utils::align(static_cast<u32>(sizeof(AtracXdecContext)), 0x80) + ATXDEC_SPURS_STRUCTS_SIZE)));

	const vm::var<sys_mutex_attribute_t> mutex_attr{{ SYS_SYNC_PRIORITY, SYS_SYNC_NOT_RECURSIVE, SYS_SYNC_NOT_PROCESS_SHARED, SYS_SYNC_NOT_ADAPTIVE, 0, 0, 0, { "_atd001"_u64 } }};
	const vm::var<sys_cond_attribute_t> cond_attr{{ SYS_SYNC_NOT_PROCESS_SHARED, 0, 0, { "_atd002"_u64 } }};

	ensure(lv2_syscall<sys_mutex_create>(ppu, handle.ptr(&AtracXdecContext::queue_mutex), mutex_attr) == CELL_OK);
	ensure(lv2_syscall<sys_cond_create>(ppu, handle.ptr(&AtracXdecContext::queue_not_empty), handle->queue_mutex, cond_attr) == CELL_OK);

	mutex_attr->name_u64 = "_atd003"_u64;
	cond_attr->name_u64 = "_atd004"_u64;

	ensure(lv2_syscall<sys_mutex_create>(ppu, handle.ptr(&AtracXdecContext::run_thread_mutex), mutex_attr) == CELL_OK);
	ensure(lv2_syscall<sys_cond_create>(ppu, handle.ptr(&AtracXdecContext::run_thread_cond), handle->run_thread_mutex, cond_attr) == CELL_OK);

	mutex_attr->name_u64 = "_atd005"_u64;
	cond_attr->name_u64 = "_atd006"_u64;

	ensure(lv2_syscall<sys_mutex_create>(ppu, handle.ptr(&AtracXdecContext::output_mutex), mutex_attr) == CELL_OK);
	ensure(lv2_syscall<sys_cond_create>(ppu, handle.ptr(&AtracXdecContext::output_consumed), handle->output_mutex, cond_attr) == CELL_OK);

	ensure(lv2_syscall<sys_mutex_lock>(ppu, handle->output_mutex, 0) == CELL_OK);
	handle->output_locked = false;
	ensure(lv2_syscall<sys_cond_signal>(ppu, handle->output_consumed) == CELL_OK);
	ensure(lv2_syscall<sys_mutex_unlock>(ppu, handle->output_mutex) == CELL_OK);

	const vm::var<char[]> _name = vm::make_str("HLE ATRAC3plus decoder");
	const auto entry = g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(atracXdecEntry));
#ifdef RPCS3_PS4
	if (const error_code ret = ppu_execute<&sys_ppu_thread_create>(ppu, handle.ptr(&AtracXdecContext::thread_id), entry, handle.addr(), +res->ppuThreadPriority, +res->ppuThreadStackSize, SYS_PPU_THREAD_CREATE_JOINABLE, +_name); ret != CELL_OK)
	{
		// 0.29: the decoder thread was not created (CELL_EAGAIN when the console is out of thread resources). Report it
		// as the LPCM decoder does (cellAdec.cpp, _CellAdecCoreOpOpenExt_lpcm) instead of returning CELL_OK for a
		// decoder that cannot run (a later close would join a thread id that does not exist). The six sync objects
		// created above have no other user yet: destroy them so repeated failed opens do not use up lv2 IDs.
		cellAtracXdec.error("_CellAdecCoreOpOpenExt_atracx(): sys_ppu_thread_create() failed (0x%x)", static_cast<u32>(static_cast<s32>(ret)));
		lv2_syscall<sys_cond_destroy>(ppu, handle->queue_not_empty);
		lv2_syscall<sys_cond_destroy>(ppu, handle->run_thread_cond);
		lv2_syscall<sys_cond_destroy>(ppu, handle->output_consumed);
		lv2_syscall<sys_mutex_destroy>(ppu, handle->queue_mutex);
		lv2_syscall<sys_mutex_destroy>(ppu, handle->run_thread_mutex);
		lv2_syscall<sys_mutex_destroy>(ppu, handle->output_mutex);
		return ret;
	}
#else
	ppu_execute<&sys_ppu_thread_create>(ppu, handle.ptr(&AtracXdecContext::thread_id), entry, handle.addr(), +res->ppuThreadPriority, +res->ppuThreadStackSize, SYS_PPU_THREAD_CREATE_JOINABLE, +_name);
#endif

	return CELL_OK;
}

error_code _CellAdecCoreOpOpen_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle, vm::ptr<AdecNotifyAuDone> notifyAuDone, vm::ptr<void> notifyAuDoneArg, vm::ptr<AdecNotifyPcmOut> notifyPcmOut, vm::ptr<void> notifyPcmOutArg,
	vm::ptr<AdecNotifyError> notifyError, vm::ptr<void> notifyErrorArg, vm::ptr<AdecNotifySeqDone> notifySeqDone, vm::ptr<void> notifySeqDoneArg, vm::cptr<CellAdecResource> res)
{
	cellAtracXdec.notice("_CellAdecCoreOpOpen_atracx(handle=*0x%x, notifyAuDone=*0x%x, notifyAuDoneArg=*0x%x, notifyPcmOut=*0x%x, notifyPcmOutArg=*0x%x, notifyError=*0x%x, notifyErrorArg=*0x%x, notifySeqDone=*0x%x, notifySeqDoneArg=*0x%x, res=*0x%x)",
		handle, notifyAuDone, notifyAuDoneArg, notifyPcmOut, notifyPcmOutArg, notifyError, notifyErrorArg, notifySeqDone, notifySeqDoneArg, res);

	return _CellAdecCoreOpOpenExt_atracx(ppu, handle, notifyAuDone, notifyAuDoneArg, notifyPcmOut, notifyPcmOutArg, notifyError, notifyErrorArg, notifySeqDone, notifySeqDoneArg, res, vm::null);
}

error_code _CellAdecCoreOpClose_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle)
{
	std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

	if (!savestate_lock.owns_lock())
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	cellAtracXdec.notice("_CellAdecCoreOpClose_atracx(handle=*0x%x)", handle);

	ensure(!!handle); // Not checked on LLE

	ensure(lv2_syscall<sys_mutex_lock>(ppu, handle->run_thread_mutex, 0) == CELL_OK);
	handle->run_thread = false;
	ensure(lv2_syscall<sys_mutex_unlock>(ppu, handle->run_thread_mutex) == CELL_OK);

	handle->send_command<AtracXdecCmdType::close>(ppu);

	ensure(lv2_syscall<sys_mutex_lock>(ppu, handle->output_mutex, 0) == CELL_OK);
	handle->output_locked = false;
	ensure(lv2_syscall<sys_mutex_unlock>(ppu, handle->output_mutex) == CELL_OK);
	ensure(lv2_syscall<sys_cond_signal>(ppu, handle->output_consumed) == CELL_OK);

	vm::var<u64> thread_ret;
	ensure(lv2_syscall<sys_ppu_thread_join>(ppu, static_cast<u32>(handle->thread_id), +thread_ret) == CELL_OK);

	error_code ret = lv2_syscall<sys_cond_destroy>(ppu, handle->queue_not_empty);
	ret = ret ? ret : lv2_syscall<sys_cond_destroy>(ppu, handle->run_thread_cond);
	ret = ret ? ret : lv2_syscall<sys_cond_destroy>(ppu, handle->output_consumed);
	ret = ret ? ret : lv2_syscall<sys_mutex_destroy>(ppu, handle->queue_mutex);
	ret = ret ? ret : lv2_syscall<sys_mutex_destroy>(ppu, handle->run_thread_mutex);
	ret = ret ? ret : lv2_syscall<sys_mutex_destroy>(ppu, handle->output_mutex);

	return ret != CELL_OK ? static_cast<error_code>(CELL_ADEC_ERROR_FATAL) : CELL_OK;
}

error_code _CellAdecCoreOpStartSeq_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle, vm::cptr<CellAdecParamAtracX> atracxParam)
{
	cellAtracXdec.notice("_CellAdecCoreOpStartSeq_atracx(handle=*0x%x, atracxParam=*0x%x)", handle, atracxParam);

	ensure(!!handle && !!atracxParam); // Not checked on LLE

	cellAtracXdec.notice("_CellAdecCoreOpStartSeq_atracx(): sampling_freq=%d, ch_config_idx=%d, nch_out=%d, nbytes=0x%x, extra_config_data=0x%08x, bw_pcm=0x%x, downmix_flag=%d, au_includes_ats_hdr_flg=%d",
		atracxParam->sampling_freq, atracxParam->ch_config_idx, atracxParam->nch_out, atracxParam->nbytes, std::bit_cast<u32>(atracxParam->extra_config_data), atracxParam->bw_pcm, atracxParam->downmix_flag, atracxParam->au_includes_ats_hdr_flg);

	return handle->send_command<AtracXdecCmdType::start_seq>(ppu, *atracxParam);
}

error_code _CellAdecCoreOpEndSeq_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle)
{
	cellAtracXdec.notice("_CellAdecCoreOpEndSeq_atracx(handle=*0x%x)", handle);

	ensure(!!handle); // Not checked on LLE

	return handle->send_command<AtracXdecCmdType::end_seq>(ppu);
}

error_code _CellAdecCoreOpDecodeAu_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle, s32 pcmHandle, vm::cptr<CellAdecAuInfo> auInfo)
{
	cellAtracXdec.trace("_CellAdecCoreOpDecodeAu_atracx(handle=*0x%x, pcmHandle=%d, auInfo=*0x%x)", handle, pcmHandle, auInfo);

	ensure(!!handle && !!auInfo); // Not checked on LLE

	cellAtracXdec.trace("_CellAdecCoreOpDecodeAu_atracx(): startAddr=*0x%x, size=0x%x, pts=%lld, userData=0x%llx", auInfo->startAddr, auInfo->size, std::bit_cast<be_t<u64>>(auInfo->pts), auInfo->userData);

	return handle->send_command<AtracXdecCmdType::decode_au>(ppu, pcmHandle, *auInfo);
}

void _CellAdecCoreOpGetVersion_atracx(vm::ptr<be_t<u32, 1>> version)
{
	cellAtracXdec.notice("_CellAdecCoreOpGetVersion_atracx(version=*0x%x)", version);

	ensure(!!version); // Not checked on LLE

	*version = 0x01020000;
}

error_code _CellAdecCoreOpRealign_atracx(vm::ptr<AtracXdecContext> handle, vm::ptr<void> outBuffer, vm::cptr<void> pcmStartAddr)
{
	cellAtracXdec.trace("_CellAdecCoreOpRealign_atracx(handle=*0x%x, outBuffer=*0x%x, pcmStartAddr=*0x%x)", handle, outBuffer, pcmStartAddr);

	if (outBuffer)
	{
		ensure(!!handle && !!pcmStartAddr); // Not checked on LLE
		ensure(vm::check_addr(outBuffer.addr(), vm::page_info_t::page_writable, handle->decoder.pcm_output_size));

		std::memcpy(outBuffer.get_ptr(), pcmStartAddr.get_ptr(), handle->decoder.pcm_output_size);
	}

	return CELL_OK;
}

error_code _CellAdecCoreOpReleasePcm_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle, s32 pcmHandle, vm::cptr<void> outBuffer)
{
	cellAtracXdec.trace("_CellAdecCoreOpReleasePcm_atracx(handle=*0x%x, pcmHandle=%d, outBuffer=*0x%x)", handle, pcmHandle, outBuffer);

	ensure(!!handle); // Not checked on LLE

	auto& savestate = *ppu.optional_savestate_state;
	const bool signal = savestate.try_read<bool>().second;
	savestate.clear();

	if (!signal)
	{
		ensure(lv2_syscall<sys_mutex_lock>(ppu, handle->output_mutex, 0) == CELL_OK);

		if (ppu.state & cpu_flag::again)
		{
			return {};
		}

		handle->output_locked = false;
	}

	ensure(lv2_syscall<sys_cond_signal>(ppu, handle->output_consumed) == CELL_OK);

	if (ppu.state & cpu_flag::again)
	{
		savestate(true);
		return {};
	}

	ensure(lv2_syscall<sys_mutex_unlock>(ppu, handle->output_mutex) == CELL_OK);

	return CELL_OK;
}

s32 _CellAdecCoreOpGetPcmHandleNum_atracx()
{
	cellAtracXdec.notice("_CellAdecCoreOpGetPcmHandleNum_atracx()");

	return 3;
}

u32 _CellAdecCoreOpGetBsiInfoSize_atracx()
{
	cellAtracXdec.notice("_CellAdecCoreOpGetBsiInfoSize_atracx()");

	return sizeof(CellAdecAtracXInfo);
}

static void init_gvar(vm::gvar<CellAdecCoreOps>& var)
{
	var->open.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpOpen_atracx)));
	var->close.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpClose_atracx)));
	var->startSeq.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpStartSeq_atracx)));
	var->endSeq.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpEndSeq_atracx)));
	var->decodeAu.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpDecodeAu_atracx)));
	var->getVersion.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpGetVersion_atracx)));
	var->realign.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpRealign_atracx)));
	var->releasePcm.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpReleasePcm_atracx)));
	var->getPcmHandleNum.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpGetPcmHandleNum_atracx)));
	var->getBsiInfoSize.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpGetBsiInfoSize_atracx)));
	var->openExt.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpOpenExt_atracx)));
}

DECLARE(ppu_module_manager::cellAtracXdec)("cellAtracXdec", []()
{
	REG_VNID(cellAtracXdec, 0x076b33ab, g_cell_adec_core_ops_atracx2ch).init = []()
	{
		g_cell_adec_core_ops_atracx2ch->getMemSize.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpGetMemSize_atracx<2>)));
		init_gvar(g_cell_adec_core_ops_atracx2ch);
	};
	REG_VNID(cellAtracXdec, 0x1d210eaa, g_cell_adec_core_ops_atracx6ch).init = []()
	{
		g_cell_adec_core_ops_atracx6ch->getMemSize.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpGetMemSize_atracx<6>)));
		init_gvar(g_cell_adec_core_ops_atracx6ch);
	};
	REG_VNID(cellAtracXdec, 0xe9a86e54, g_cell_adec_core_ops_atracx8ch).init = []()
	{
		g_cell_adec_core_ops_atracx8ch->getMemSize.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpGetMemSize_atracx<8>)));
		init_gvar(g_cell_adec_core_ops_atracx8ch);
	};
	REG_VNID(cellAtracXdec, 0x4944af9a, g_cell_adec_core_ops_atracx).init = []()
	{
		g_cell_adec_core_ops_atracx->getMemSize.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpGetMemSize_atracx<8>)));
		init_gvar(g_cell_adec_core_ops_atracx);
	};

	REG_HIDDEN_FUNC(_CellAdecCoreOpGetMemSize_atracx<2>);
	REG_HIDDEN_FUNC(_CellAdecCoreOpGetMemSize_atracx<6>);
	REG_HIDDEN_FUNC(_CellAdecCoreOpGetMemSize_atracx<8>);
	REG_HIDDEN_FUNC(_CellAdecCoreOpOpen_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpClose_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpStartSeq_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpEndSeq_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpDecodeAu_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpGetVersion_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpRealign_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpReleasePcm_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpGetPcmHandleNum_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpGetBsiInfoSize_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpOpenExt_atracx);

	REG_HIDDEN_FUNC(atracXdecEntry);
});
