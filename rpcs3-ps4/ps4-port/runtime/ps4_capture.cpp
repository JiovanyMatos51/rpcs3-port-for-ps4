#include <alc.h>
#include <al.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>

extern "C"
{
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
// Correct AudioIn signatures; the SDK AudioIn.h still declares void(void).
int sceAudioInInit();
int sceAudioInOpen(int32_t user, uint32_t type, uint32_t index, uint32_t length,
    uint32_t frequency, uint32_t format);
int sceAudioInInput(int32_t handle, void* buffer);
int sceAudioInClose(int32_t handle);
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

struct ALCdevice
{
    int handle = -1;
    unsigned int frequency = 0;
    unsigned int channels = 0;
    SwrContext* resampler = nullptr;
    std::vector<int16_t> ring;
    size_t read_frame = 0;
    size_t available_frames = 0;
    std::mutex buffer_mutex;
    std::mutex control_mutex;
    std::atomic<bool> running{false};
    std::atomic<ALCenum> error{ALC_NO_ERROR};
    bool closing = false;
    std::thread worker;

    ~ALCdevice()
    {
        running = false;
        if (worker.joinable()) worker.join();
        if (handle >= 0) sceAudioInClose(handle);
        swr_free(&resampler);
    }

    void capture()
    {
        std::array<int16_t, 256 * 2> input{};
        std::array<int16_t, 512 * 2> output{};
        while (running.load())
        {
            const int native_result = sceAudioInInput(handle, input.data());
            if (native_result < 0)
            {
                std::fprintf(stderr, "PS4 AudioIn input failed: 0x%08x\n", static_cast<unsigned int>(native_result));
                error = ALC_INVALID_DEVICE;
                running = false;
                break;
            }
            if (!running.load()) break;
            const uint8_t* inputs[] = {reinterpret_cast<const uint8_t*>(input.data())};
            uint8_t* outputs[] = {reinterpret_cast<uint8_t*>(output.data())};
            const int frames = swr_convert(resampler, outputs, 512, inputs, 256);
            if (frames < 0)
            {
                error = ALC_INVALID_VALUE;
                running = false;
                break;
            }
            std::lock_guard<std::mutex> lock(buffer_mutex);
            const size_t capacity = ring.size() / channels;
            for (int frame = 0; frame < frames; ++frame)
            {
                // A stalled guest must not block the native capture thread.
                // Retain the newest samples if the finite capture ring fills.
                if (available_frames == capacity)
                {
                    read_frame = (read_frame + 1) % capacity;
                    --available_frames;
                }
                const size_t write_frame = (read_frame + available_frames) % capacity;
                for (unsigned int channel = 0; channel < channels; ++channel)
                    ring[write_frame * channels + channel] = output[frame * channels + channel];
                ++available_frames;
            }
        }
    }
};

namespace
{
constexpr char capture_name[] = "PS4 AudioIn";
constexpr char capture_list[] = "PS4 AudioIn\0";
std::mutex devices_mutex;
std::unordered_map<ALCdevice*, std::shared_ptr<ALCdevice>> devices;
std::atomic<ALCenum> global_error{ALC_NO_ERROR};
std::mutex init_mutex;
bool audio_initialized = false;

std::shared_ptr<ALCdevice> find_device(ALCdevice* device)
{
    std::lock_guard<std::mutex> lock(devices_mutex);
    const auto found = devices.find(device);
    return found == devices.end() ? nullptr : found->second;
}
void set_error(const std::shared_ptr<ALCdevice>& device, ALCenum value)
{
    if (device) device->error = value;
    else global_error = value;
}
}

extern "C"
{
ALCdevice* ALC_APIENTRY alcCaptureOpenDevice(const ALCchar* name, ALCuint frequency,
    ALCenum format, ALCsizei buffer_size) noexcept
{
    if ((name && *name && std::strcmp(name, capture_name)) || buffer_size <= 0 ||
        frequency < 8000 || frequency > 48000 ||
        (format != AL_FORMAT_MONO16 && format != AL_FORMAT_STEREO16))
    {
        global_error = ALC_INVALID_VALUE;
        return nullptr;
    }
    try
    {
        std::lock_guard<std::mutex> initialization(init_mutex);
        if (!audio_initialized)
        {
            if (static_cast<int32_t>(sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOIN)) < 0 || sceAudioInInit() < 0)
            {
                global_error = ALC_INVALID_DEVICE;
                return nullptr;
            }
            audio_initialized = true;
        }
        int32_t user = -1;
        if (sceUserServiceGetInitialUser(&user) < 0)
        {
            global_error = ALC_INVALID_DEVICE;
            return nullptr;
        }
        auto device = std::make_shared<ALCdevice>();
        device->channels = format == AL_FORMAT_MONO16 ? 1 : 2;
        device->frequency = frequency;
        device->ring.resize(static_cast<size_t>(buffer_size) * device->channels);
        AVChannelLayout layout = device->channels == 1 ? AVChannelLayout AV_CHANNEL_LAYOUT_MONO : AVChannelLayout AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&device->resampler, &layout, AV_SAMPLE_FMT_S16, frequency,
            &layout, AV_SAMPLE_FMT_S16, 48000, 0, nullptr) < 0 || swr_init(device->resampler) < 0)
        {
            global_error = ALC_OUT_OF_MEMORY;
            return nullptr;
        }
        // AudioIn GENERAL, 256 input frames, 48 kHz, native mono/stereo S16.
        device->handle = sceAudioInOpen(user, 1, 0, 256, 48000, device->channels == 1 ? 0 : 2);
        if (device->handle < 0)
        {
            std::fprintf(stderr, "PS4 AudioIn open failed: 0x%08x\n", static_cast<unsigned int>(device->handle));
            global_error = ALC_INVALID_DEVICE;
            return nullptr;
        }
        ALCdevice* key = device.get();
        std::lock_guard<std::mutex> lock(devices_mutex);
        devices.emplace(key, std::move(device));
        return key;
    }
    catch (...)
    {
        global_error = ALC_OUT_OF_MEMORY;
        return nullptr;
    }
}

ALCboolean ALC_APIENTRY alcCaptureCloseDevice(ALCdevice* key) noexcept
{
    std::shared_ptr<ALCdevice> device;
    {
        std::lock_guard<std::mutex> lock(devices_mutex);
        auto found = devices.find(key);
        if (found == devices.end()) { global_error = ALC_INVALID_DEVICE; return ALC_FALSE; }
        device = std::move(found->second);
        devices.erase(found);
    }
    std::lock_guard<std::mutex> control(device->control_mutex);
    device->closing = true;
    device->running = false;
    if (device->worker.joinable()) device->worker.join();
    const int result = sceAudioInClose(device->handle);
    device->handle = -1;
    if (result < 0) { global_error = ALC_INVALID_DEVICE; return ALC_FALSE; }
    return ALC_TRUE;
}

void ALC_APIENTRY alcCaptureStart(ALCdevice* key) noexcept
{
    auto device = find_device(key);
    if (!device) { global_error = ALC_INVALID_DEVICE; return; }
    std::lock_guard<std::mutex> control(device->control_mutex);
    if (device->closing) { device->error = ALC_INVALID_DEVICE; return; }
    if (device->running) return;
    if (device->worker.joinable()) device->worker.join();
    try
    {
        device->running = true;
        device->worker = std::thread([raw = device.get()] { raw->capture(); });
    }
    catch (...) { device->running = false; device->error = ALC_OUT_OF_MEMORY; }
}

void ALC_APIENTRY alcCaptureStop(ALCdevice* key) noexcept
{
    auto device = find_device(key);
    if (!device) { global_error = ALC_INVALID_DEVICE; return; }
    std::lock_guard<std::mutex> control(device->control_mutex);
    device->running = false;
    if (device->worker.joinable()) device->worker.join();
}

void ALC_APIENTRY alcGetIntegerv(ALCdevice* key, ALCenum parameter, ALCsizei count, ALCint* values) noexcept
{
    auto device = find_device(key);
    if (!device) { global_error = ALC_INVALID_DEVICE; return; }
    if (!values || count < 1) { device->error = ALC_INVALID_VALUE; return; }
    if (parameter != ALC_CAPTURE_SAMPLES) { device->error = ALC_INVALID_ENUM; return; }
    std::lock_guard<std::mutex> lock(device->buffer_mutex);
    values[0] = static_cast<ALCint>(device->available_frames);
}

void ALC_APIENTRY alcCaptureSamples(ALCdevice* key, ALCvoid* destination, ALCsizei frames) noexcept
{
    auto device = find_device(key);
    if (!device) { global_error = ALC_INVALID_DEVICE; return; }
    if (frames < 0 || (frames && !destination)) { device->error = ALC_INVALID_VALUE; return; }
    std::lock_guard<std::mutex> lock(device->buffer_mutex);
    if (static_cast<size_t>(frames) > device->available_frames) { device->error = ALC_INVALID_VALUE; return; }
    auto output = static_cast<int16_t*>(destination);
    const size_t capacity = device->ring.size() / device->channels;
    for (ALCsizei frame = 0; frame < frames; ++frame)
    {
        for (unsigned int channel = 0; channel < device->channels; ++channel)
            *output++ = device->ring[device->read_frame * device->channels + channel];
        device->read_frame = (device->read_frame + 1) % capacity;
        --device->available_frames;
    }
}

ALCenum ALC_APIENTRY alcGetError(ALCdevice* key) noexcept
{
    if (!key) return global_error.exchange(ALC_NO_ERROR);
    auto device = find_device(key);
    return device ? device->error.exchange(ALC_NO_ERROR) : ALC_INVALID_DEVICE;
}
ALCboolean ALC_APIENTRY alcIsExtensionPresent(ALCdevice*, const ALCchar* name) noexcept
{
    return name && !std::strcmp(name, "ALC_ENUMERATION_EXT") ? ALC_TRUE : ALC_FALSE;
}
const ALCchar* ALC_APIENTRY alcGetString(ALCdevice* key, ALCenum parameter) noexcept
{
    switch (parameter)
    {
    case ALC_CAPTURE_DEVICE_SPECIFIER: return key ? capture_name : capture_list;
    case ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER:
    case ALC_DEFAULT_DEVICE_SPECIFIER: return capture_name;
    case ALC_EXTENSIONS: return "ALC_ENUMERATION_EXT";
    case ALC_NO_ERROR: return "No error";
    case ALC_INVALID_DEVICE: return "Invalid PS4 capture device";
    case ALC_INVALID_CONTEXT: return "Invalid context";
    case ALC_INVALID_ENUM: return "Invalid parameter";
    case ALC_INVALID_VALUE: return "Invalid value";
    case ALC_OUT_OF_MEMORY: return "Out of memory";
    default: set_error(find_device(key), ALC_INVALID_ENUM); return nullptr;
    }
}
}
