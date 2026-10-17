#include <rtmidi_c.h>
#include <libusb.h>
#include "usb_midi_decoder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace
{
struct midi_port
{
    libusb_device* device = nullptr;
    unsigned char endpoint = 0;
    unsigned int cable = 0;
    int interface_number = 0;
    int alternate = 0;
    std::string name;
    midi_port() = default;
    midi_port(const midi_port&) = delete;
    midi_port& operator=(const midi_port&) = delete;
    midi_port(midi_port&& other) noexcept
        : device(std::exchange(other.device, nullptr)), endpoint(other.endpoint), cable(other.cable),
          interface_number(other.interface_number), alternate(other.alternate), name(std::move(other.name)) {}
    ~midi_port() { if (device) libusb_unref_device(device); }
};
struct midi_message
{
    std::vector<unsigned char> data;
    double delta = 0;
};
struct midi_input
{
    libusb_context* context = nullptr;
    libusb_device_handle* handle = nullptr;
    std::vector<midi_port> ports;
    size_t selected = 0;
    std::atomic<bool> running{false};
    std::atomic<unsigned int> ignore_flags{7};
    std::atomic<int> input_error{0};
    std::mutex mutex;
    std::deque<midi_message> queue;
    std::thread worker;

    void close()
    {
        running = false;
        if (worker.joinable()) worker.join();
        if (handle)
        {
            libusb_release_interface(handle, ports[selected].interface_number);
            libusb_close(handle);
            handle = nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex);
        queue.clear();
    }
    ~midi_input()
    {
        close();
        ports.clear();
        if (context) libusb_exit(context);
    }

    bool scan()
    {
        if (libusb_init(&context) < 0) return false;
        libusb_device** list = nullptr;
        const auto count = libusb_get_device_list(context, &list);
        if (count < 0) return false;
        std::unique_ptr<libusb_device*, void(*)(libusb_device**)> list_owner(list,
            [](libusb_device** value) { libusb_free_device_list(value, 1); });
        for (ssize_t i = 0; i < count; ++i)
        {
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(list[i], &descriptor) < 0) continue;
            libusb_config_descriptor* config = nullptr;
            if (libusb_get_active_config_descriptor(list[i], &config) < 0 || !config) continue;
            std::unique_ptr<libusb_config_descriptor, decltype(&libusb_free_config_descriptor)>
                config_owner(config, &libusb_free_config_descriptor);
            for (unsigned int j = 0; j < config->bNumInterfaces; ++j)
            for (int a = 0; a < config->interface[j].num_altsetting; ++a)
            {
                const auto& interface = config->interface[j].altsetting[a];
                // USB MIDI 1.0 streaming interfaces; MIDI 2.0 uses a different
                // packet protocol and must not be decoded as MIDI 1.0.
                if (interface.bInterfaceClass != 1 || interface.bInterfaceSubClass != 3 ||
                    interface.bInterfaceProtocol != 0) continue;
                for (unsigned int e = 0; e < interface.bNumEndpoints; ++e)
                {
                    const auto& endpoint = interface.endpoint[e];
                    if (!(endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN) ||
                        (endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
                    unsigned int cables = 1;
                    for (int offset = 0; offset + 4 <= endpoint.extra_length;)
                    {
                        const auto* extra = endpoint.extra + offset;
                        if (extra[0] < 2 || offset + extra[0] > endpoint.extra_length) break;
                        if (extra[1] == 0x25 && extra[2] == 1 && extra[0] >= 4 + extra[3])
                            cables = std::clamp<unsigned int>(extra[3], 1, 16);
                        offset += extra[0];
                    }
                    for (unsigned int cable = 0; cable < cables; ++cable)
                    {
                        midi_port port;
                        port.device = libusb_ref_device(list[i]);
                        port.endpoint = endpoint.bEndpointAddress;
                        port.interface_number = interface.bInterfaceNumber;
                        port.alternate = interface.bAlternateSetting;
                        port.cable = cable;
                        char name[128]{};
                        std::snprintf(name, sizeof(name), "PS4 USB MIDI %04x:%04x bus %u device %u interface %u endpoint %02x cable %u",
                            descriptor.idVendor, descriptor.idProduct, libusb_get_bus_number(list[i]),
                            libusb_get_device_address(list[i]), interface.bInterfaceNumber, port.endpoint, cable);
                        port.name = name;
                        ports.emplace_back(std::move(port));
                    }
                }
            }
        }
        return true;
    }

    void read()
    {
        try
        {
            unsigned char buffer[1024]{};
            usb_midi_decoder decoder;
            auto previous = std::chrono::steady_clock::time_point{};
            while (running.load())
            {
                int transferred = 0;
                const int result = libusb_bulk_transfer(handle, ports[selected].endpoint,
                    buffer, sizeof(buffer), &transferred, 50);
                if (result < 0 && result != LIBUSB_ERROR_TIMEOUT)
                { input_error = result; running = false; break; }
                if (transferred < 0 || transferred > static_cast<int>(sizeof(buffer)))
                { input_error = LIBUSB_ERROR_OVERFLOW; running = false; break; }
                const unsigned int flags = ignore_flags.load();
                for (int offset = 0; offset + 4 <= transferred; offset += 4)
                {
                    decoder.packet(buffer + offset, ports[selected].cable, flags & 1, flags & 2, flags & 4,
                        [&](const unsigned char* data, size_t length)
                        {
                            const auto now = std::chrono::steady_clock::now();
                            midi_message message;
                            message.data.assign(data, data + length);
                            message.delta = previous == std::chrono::steady_clock::time_point{} ? 0 :
                                std::chrono::duration<double>(now - previous).count();
                            previous = now;
                            std::lock_guard<std::mutex> lock(mutex);
                            if (queue.size() == 1024) queue.pop_front();
                            queue.emplace_back(std::move(message));
                        });
                }
            }
        }
        catch (...) { input_error = LIBUSB_ERROR_NO_MEM; running = false; }
    }
};
midi_input* unwrap(RtMidiPtr wrapper)
{
    return wrapper ? static_cast<midi_input*>(wrapper->ptr) : nullptr;
}
void fail(RtMidiPtr wrapper, const char* message)
{
    if (wrapper) { wrapper->ok = false; wrapper->msg = message; }
}
}

extern "C"
{
RtMidiInPtr rtmidi_in_create_default()
{
    auto wrapper = new (std::nothrow) RtMidiWrapper{nullptr, nullptr, true, ""};
    if (!wrapper) return nullptr;
    try
    {
        auto input = std::make_unique<midi_input>();
        if (!input->scan()) fail(wrapper, "PS4 USB MIDI enumeration failed");
        wrapper->ptr = input.release();
    }
    catch (...) { fail(wrapper, "PS4 USB MIDI allocation failed"); }
    return wrapper;
}
void rtmidi_in_free(RtMidiInPtr wrapper)
{
    delete unwrap(wrapper);
    delete wrapper;
}
RtMidiApi rtmidi_in_get_current_api(RtMidiPtr wrapper)
{
    if (!unwrap(wrapper)) { fail(wrapper, "Invalid PS4 MIDI input"); return RTMIDI_API_UNSPECIFIED; }
    return RTMIDI_API_PS4_USB;
}
const char* rtmidi_api_name(RtMidiApi api)
{
    return api == RTMIDI_API_PS4_USB ? "ps4-usb-midi" : nullptr;
}
unsigned int rtmidi_get_port_count(RtMidiPtr wrapper)
{
    auto input = unwrap(wrapper);
    if (!input) { fail(wrapper, "Invalid PS4 MIDI input"); return 0; }
    return static_cast<unsigned int>(input->ports.size());
}
int rtmidi_get_port_name(RtMidiPtr wrapper, unsigned int port, char* output, int* length)
{
    auto input = unwrap(wrapper);
    if (!input || !length || port >= input->ports.size()) { fail(wrapper, "Invalid PS4 MIDI port"); return -1; }
    const auto& name = input->ports[port].name;
    const int needed = static_cast<int>(name.size() + 1);
    if (!output) { *length = needed; return 0; }
    if (*length < needed) { *length = needed; fail(wrapper, "PS4 MIDI name buffer too small"); return -1; }
    std::memcpy(output, name.c_str(), needed);
    *length = needed;
    return 0;
}
void rtmidi_open_port(RtMidiPtr wrapper, unsigned int port, const char*)
{
    auto input = unwrap(wrapper);
    if (!input || port >= input->ports.size()) { fail(wrapper, "Invalid PS4 MIDI port"); return; }
    try
    {
        input->close();
        input->selected = port;
        const auto& selected = input->ports[port];
        if (libusb_open(selected.device, &input->handle) < 0 || !input->handle)
        { fail(wrapper, "Could not open PS4 MIDI device"); return; }
        if (libusb_claim_interface(input->handle, selected.interface_number) < 0)
        { libusb_close(input->handle); input->handle = nullptr; fail(wrapper, "Could not claim PS4 MIDI interface"); return; }
        if (selected.alternate && libusb_set_interface_alt_setting(input->handle, selected.interface_number, selected.alternate) < 0)
        { input->close(); fail(wrapper, "Could not select PS4 MIDI interface setting"); return; }
        input->input_error = 0;
        input->running = true;
        input->worker = std::thread([input] { input->read(); });
        wrapper->ok = true;
        wrapper->msg = "";
    }
    catch (...) { input->close(); fail(wrapper, "Could not start PS4 MIDI capture thread"); }
}
void rtmidi_in_ignore_types(RtMidiInPtr wrapper, bool sysex, bool timing, bool sense)
{
    auto input = unwrap(wrapper);
    if (!input) { fail(wrapper, "Invalid PS4 MIDI input"); return; }
    input->ignore_flags = (sysex ? 1u : 0u) | (timing ? 2u : 0u) | (sense ? 4u : 0u);
}
double rtmidi_in_get_message(RtMidiInPtr wrapper, unsigned char* output, size_t* length)
{
    auto input = unwrap(wrapper);
    if (!input || !length) { fail(wrapper, "Invalid PS4 MIDI input"); return -1; }
    if (input->input_error.load()) { fail(wrapper, libusb_error_name(input->input_error.load())); *length = 0; return -1; }
    std::lock_guard<std::mutex> lock(input->mutex);
    if (input->queue.empty()) { *length = 0; return 0; }
    const auto& message = input->queue.front();
    if (!output || *length < message.data.size())
    { *length = message.data.size(); fail(wrapper, "PS4 MIDI message buffer too small"); return -1; }
    std::memcpy(output, message.data.data(), message.data.size());
    *length = message.data.size();
    const double delta = message.delta;
    input->queue.pop_front();
    return delta;
}
}
