#include <alc.h>
#include <al.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
extern "C"
{
#include <libswresample/swresample.h>
}
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>

struct SwrContext { int channels; };
namespace
{
int channels = 1;
int opens = 0, closes = 0;
std::atomic<bool> input_failure{false};
std::atomic<int> reads{0};
}
extern "C"
{
uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal) { return 0; }
int32_t sceUserServiceGetInitialUser(int32_t* user) { *user = 1; return 0; }
int sceAudioInInit() { return 0; }
int sceAudioInOpen(int32_t, uint32_t type, uint32_t, uint32_t length, uint32_t frequency, uint32_t format)
{
    assert(type == 1 && length == 256 && frequency == 48000);
    channels = format == 0 ? 1 : 2;
    return ++opens;
}
int sceAudioInInput(int32_t, void* destination)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++reads;
    if (input_failure) return -1;
    auto data = static_cast<int16_t*>(destination);
    for (int frame = 0; frame < 256; ++frame)
        for (int channel = 0; channel < channels; ++channel)
            *data++ = static_cast<int16_t>(frame + 1000 * channel);
    return 0;
}
int sceAudioInClose(int32_t) { ++closes; return 0; }
int swr_alloc_set_opts2(SwrContext** context, const AVChannelLayout* output, AVSampleFormat, int output_rate,
    const AVChannelLayout* input, AVSampleFormat, int input_rate, int, void*)
{
    assert(output->nb_channels == input->nb_channels && output_rate == input_rate);
    *context = new SwrContext{output->nb_channels};
    return 0;
}
int swr_init(SwrContext*) { return 0; }
void swr_free(SwrContext** context) { delete *context; *context = nullptr; }
int swr_convert(SwrContext* context, uint8_t* const* output, int output_size, const uint8_t* const* input, int input_size)
{
    assert(output_size >= input_size);
    std::memcpy(output[0], input[0], input_size * context->channels * sizeof(int16_t));
    return input_size;
}
}

int main()
{
    // The test substitutes only native I/O and a 1:1 resampler. It checks the
    // actual capture worker/ring/ALC contract, not FFmpeg resampling quality.
    assert(!alcCaptureOpenDevice(nullptr, 1, AL_FORMAT_MONO16, 32));
    assert(alcGetError(nullptr) == ALC_INVALID_VALUE);
    assert(alcGetError(nullptr) == ALC_NO_ERROR);
    for (const ALCenum format : {AL_FORMAT_MONO16, AL_FORMAT_STEREO16})
    {
        ALCdevice* device = alcCaptureOpenDevice(nullptr, 48000, format, 32);
        assert(device);
        ALCint available = -1;
        alcGetIntegerv(device, ALC_CAPTURE_SAMPLES, 1, &available);
        assert(available == 0);
        alcCaptureStart(device);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do
        {
            alcGetIntegerv(device, ALC_CAPTURE_SAMPLES, 1, &available);
            std::this_thread::yield();
        } while (available < 32 && std::chrono::steady_clock::now() < deadline);
        assert(available == 32);
        alcCaptureStop(device);
        int16_t samples[64]{};
        alcCaptureSamples(device, samples, 33);
        assert(alcGetError(device) == ALC_INVALID_VALUE);
        alcCaptureSamples(device, samples, 4);
        assert(alcGetError(device) == ALC_NO_ERROR);
        const int output_channels = format == AL_FORMAT_MONO16 ? 1 : 2;
        for (int frame = 0; frame < 4; ++frame)
            for (int channel = 0; channel < output_channels; ++channel)
                assert(samples[frame * output_channels + channel] == 224 + frame + 1000 * channel);
        alcGetIntegerv(device, ALC_CAPTURE_SAMPLES, 1, &available);
        assert(available == 28);
        input_failure = true;
        const int previous_reads = reads.load();
        alcCaptureStart(device);
        while (reads.load() == previous_reads && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        alcCaptureStop(device);
        assert(alcGetError(device) == ALC_INVALID_DEVICE);
        input_failure = false;
        assert(alcCaptureCloseDevice(device) == ALC_TRUE);
        assert(alcCaptureCloseDevice(device) == ALC_FALSE);
    }
    assert(opens == 2 && closes == 2);
}
