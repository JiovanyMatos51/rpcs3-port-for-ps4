#include <libusb.h>
#include <orbis/Sysmodule.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>

// OpenOrbis Usbd.h has incomplete signatures and depends on an unbundled
// libusb header. Declare the native ABI against RPCS3's pinned libusb types.
// Signature reference: shadPS4 src/core/libraries/usbd/usbd.{h,cpp}.
extern "C"
{
int sceUsbdInit();
void sceUsbdExit();
int64_t sceUsbdGetDeviceList(libusb_device***);
void sceUsbdFreeDeviceList(libusb_device**, int);
libusb_device* sceUsbdRefDevice(libusb_device*);
void sceUsbdUnrefDevice(libusb_device*);
int sceUsbdGetDeviceDescriptor(libusb_device*, libusb_device_descriptor*);
uint8_t sceUsbdGetDeviceAddress(libusb_device*);
uint8_t sceUsbdGetBusNumber(libusb_device*);
int sceUsbdOpen(libusb_device*, libusb_device_handle**);
void sceUsbdClose(libusb_device_handle*);
int sceUsbdGetConfiguration(libusb_device_handle*, int*);
int sceUsbdSetConfiguration(libusb_device_handle*, int);
int sceUsbdClaimInterface(libusb_device_handle*, int);
int sceUsbdReleaseInterface(libusb_device_handle*, int);
int sceUsbdSetInterfaceAltSetting(libusb_device_handle*, int, int);
int sceUsbdGetActiveConfigDescriptor(libusb_device*, libusb_config_descriptor**);
void sceUsbdFreeConfigDescriptor(libusb_config_descriptor*);
int sceUsbdBulkTransfer(libusb_device_handle*, unsigned char, unsigned char*, int, int*, unsigned int);
int sceUsbdControlTransfer(libusb_device_handle*, uint8_t, uint8_t, uint16_t,
    uint16_t, unsigned char*, uint16_t, unsigned int);
libusb_transfer* sceUsbdAllocTransfer(int);
void sceUsbdFreeTransfer(libusb_transfer*);
int sceUsbdSubmitTransfer(libusb_transfer*);
int sceUsbdCancelTransfer(libusb_transfer*);
int sceUsbdHandleEventsTimeout(timeval*);
}

static_assert(sizeof(timeval) == 16, "PS4 USB timeout ABI mismatch");
static_assert(offsetof(libusb_transfer, callback) == 32, "PS4 USB transfer ABI mismatch");
static_assert(offsetof(libusb_transfer, iso_packet_desc) == 60, "PS4 USB iso ABI mismatch");

struct libusb_context
{
    libusb_log_cb log_callback = nullptr;
    int log_level = LIBUSB_LOG_LEVEL_NONE;
};

namespace
{
std::mutex lifecycle_mutex;
std::mutex events_mutex;
unsigned int users = 0;
unsigned int default_users = 0;
libusb_context default_context;
bool module_owned = false;

int usb_result(int64_t result)
{
    const uint32_t code = static_cast<uint32_t>(result);
    if ((code & 0xffff0000u) == 0x80240000u)
    {
        const unsigned int error = code & 0xffffu;
        return error >= 1 && error <= 12 ? -static_cast<int>(error) : LIBUSB_ERROR_OTHER;
    }
    if (result < 0 && result >= LIBUSB_ERROR_OTHER)
        return static_cast<int>(result);
    return result >= 0 && result <= INT32_MAX ? static_cast<int>(result) : LIBUSB_ERROR_OTHER;
}
}

extern "C"
{
int LIBUSB_CALL libusb_init_context(libusb_context** context,
    const libusb_init_option options[], int option_count)
{
    if (option_count < 0 || (option_count && !options))
        return LIBUSB_ERROR_INVALID_PARAM;
    if (context)
        *context = nullptr;
    libusb_context selected;
    for (int i = 0; i < option_count; ++i)
    {
        switch (options[i].option)
        {
        case LIBUSB_OPTION_LOG_LEVEL:
            if (options[i].value.ival < LIBUSB_LOG_LEVEL_NONE || options[i].value.ival > LIBUSB_LOG_LEVEL_DEBUG)
                return LIBUSB_ERROR_INVALID_PARAM;
            selected.log_level = options[i].value.ival;
            break;
        case LIBUSB_OPTION_LOG_CB:
            selected.log_callback = options[i].value.log_cbval;
            break;
        case LIBUSB_OPTION_USE_USBDK:
        case LIBUSB_OPTION_NO_DEVICE_DISCOVERY:
            break; // These options are documented as ignored on this OS.
        default:
            return LIBUSB_ERROR_INVALID_PARAM;
        }
    }
    libusb_context* allocated = context ? new (std::nothrow) libusb_context(selected) : nullptr;
    if (context && !allocated)
        return LIBUSB_ERROR_NO_MEM;
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    if (!users)
    {
        module_owned = sceSysmoduleIsLoaded(ORBIS_SYSMODULE_USBD) != 0;
        if (module_owned && sceSysmoduleLoadModule(ORBIS_SYSMODULE_USBD) != 0)
        {
            module_owned = false;
            delete allocated;
            return LIBUSB_ERROR_NOT_FOUND;
        }
        const int result = usb_result(sceUsbdInit());
        if (result < 0)
        {
            if (module_owned)
                sceSysmoduleUnloadModule(ORBIS_SYSMODULE_USBD);
            module_owned = false;
            delete allocated;
            return result;
        }
    }
    ++users;
    if (context)
        *context = allocated;
    else
    {
        ++default_users;
        default_context = selected;
    }
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_init(libusb_context** context)
{
    return libusb_init_context(context, nullptr, 0);
}

void LIBUSB_CALL libusb_exit(libusb_context* context)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    if (!users || (!context && !default_users))
        return;
    if (context)
        delete context;
    else
        --default_users;
    if (!--users)
    {
        sceUsbdExit();
        if (module_owned)
            sceSysmoduleUnloadModule(ORBIS_SYSMODULE_USBD);
        module_owned = false;
    }
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context*, libusb_device*** list)
{
    if (!list)
        return LIBUSB_ERROR_INVALID_PARAM;
    *list = nullptr;
    return usb_result(sceUsbdGetDeviceList(list));
}
void LIBUSB_CALL libusb_free_device_list(libusb_device** list, int unref_devices)
{
    if (list)
        sceUsbdFreeDeviceList(list, unref_devices);
}
libusb_device* LIBUSB_CALL libusb_ref_device(libusb_device* device)
{
    return device ? sceUsbdRefDevice(device) : nullptr;
}
void LIBUSB_CALL libusb_unref_device(libusb_device* device)
{
    if (device)
        sceUsbdUnrefDevice(device);
}
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device* device, libusb_device_descriptor* descriptor)
{
    return usb_result(sceUsbdGetDeviceDescriptor(device, descriptor));
}
uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device* device)
{
    return sceUsbdGetDeviceAddress(device);
}
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device* device)
{
    return sceUsbdGetBusNumber(device);
}
int LIBUSB_CALL libusb_open(libusb_device* device, libusb_device_handle** handle)
{
    return usb_result(sceUsbdOpen(device, handle));
}
void LIBUSB_CALL libusb_close(libusb_device_handle* handle)
{
    if (handle)
        sceUsbdClose(handle);
}
int LIBUSB_CALL libusb_get_configuration(libusb_device_handle* handle, int* configuration)
{
    return usb_result(sceUsbdGetConfiguration(handle, configuration));
}
int LIBUSB_CALL libusb_set_configuration(libusb_device_handle* handle, int configuration)
{
    return usb_result(sceUsbdSetConfiguration(handle, configuration));
}
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle* handle, int interface_number)
{
    return usb_result(sceUsbdClaimInterface(handle, interface_number));
}
int LIBUSB_CALL libusb_release_interface(libusb_device_handle* handle, int interface_number)
{
    return usb_result(sceUsbdReleaseInterface(handle, interface_number));
}
int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle* handle, int interface_number, int alternate)
{
    return usb_result(sceUsbdSetInterfaceAltSetting(handle, interface_number, alternate));
}
int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device* device, libusb_config_descriptor** config)
{
    return usb_result(sceUsbdGetActiveConfigDescriptor(device, config));
}
void LIBUSB_CALL libusb_free_config_descriptor(libusb_config_descriptor* config)
{
    if (config) sceUsbdFreeConfigDescriptor(config);
}
int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle* handle, unsigned char endpoint,
    unsigned char* buffer, int length, int* transferred, unsigned int timeout)
{
    return usb_result(sceUsbdBulkTransfer(handle, endpoint, buffer, length, transferred, timeout));
}
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle* handle, uint8_t request_type,
    uint8_t request, uint16_t value, uint16_t index, unsigned char* data, uint16_t length,
    unsigned int timeout)
{
    return usb_result(sceUsbdControlTransfer(handle, request_type, request, value, index, data, length, timeout));
}
libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    return iso_packets < 0 ? nullptr : sceUsbdAllocTransfer(iso_packets);
}
void LIBUSB_CALL libusb_free_transfer(libusb_transfer* transfer)
{
    if (transfer)
        sceUsbdFreeTransfer(transfer);
}
int LIBUSB_CALL libusb_submit_transfer(libusb_transfer* transfer)
{
    return usb_result(sceUsbdSubmitTransfer(transfer));
}
int LIBUSB_CALL libusb_cancel_transfer(libusb_transfer* transfer)
{
    return usb_result(sceUsbdCancelTransfer(transfer));
}
int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context*, timeval* timeout, int* completed)
{
    if (timeout && (timeout->tv_sec < 0 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000))
        return LIBUSB_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(events_mutex);
    if (completed && *completed)
        return LIBUSB_SUCCESS;
    timeval fallback{60, 0};
    return usb_result(sceUsbdHandleEventsTimeout(timeout ? timeout : &fallback));
}

int LIBUSB_CALL libusb_has_capability(uint32_t capability)
{
    // sceUsbd has no hotplug registration API. RPCS3 already implements
    // periodic enumeration when this capability is absent.
    return capability == LIBUSB_CAP_HAS_CAPABILITY;
}
int LIBUSB_CALL libusb_hotplug_register_callback(libusb_context*, int, int,
    int, int, int, libusb_hotplug_callback_fn, void*, libusb_hotplug_callback_handle*)
{
    return LIBUSB_ERROR_NOT_SUPPORTED;
}
void LIBUSB_CALL libusb_hotplug_deregister_callback(libusb_context*, libusb_hotplug_callback_handle)
{
    // No callbacks can be registered by this backend.
}
const char* LIBUSB_CALL libusb_error_name(int error)
{
    switch (error)
    {
    case LIBUSB_SUCCESS: return "LIBUSB_SUCCESS";
    case LIBUSB_ERROR_IO: return "LIBUSB_ERROR_IO";
    case LIBUSB_ERROR_INVALID_PARAM: return "LIBUSB_ERROR_INVALID_PARAM";
    case LIBUSB_ERROR_ACCESS: return "LIBUSB_ERROR_ACCESS";
    case LIBUSB_ERROR_NO_DEVICE: return "LIBUSB_ERROR_NO_DEVICE";
    case LIBUSB_ERROR_NOT_FOUND: return "LIBUSB_ERROR_NOT_FOUND";
    case LIBUSB_ERROR_BUSY: return "LIBUSB_ERROR_BUSY";
    case LIBUSB_ERROR_TIMEOUT: return "LIBUSB_ERROR_TIMEOUT";
    case LIBUSB_ERROR_OVERFLOW: return "LIBUSB_ERROR_OVERFLOW";
    case LIBUSB_ERROR_PIPE: return "LIBUSB_ERROR_PIPE";
    case LIBUSB_ERROR_INTERRUPTED: return "LIBUSB_ERROR_INTERRUPTED";
    case LIBUSB_ERROR_NO_MEM: return "LIBUSB_ERROR_NO_MEM";
    case LIBUSB_ERROR_NOT_SUPPORTED: return "LIBUSB_ERROR_NOT_SUPPORTED";
    default: return "LIBUSB_ERROR_OTHER";
    }
}
}
