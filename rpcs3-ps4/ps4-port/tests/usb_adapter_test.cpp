#include <libusb.h>
#include <orbis/Sysmodule.h>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace
{
int loaded = 1;
int load_result = 0;
int init_result = 0;
int native_result = 0;
int loads = 0, unloads = 0, inits = 0, exits = 0, event_calls = 0;
int unref_flag = -1;
libusb_device* device = reinterpret_cast<libusb_device*>(uintptr_t{16});
libusb_device_handle* handle = reinterpret_cast<libusb_device_handle*>(uintptr_t{32});
libusb_device* devices[] = {device, nullptr};
libusb_transfer transfer{};
}

// Native call doubles are confined to this host test; the console build links
// these names to the actual SceUsbd/SceSysmodule import libraries.
extern "C"
{
int32_t sceSysmoduleIsLoaded(OrbisSysModule) { return loaded; }
int32_t sceSysmoduleLoadModule(OrbisSysModule) { ++loads; return load_result; }
int32_t sceSysmoduleUnloadModule(OrbisSysModule) { ++unloads; return 0; }
int sceUsbdInit() { ++inits; return init_result; }
void sceUsbdExit() { ++exits; }
int64_t sceUsbdGetDeviceList(libusb_device*** list) { *list = devices; return native_result ? native_result : 1; }
void sceUsbdFreeDeviceList(libusb_device**, int unref) { unref_flag = unref; }
libusb_device* sceUsbdRefDevice(libusb_device* dev) { return dev; }
void sceUsbdUnrefDevice(libusb_device*) {}
int sceUsbdGetDeviceDescriptor(libusb_device*, libusb_device_descriptor*) { return native_result; }
uint8_t sceUsbdGetDeviceAddress(libusb_device*) { return 4; }
uint8_t sceUsbdGetBusNumber(libusb_device*) { return 3; }
int sceUsbdOpen(libusb_device*, libusb_device_handle** out) { *out = handle; return native_result; }
void sceUsbdClose(libusb_device_handle*) {}
int sceUsbdGetConfiguration(libusb_device_handle*, int* config) { *config = 1; return native_result; }
int sceUsbdSetConfiguration(libusb_device_handle*, int) { return native_result; }
int sceUsbdClaimInterface(libusb_device_handle*, int) { return native_result; }
int sceUsbdReleaseInterface(libusb_device_handle*, int) { return native_result; }
int sceUsbdSetInterfaceAltSetting(libusb_device_handle*, int, int) { return native_result; }
int sceUsbdGetActiveConfigDescriptor(libusb_device*, libusb_config_descriptor**) { return native_result; }
void sceUsbdFreeConfigDescriptor(libusb_config_descriptor*) {}
int sceUsbdBulkTransfer(libusb_device_handle*, unsigned char, unsigned char*, int, int*, unsigned int) { return native_result; }
int sceUsbdControlTransfer(libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t,
    unsigned char*, uint16_t, unsigned int) { return native_result; }
libusb_transfer* sceUsbdAllocTransfer(int) { return &transfer; }
void sceUsbdFreeTransfer(libusb_transfer*) {}
int sceUsbdSubmitTransfer(libusb_transfer*) { return native_result; }
int sceUsbdCancelTransfer(libusb_transfer*) { return native_result; }
int sceUsbdHandleEventsTimeout(timeval* timeout) { assert(timeout); ++event_calls; return native_result; }
}

int main()
{
    libusb_context* first = nullptr;
    libusb_context* second = nullptr;
    assert(libusb_init(&first) == 0 && first);
    assert(libusb_init(&second) == 0 && second && second != first);
    assert(inits == 1 && loads == 1);
    libusb_exit(first);
    assert(exits == 0 && unloads == 0);
    libusb_exit(second);
    assert(exits == 1 && unloads == 1);

    loaded = 0; // A module owned by another component must remain loaded.
    assert(libusb_init(nullptr) == 0);
    libusb_exit(nullptr);
    assert(exits == 2 && loads == 1 && unloads == 1);

    loaded = 1;
    init_result = static_cast<int>(0x80240003u);
    assert(libusb_init(&first) == LIBUSB_ERROR_ACCESS && !first);
    assert(unloads == 2);
    init_result = 0;
    load_result = -1;
    assert(libusb_init(&first) == LIBUSB_ERROR_NOT_FOUND && !first);
    load_result = 0;
    assert(libusb_init(&first) == 0);
    for (int error = 1; error <= 12; ++error)
    {
        native_result = static_cast<int>(0x80240000u + error);
        assert(libusb_claim_interface(handle, 0) == -error);
        assert(libusb_submit_transfer(&transfer) == -error);
    }
    native_result = static_cast<int>(0x802400ffu);
    assert(libusb_submit_transfer(&transfer) == LIBUSB_ERROR_OTHER);
    native_result = 42;
    assert(libusb_control_transfer(handle, 0, 0, 0, 0, nullptr, 0, 0) == 42);
    native_result = 0;
    libusb_device** list = nullptr;
    assert(libusb_get_device_list(first, &list) == 1 && list == devices);
    libusb_free_device_list(list, 1);
    assert(unref_flag == 1 && libusb_get_bus_number(device) == 3);

    timeval timeout{0, 1000};
    int completed = 1;
    assert(libusb_handle_events_timeout_completed(first, &timeout, &completed) == 0 && !event_calls);
    completed = 0;
    assert(libusb_handle_events_timeout_completed(first, &timeout, &completed) == 0 && event_calls == 1);
    timeout.tv_usec = 1000000;
    assert(libusb_handle_events_timeout_completed(first, &timeout, nullptr) == LIBUSB_ERROR_INVALID_PARAM);
    assert(event_calls == 1);
    assert(!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG));
    assert(std::strcmp(libusb_error_name(LIBUSB_ERROR_TIMEOUT), "LIBUSB_ERROR_TIMEOUT") == 0);
    libusb_exit(first);
    return 0;
}
