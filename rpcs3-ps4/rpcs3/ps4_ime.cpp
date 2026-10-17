#include "stdafx.h"
#include "ps4_ime.h"

#ifdef RPCS3_PS4

// 0.31: system on-screen keyboard (libSceImeDialog) for the XMB launcher. Frontend file: built without exceptions,
// so nothing here throws, and it includes neither orbis/ImeDialog.h nor wolfSSL.
//
// The dialog is opened with a copy of OrbisImeDialogSetting whose text pointers are char16_t: the system library
// works in UTF-16, while wchar_t is 32 bits in this build. The text and title buffers are static, because the
// system writes the typed text into the buffer while the dialog is open; both are zeroed after every dialog.

#include "ps4_launcher.h"

#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/_types/ime_dialog.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

LOG_CHANNEL(ps4_ime_log, "PS4 IME");

// libSceImeDialog and libSceCommonDialog exports (present in the SDK stubs, linked since 0.31). Bound under private
// names so the prototypes can take the char16_t struct below instead of the SDK's wchar_t one, and weak so a
// missing export means "no keyboard" instead of a load failure. The console's loader leaves weak imports from
// modules other than libkernel NULL, and libSceImeDialog is only loaded by setup(), so setup() binds each export
// after the module loads: the weak import when the loader bound it, else a lookup by name.
extern "C"
{
std::int32_t ps4_ime_dialog_init(const void* param, void* extended) __asm__("sceImeDialogInit") __attribute__((weak));
std::int32_t ps4_ime_dialog_get_status() __asm__("sceImeDialogGetStatus") __attribute__((weak));
std::int32_t ps4_ime_dialog_get_result(OrbisDialogResult* result) __asm__("sceImeDialogGetResult") __attribute__((weak));
std::int32_t ps4_ime_dialog_term() __asm__("sceImeDialogTerm") __attribute__((weak));
std::int32_t ps4_common_dialog_initialize() __asm__("sceCommonDialogInitialize") __attribute__((weak));
// libkernel exports for the lookup by name; other objects of this build import both strongly already.
std::int32_t ps4_ime_module_list(std::uint32_t* handles, std::size_t size, std::size_t* available) __asm__("sceKernelGetModuleList");
std::int32_t ps4_ime_dlsym(std::int32_t handle, const char* symbol, void** address) __asm__("sceKernelDlsym");
}

namespace ps4_ime
{
namespace
{
// OrbisImeDialogSetting (orbis/_types/ime_dialog.h) with 16-bit text pointers.
struct dialog_param
{
    std::uint32_t user_id;
    std::uint32_t type;
    std::uint64_t supported_languages; // 0 = the system's choice
    std::uint32_t enter_label;
    std::uint32_t input_method;
    void* filter;
    std::uint32_t option;
    std::uint32_t max_text_length;
    char16_t* input_text; // max_text_length + 1 units, also the initial text
    float posx;
    float posy;
    std::uint32_t horizontal_alignment;
    std::uint32_t vertical_alignment;
    const char16_t* placeholder;
    const char16_t* title;
    std::int8_t reserved[16];
};

static_assert(sizeof(dialog_param) == 96);
static_assert(offsetof(dialog_param, input_text) == 40);
static_assert(offsetof(dialog_param, title) == 72);
static_assert(sizeof(dialog_param) == sizeof(OrbisImeDialogSetting));
static_assert(offsetof(dialog_param, supported_languages) == offsetof(OrbisImeDialogSetting, supportedLanguages));
static_assert(offsetof(dialog_param, filter) == offsetof(OrbisImeDialogSetting, filter));
static_assert(offsetof(dialog_param, option) == offsetof(OrbisImeDialogSetting, option));
static_assert(offsetof(dialog_param, max_text_length) == offsetof(OrbisImeDialogSetting, maxTextLength));
static_assert(offsetof(dialog_param, input_text) == offsetof(OrbisImeDialogSetting, inputTextBuffer));
static_assert(offsetof(dialog_param, posx) == offsetof(OrbisImeDialogSetting, posx));
static_assert(offsetof(dialog_param, horizontal_alignment) == offsetof(OrbisImeDialogSetting, horizontalAlignment));
static_assert(offsetof(dialog_param, placeholder) == offsetof(OrbisImeDialogSetting, placeholder));
static_assert(offsetof(dialog_param, title) == offsetof(OrbisImeDialogSetting, title));
static_assert(sizeof(OrbisDialogResult) == 16);

// Option bits. The SDK headers define none; these are the values other PS4 projects use and are unverified on this
// firmware, so open() retries with fewer bits when the dialog refuses them.
constexpr std::uint32_t opt_no_auto_capitalization = 0x2;
constexpr std::uint32_t opt_password = 0x4;
constexpr std::uint32_t opt_no_learning = 0x20; // keep account names, mails and tokens out of the word suggestions

constexpr std::int32_t status_running = ORBIS_DIALOG_STATUS_RUNNING;
constexpr std::int32_t status_stopped = ORBIS_DIALOG_STATUS_STOPPED;

constexpr std::size_t max_text = 2048; // system limit
constexpr std::size_t max_title = 127; // in UTF-16 units, terminator excluded; stays inside a 128-unit system limit
                                       // whether or not that limit counts the terminator

char16_t s_text[max_text + 1]{};
char16_t s_title[max_title + 1]{};
dialog_param s_param{}; // kept for the lifetime of the dialog

// Entry points bound by setup(); nullptr = not exported. Written before s_setup becomes 1 and before s_active is
// published, and only read after that.
struct dialog_entry_points
{
    decltype(&ps4_ime_dialog_init) init = nullptr;
    decltype(&ps4_ime_dialog_get_status) get_status = nullptr;
    decltype(&ps4_ime_dialog_get_result) get_result = nullptr;
    decltype(&ps4_ime_dialog_term) term = nullptr;
    decltype(&ps4_common_dialog_initialize) common_initialize = nullptr;
};

constexpr unsigned entry_point_count = 5;

std::atomic<bool> s_active{false};
kind s_kind = kind::text;
std::uint32_t s_limit = 0;
int s_setup = 0;              // 0: not done, 1: ready, -1: failed
unsigned s_setup_failures = 0;
unsigned s_log_lines = 0;     // startup.log lines written by this file
dialog_entry_points s_fn{};
unsigned s_bound_by_name = 0; // entry points found by sceKernelDlsym

[[gnu::format(printf, 1, 2)]] void checkpoint(const char* format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    ps4_ime_log.notice("%s", line);
    if (s_log_lines < 32)
    {
        s_log_lines++;
        ps4_startup_checkpoint(line);
    }
}

const char* kind_name(kind type)
{
    switch (type)
    {
    case kind::text: return "text";
    case kind::username: return "username";
    case kind::password: return "password";
    case kind::email: return "email";
    case kind::token: return "token";
    case kind::number: return "number";
    case kind::hex: return "hex";
    }
    return "text";
}

std::uint32_t ime_type(kind type)
{
    switch (type)
    {
    case kind::username:
    case kind::token:
    case kind::hex: return ORBIS_TYPE_BASIC_LATIN;
    case kind::number: return ORBIS_TYPE_NUMBER;
    case kind::email: return ORBIS_TYPE_MAIL;
    case kind::text:
    case kind::password: return ORBIS_TYPE_DEFAULT; // passwords may use any character, as on the PC
    }
    return ORBIS_TYPE_DEFAULT;
}

std::uint32_t preferred_options(kind type)
{
    switch (type)
    {
    case kind::text: return 0;
    case kind::password: return opt_password | opt_no_auto_capitalization | opt_no_learning;
    case kind::username:
    case kind::email:
    case kind::token:
    case kind::number:
    case kind::hex: return opt_no_auto_capitalization | opt_no_learning;
    }
    return 0;
}

std::uint32_t text_limit(kind type, unsigned requested)
{
    std::uint32_t cap = max_text, fallback = 256;
    switch (type)
    {
    case kind::text: break;
    case kind::username: cap = fallback = 16; break;
    case kind::token: cap = fallback = 16; break;
    case kind::password: cap = fallback = 128; break;
    case kind::email: cap = fallback = 254; break;
    case kind::number: cap = fallback = 24; break;
    case kind::hex: cap = fallback = 40; break;
    }
    if (!requested) return fallback;
    return requested < cap ? requested : cap;
}

void wipe_buffers() noexcept
{
    volatile char16_t* text = s_text;
    for (std::size_t i = 0; i < max_text + 1; i++) text[i] = 0;
    for (std::size_t i = 0; i < max_title + 1; i++) s_title[i] = 0;
}

// First loaded module that exports 'name' (sceKernelDlsym by plain name, the lookup the profiler and the
// shared-memory code use on the console), or nullptr.
void* find_export(const char* name)
{
    std::uint32_t handles[256]{};
    std::size_t count = 0;
    if (ps4_ime_module_list(handles, 256, &count) != 0) return nullptr;
    for (std::size_t i = 0; i < count && i < 256; i++)
    {
        void* address = nullptr;
        if (ps4_ime_dlsym(static_cast<std::int32_t>(handles[i]), name, &address) == 0 && address) return address;
    }
    return nullptr;
}

// Fills an unbound slot from the weak import when the loader bound it, else from the export found by name.
template <typename F>
void bind_export(F& slot, F weak, const char* name)
{
    if (slot) return;
    if (weak)
    {
        slot = weak;
    }
    else if (void* address = find_export(name))
    {
        slot = reinterpret_cast<F>(address);
        s_bound_by_name++;
    }
}

// Loads the dialog modules once. A failed attempt is repeated on later opens, at most three times in all.
bool setup()
{
    if (s_setup == 1) return true;
    if (s_setup == -1 && s_setup_failures >= 3) return false;

    const std::int32_t ime = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
    if (ime != 0 && sceSysmoduleIsLoaded(ORBIS_SYSMODULE_IME_DIALOG) != 0)
    {
        checkpoint("on-screen keyboard: loading libSceImeDialog failed (0x%x)", static_cast<std::uint32_t>(ime));
        s_setup = -1;
        s_setup_failures++;
        return false;
    }

    // The common dialog layer is shared by the system dialogs; an "already initialised" result is fine, and any
    // other failure is only logged, since sceImeDialogInit then reports its own error.
    const std::int32_t common = static_cast<std::int32_t>(sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG));

    // After both loads: libSceImeDialog is not in the process before its load, so nothing could be found earlier.
    bind_export(s_fn.init, ps4_ime_dialog_init, "sceImeDialogInit");
    bind_export(s_fn.get_status, ps4_ime_dialog_get_status, "sceImeDialogGetStatus");
    bind_export(s_fn.get_result, ps4_ime_dialog_get_result, "sceImeDialogGetResult");
    bind_export(s_fn.term, ps4_ime_dialog_term, "sceImeDialogTerm");
    bind_export(s_fn.common_initialize, ps4_common_dialog_initialize, "sceCommonDialogInitialize");
    const unsigned bound = static_cast<unsigned>(!!s_fn.init + !!s_fn.get_status + !!s_fn.get_result + !!s_fn.term +
        !!s_fn.common_initialize);

    if (!s_fn.init || !s_fn.get_status || !s_fn.get_result || !s_fn.term)
    {
        checkpoint("on-screen keyboard: libSceImeDialog exports missing (bound %u/%u, %u by dlsym), keyboard unavailable",
            bound, entry_point_count, s_bound_by_name);
        s_setup = -1;
        s_setup_failures = 3;
        return false;
    }

    const std::int32_t init = s_fn.common_initialize ? s_fn.common_initialize() : -1;
    checkpoint("on-screen keyboard: libSceImeDialog 0x%x, common dialog module 0x%x, sceCommonDialogInitialize 0x%x; exports bound %u/%u (%u by dlsym)",
        static_cast<std::uint32_t>(ime), static_cast<std::uint32_t>(common), static_cast<std::uint32_t>(init), bound,
        entry_point_count, s_bound_by_name);

    s_setup = 1;
    return true;
}
} // namespace

bool open(kind type, std::string_view title, unsigned max_len, std::string_view initial)
{
    if (s_active.load(std::memory_order_acquire))
    {
        ps4_ime_log.warning("on-screen keyboard: open(%s) ignored, a dialog is already open", kind_name(type));
        return false;
    }

    if (!setup())
    {
        return false;
    }

    std::int32_t user = -1;
    if (const std::int32_t r = sceUserServiceGetInitialUser(&user); r < 0 || user < 0)
    {
        checkpoint("on-screen keyboard: no initial user (0x%x)", static_cast<std::uint32_t>(r));
        return false;
    }

    wipe_buffers();
    utf::to_utf16(title, s_title, max_title + 1);
    const std::uint32_t limit = text_limit(type, max_len);
    // The dialog edits the buffer in place, so the text it starts with is written there first
    if (!initial.empty() && type != kind::password) utf::to_utf16(initial, s_text, limit + 1);

    s_param = {};
    s_param.user_id = static_cast<std::uint32_t>(user);
    s_param.type = ime_type(type);
    s_param.supported_languages = 0;
    s_param.enter_label = ORBIS_BUTTON_LABEL_DEFAULT;
    s_param.input_method = ORBIS__DEFAULT;
    s_param.filter = nullptr;
    s_param.max_text_length = limit;
    s_param.input_text = s_text;
    s_param.posx = 960.0f; // centre of the 1920x1080 layout the dialog uses
    s_param.posy = 540.0f;
    s_param.horizontal_alignment = ORBIS_H_CENTER;
    s_param.vertical_alignment = ORBIS_V_CENTER;
    s_param.placeholder = nullptr;
    s_param.title = s_title;

    // Preferred bits first; then, for a password, only the hiding bit; then none.
    const std::uint32_t preferred = preferred_options(type);
    std::uint32_t attempts[3]{};
    std::size_t count = 0;
    const auto add = [&](std::uint32_t options)
    {
        for (std::size_t i = 0; i < count; i++)
            if (attempts[i] == options) return;
        attempts[count++] = options;
    };
    add(preferred);
    if (type == kind::password) add(opt_password);
    add(0);

    std::int32_t result = -1;
    std::uint32_t used = 0;
    for (std::size_t i = 0; i < count; i++)
    {
        const std::uint32_t options = attempts[i];
        s_param.option = options;
        result = s_fn.init(&s_param, nullptr);
        if (result >= 0)
        {
            used = options;
            break;
        }
        checkpoint("on-screen keyboard: sceImeDialogInit(%s, options 0x%x) refused 0x%x", kind_name(type), options,
            static_cast<std::uint32_t>(result));
    }
    // The number keyboard is not verified on this firmware: refused, the Latin keyboard takes the same digits
    if (result < 0 && type == kind::number)
    {
        s_param.type = ORBIS_TYPE_BASIC_LATIN;
        s_param.option = 0;
        result = s_fn.init(&s_param, nullptr);
        checkpoint("on-screen keyboard: number keyboard refused; Latin keyboard %s (0x%x)", result >= 0 ? "opened" : "refused too",
            static_cast<std::uint32_t>(result));
    }

    if (result < 0)
    {
        wipe_buffers();
        return false;
    }

    if (used != preferred)
    {
        checkpoint("on-screen keyboard: opened with options 0x%x instead of 0x%x%s", used, preferred,
            type == kind::password && !(used & opt_password) ? "; the password is visible while typing" : "");
    }

    s_kind = type;
    s_limit = limit;
    s_active.store(true, std::memory_order_release);
    ps4_ime_log.notice("on-screen keyboard opened (%s, up to %u characters)", kind_name(type), limit);
    return true;
}

state poll(std::string& out)
{
    if (!s_active.load(std::memory_order_acquire))
    {
        return state::idle;
    }

    const std::int32_t status = s_fn.get_status();
    if (status == status_running)
    {
        return state::running;
    }

    state result = state::failed;
    if (status == status_stopped)
    {
        OrbisDialogResult end{};
        const std::int32_t r = s_fn.get_result(&end);
        if (r < 0)
        {
            checkpoint("on-screen keyboard: sceImeDialogGetResult failed (0x%x)", static_cast<std::uint32_t>(r));
        }
        else if (end.endstatus == ORBIS_DIALOG_OK)
        {
            // Room for the longest result up front (at most 3 UTF-8 bytes per UTF-16 unit), so appending never
            // reallocates and leaves copies of a password in freed memory.
            out.clear();
            out.reserve(static_cast<std::size_t>(s_limit) * 3);
            utf::to_utf8(s_text, s_limit, out);
            result = state::done;
        }
        else
        {
            result = state::cancelled; // cancelled by the player, or closed by the system
        }
    }
    else
    {
        // Not running and never stopped: the system closed it, or the status call failed.
        checkpoint("on-screen keyboard: unexpected dialog status 0x%x", static_cast<std::uint32_t>(status));
    }

    const std::int32_t term = s_fn.term();
    if (term < 0)
    {
        ps4_ime_log.warning("on-screen keyboard: sceImeDialogTerm returned 0x%x", static_cast<std::uint32_t>(term));
    }

    wipe_buffers();
    s_param = {};
    s_active.store(false, std::memory_order_release);
    ps4_ime_log.notice("on-screen keyboard closed (%s): %s", kind_name(s_kind),
        result == state::done ? "confirmed" : result == state::cancelled ? "cancelled" : "failed");
    return result;
}

bool active()
{
    return s_active.load(std::memory_order_acquire);
}
} // namespace ps4_ime

#endif // RPCS3_PS4
