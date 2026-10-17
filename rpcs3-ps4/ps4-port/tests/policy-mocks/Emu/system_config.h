#pragma once

// Host-test stand-in for the RPCS3 config tree, used by tests/policy_test.cpp: only the settings that
// util/ps4_policy.hpp touches, with the real names, YAML value spellings and bounds. Every setting registers in
// its cfg_root, so to_string()/from_string() work on flat "Name: value" lines like a full-dump YAML (section
// headers and indentation are ignored). set() throws on an out-of-range value, like the real cfg::_int::set.
// 0.33: entry types (get_type), a set entry (Libraries Control, "Name:" followed by "- item" lines), get_nodes() and
// a strict validate() (an unknown setting name or a rejected value fails, the known ones are still applied, like
// the real cfg::node::validate), for the config database step.
// 0.37: 'Disable SPU GETLLAR Spin Optimization', and the upstream Shader Mode default (Async Recompiler with Shader
// Interpreter).
// 0.39: 'SPU Decoder' (the interpreter warning) and System 'Language' (the game language row), with the real spellings.
// Section names of the real tree (cfg::section) give every setting its parent, so setting_key() builds the real keys
// of the pins ("Video > Vulkan > VRAM allocation limit (MB)"); the console limits 'Assume External Debugger',
// 'Initialize Directories', 'Automatically start games after boot' and 'Exit RPCS3 when process finishes'.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

enum class ppu_decoder_type { _static, llvm };
enum class spu_decoder_type { _static, dynamic, asmjit, llvm };
// Like Emu/Cell/Modules/cellSysutil.h: unscoped, s32-based, 0..19
enum CellSysutilLang : s32
{
    CELL_SYSUTIL_LANG_JAPANESE = 0,
    CELL_SYSUTIL_LANG_ENGLISH_US = 1,
    CELL_SYSUTIL_LANG_PORTUGUESE_BR = 17,
    CELL_SYSUTIL_LANG_TURKISH = 19,
};
enum class sleep_timers_accuracy_level { _as_host, _usleep, _all_timers };
enum class thread_scheduler_mode { os, old, alt };
enum class spu_block_size_type { safe, mega, giga };
enum class xfloat_accuracy { accurate, approximate, relaxed, inaccurate };
enum class video_renderer { null, opengl, vulkan };
enum class video_resolution { _1080p, _1080i, _720p, _480p, _480i };
enum class frame_limit_type { none, _30, _50, _60, _120, display_rate, _auto };
enum class shader_mode { recompiler, async_recompiler, async_with_interpreter, interpreter_only };
enum class np_internet_status { disabled, enabled };
enum class np_psn_status { disabled, psn_fake, psn_rpcn };
enum class vk_gpu_scheduler_mode { safe, fast };
enum class msaa_level { none, _auto };

template <typename T>
struct enum_text;

#define POLICY_MOCK_ENUM(T, ...) \
    template <> \
    struct enum_text<T> \
    { \
        static constexpr std::string_view names[] = {__VA_ARGS__}; \
    };

POLICY_MOCK_ENUM(ppu_decoder_type, "Interpreter (static)", "Recompiler (LLVM)")
POLICY_MOCK_ENUM(spu_decoder_type, "Interpreter (static)", "Interpreter (dynamic)", "Recompiler (ASMJIT)", "Recompiler (LLVM)")
POLICY_MOCK_ENUM(CellSysutilLang, "Japanese", "English (US)", "French", "Spanish", "German", "Italian", "Dutch", "Portuguese (Portugal)", "Russian",
    "Korean", "Chinese (Traditional)", "Chinese (Simplified)", "Finnish", "Swedish", "Danish", "Norwegian", "Polish", "Portuguese (Brazil)",
    "English (UK)", "Turkish")
POLICY_MOCK_ENUM(sleep_timers_accuracy_level, "As Host", "Usleep Only", "All Timers")
POLICY_MOCK_ENUM(thread_scheduler_mode, "Operating System", "RPCS3 Scheduler", "RPCS3 Alternative Scheduler")
POLICY_MOCK_ENUM(spu_block_size_type, "Safe", "Mega", "Giga")
POLICY_MOCK_ENUM(xfloat_accuracy, "Accurate", "Approximate", "Relaxed", "Inaccurate")
POLICY_MOCK_ENUM(video_renderer, "Null", "OpenGL", "Vulkan")
POLICY_MOCK_ENUM(video_resolution, "1920x1080", "1920x1080i", "1280x720", "720x480", "720x480i")
POLICY_MOCK_ENUM(frame_limit_type, "Off", "30", "50", "60", "120", "DisplayRate", "Auto")
POLICY_MOCK_ENUM(shader_mode, "Legacy Recompiler (single-threaded)", "Async Recompiler (multi-threaded)",
    "Async Recompiler with Shader Interpreter", "Shader Interpreter only")
POLICY_MOCK_ENUM(np_internet_status, "Disconnected", "Connected")
POLICY_MOCK_ENUM(np_psn_status, "Disconnected", "Simulated", "RPCN")
POLICY_MOCK_ENUM(vk_gpu_scheduler_mode, "Safe", "Fast")
POLICY_MOCK_ENUM(msaa_level, "Disabled", "Auto")

#undef POLICY_MOCK_ENUM

namespace cfg
{
// The real Utilities/Config.h entry types.
enum class type : unsigned
{
    node = 0,
    _bool,
    _enum,
    _int,
    uint,
    uint128,
    string,
    set,
    map,
    node_map,
    log,
    device,
};

class _base;
inline std::vector<_base*>* g_registry = nullptr;
inline _base* g_section = nullptr; // node of the real tree the settings being constructed belong to

class _base
{
    std::string m_name;
    type m_type;
    _base* m_parent = g_section;

protected:
    struct unregistered {};
    _base(std::string name, type t, unregistered) : m_name(std::move(name)), m_type(t) {}

public:
    explicit _base(std::string name, type t)
        : m_name(std::move(name)), m_type(t)
    {
        if (g_registry) g_registry->push_back(this);
    }
    _base(const _base&) = delete;
    _base& operator=(const _base&) = delete;
    virtual ~_base() = default;

    const std::string& get_name() const { return m_name; }
    type get_type() const { return m_type; }
    _base* get_parent() const { return m_parent; }
    virtual std::string to_string() const = 0;
    virtual bool from_string(std::string_view value, bool dynamic = false) = 0;
    virtual void from_default() = 0;
    virtual std::vector<std::string> to_list() const { return {}; }
    virtual bool from_list(std::vector<std::string>&&) { return false; }
};

// Never instantiated by the mock (its tree is flat); policy code walks get_nodes() and recurses into nodes.
class node : public _base
{
    std::vector<_base*> m_nodes;

public:
    const std::vector<_base*>& get_nodes() const { return m_nodes; }
};

// A node name of the real tree ("Core", "Video" > "Vulkan"): the parent of the settings constructed after it, so
// get_parent() gives the real keys ("Video > Vulkan > VRAM allocation limit (MB)"). Not part of the flat registry.
class section final : public _base
{
public:
    explicit section(std::string name) : _base(std::move(name), type::node, unregistered{}) { g_section = this; }
    std::string to_string() const override { return {}; }
    bool from_string(std::string_view, bool = false) override { return false; }
    void from_default() override {}
};

// Ends the innermost section: the next settings belong to the enclosing one.
struct section_end
{
    section_end() { g_section = g_section ? g_section->get_parent() : nullptr; }
};

class _bool final : public _base
{
    bool m_value, m_def;

public:
    explicit _bool(std::string name, bool def = false) : _base(std::move(name), type::_bool), m_value(def), m_def(def) {}
    bool get() const { return m_value; }
    explicit operator bool() const { return m_value; } // explicit, as in Utilities/Config.h
    void set(bool value) { m_value = value; }
    std::string to_string() const override { return m_value ? "true" : "false"; }
    bool from_string(std::string_view value, bool = false) override
    {
        if (value != "true" && value != "false") return false;
        m_value = value == "true";
        return true;
    }
    void from_default() override { m_value = m_def; }
};

template <s64 Min, s64 Max>
class _int final : public _base
{
    s64 m_value, m_def;

public:
    explicit _int(std::string name, s64 def = std::clamp<s64>(0, Min, Max)) : _base(std::move(name), type::_int), m_value(def), m_def(def) {}
    s64 get() const { return m_value; }
    operator s64() const { return m_value; }
    void set(const s64& value)
    {
        if (value < Min || value > Max) throw std::out_of_range("out of range: " + get_name());
        m_value = value;
    }
    std::string to_string() const override { return std::to_string(m_value); }
    bool from_string(std::string_view value, bool = false) override
    {
        s64 result = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (ec != std::errc{} || end != value.data() + value.size() || result < Min || result > Max) return false;
        m_value = result;
        return true;
    }
    void from_default() override { m_value = m_def; }
};

template <u64 Min, u64 Max>
class uint final : public _base
{
    u64 m_value, m_def;

public:
    explicit uint(std::string name, u64 def = Min) : _base(std::move(name), type::uint), m_value(def), m_def(def) {}
    u64 get() const { return m_value; }
    operator u64() const { return m_value; }
    void set(const u64& value)
    {
        if (value < Min || value > Max) throw std::out_of_range("out of range: " + get_name());
        m_value = value;
    }
    std::string to_string() const override { return std::to_string(m_value); }
    bool from_string(std::string_view value, bool = false) override
    {
        u64 result = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (ec != std::errc{} || end != value.data() + value.size() || result < Min || result > Max) return false;
        m_value = result;
        return true;
    }
    void from_default() override { m_value = m_def; }
};

template <typename T>
class _enum final : public _base
{
    T m_value, m_def;

public:
    explicit _enum(std::string name, T def) : _base(std::move(name), type::_enum), m_value(def), m_def(def) {}
    T get() const { return m_value; }
    operator T() const { return m_value; }
    void set(T value) { m_value = value; }
    std::string to_string() const override { return std::string(enum_text<T>::names[static_cast<std::size_t>(m_value)]); }
    bool from_string(std::string_view value, bool = false) override
    {
        std::size_t i = 0;
        for (const std::string_view name : enum_text<T>::names)
        {
            if (name == value)
            {
                m_value = static_cast<T>(i);
                return true;
            }
            i++;
        }
        return false;
    }
    void from_default() override { m_value = m_def; }
};

class string final : public _base
{
    std::string m_value, m_def;

public:
    explicit string(std::string name, std::string def = {}) : _base(std::move(name), type::string), m_value(def), m_def(std::move(def)) {}
    std::string get() const { return m_value; }
    void set(std::string value) { m_value = std::move(value); }
    std::string to_string() const override { return m_value; }
    bool from_string(std::string_view value, bool = false) override
    {
        m_value = std::string(value);
        return true;
    }
    void from_default() override { m_value = m_def; }
};

// Like the real cfg::set_entry: no text form of its own (to_string() empty, from_string() refused), only lists.
class set_entry final : public _base
{
    std::set<std::string> m_set;

public:
    explicit set_entry(std::string name) : _base(std::move(name), type::set) {}
    const std::set<std::string>& get_set() const { return m_set; }
    std::string to_string() const override { return {}; }
    bool from_string(std::string_view, bool = false) override { return false; }
    void from_default() override { m_set.clear(); }
    std::vector<std::string> to_list() const override { return {m_set.begin(), m_set.end()}; }
    bool from_list(std::vector<std::string>&& list) override
    {
        m_set = {list.begin(), list.end()};
        return true;
    }
    void add(std::string item) { m_set.insert(std::move(item)); }
};
} // namespace cfg

struct cfg_registry
{
    std::vector<cfg::_base*> m_settings;
    cfg_registry() { cfg::g_registry = &m_settings; }
    cfg_registry(const cfg_registry&) = delete;
    cfg_registry& operator=(const cfg_registry&) = delete;
};

struct cfg_root : cfg_registry
{
    struct node_core
    {
        cfg::section m_node{"Core"};
        cfg::_enum<ppu_decoder_type> ppu_decoder{"PPU Decoder", ppu_decoder_type::llvm};
        cfg::_enum<spu_decoder_type> spu_decoder{"SPU Decoder", spu_decoder_type::llvm};
        cfg::_int<1, 8> ppu_threads{"PPU Threads", 2};
        cfg::_int<0, 1024> llvm_threads{"Max LLVM Compile Threads", 0};
        cfg::string llvm_cpu{"Use LLVM CPU"};
        cfg::_bool llvm_precompilation{"LLVM Precompilation", true};
        cfg::_bool spu_cache{"SPU Cache", true};
        cfg::_bool ppu_debug{"PPU Debug"};
        cfg::_bool spu_debug{"SPU Debug"};
        cfg::_bool mfc_debug{"MFC Debug"};
        cfg::_bool ppu_prof{"PPU Profiler"};
        cfg::_bool spu_prof{"SPU Profiler"};
        cfg::_bool llvm_logs{"Save LLVM logs"};
        cfg::_enum<sleep_timers_accuracy_level> sleep_timers_accuracy{"Sleep Timers Accuracy", sleep_timers_accuracy_level::_as_host};
        cfg::uint<0, 100> spu_getllar_busy_waiting_percentage{"SPU GETLLAR Busy Waiting Percentage", 100};
        cfg::_bool spu_getllar_spin_optimization_disabled{"Disable SPU GETLLAR Spin Optimization"};
        cfg::_bool spu_reservation_busy_waiting_enabled{"SPU Reservation Busy Waiting Enabled"};
        cfg::_enum<thread_scheduler_mode> thread_scheduler{"Thread Scheduler Mode", thread_scheduler_mode::os};
        cfg::_int<0, 6> max_spurs_threads{"Max SPURS Threads", 6};
        cfg::_int<0, 6> preferred_spu_threads{"Preferred SPU Threads", 0};
        cfg::_enum<spu_block_size_type> spu_block_size{"SPU Block Size", spu_block_size_type::safe};
        cfg::_enum<xfloat_accuracy> spu_xfloat_accuracy{"SPU XFloat Accuracy", xfloat_accuracy::approximate};
        cfg::_bool spu_accurate_reservations{"Accurate SPU Reservations", true};
        cfg::_bool rsx_accurate_res_access{"Accurate RSX reservation access"};
        cfg::_bool use_accurate_dfma{"Use Accurate DFMA", true};
        cfg::set_entry libraries_control{"Libraries Control"};
        cfg::_bool external_debugger{"Assume External Debugger"};
        cfg::section_end m_node_end;
    } core;

    struct node_video
    {
        cfg::section m_node{"Video"};
        cfg::_enum<video_renderer> renderer{"Renderer", video_renderer::vulkan};
        cfg::_enum<video_resolution> resolution{"Resolution", video_resolution::_720p};
        cfg::uint<25, 800> resolution_scale_percent{"Resolution Scale", 100};
        cfg::_enum<frame_limit_type> frame_limit{"Frame limit", frame_limit_type::_auto};
        cfg::_enum<shader_mode> shadermode{"Shader Mode", shader_mode::async_with_interpreter};
        cfg::_int<0, 16> shader_compiler_threads_count{"Shader Compiler Threads", 0};
        cfg::_bool multithreaded_rsx{"Multithreaded RSX"};
        cfg::_bool write_color_buffers{"Write Color Buffers"};
        cfg::_bool write_depth_buffer{"Write Depth Buffer"};
        cfg::_bool read_color_buffers{"Read Color Buffers"};
        cfg::_bool read_depth_buffer{"Read Depth Buffer"};
        cfg::_bool strict_rendering_mode{"Strict Rendering Mode"};
        cfg::_bool strict_texture_flushing{"Strict Texture Flushing"};
        cfg::_bool precise_zpass_count{"Accurate ZCULL stats", true};
        cfg::_bool relaxed_zcull_sync{"Relaxed ZCULL Sync"};
        cfg::_bool debug_output{"Debug output"};
        cfg::_bool log_programs{"Log shader programs"};
        cfg::_bool disable_vulkan_mem_allocator{"Disable Vulkan Memory Allocator"};
        cfg::_enum<msaa_level> antialiasing_level{"MSAA", msaa_level::_auto};
        cfg::_bool disable_vertex_cache{"Disable Vertex Cache"};

        struct node_vk
        {
            cfg::section m_node{"Vulkan"};
            cfg::_bool asynchronous_texture_streaming{"Asynchronous Texture Streaming"};
            cfg::_enum<vk_gpu_scheduler_mode> asynchronous_scheduler{"Asynchronous Queue Scheduler", vk_gpu_scheduler_mode::safe};
            cfg::uint<256, 65536> vram_allocation_limit{"VRAM allocation limit (MB)", 65536};
            cfg::section_end m_node_end;
        } vk;
        cfg::section_end m_node_end;
    } video;

    struct node_audio
    {
        cfg::section m_node{"Audio"};
        cfg::_bool enable_buffering{"Enable Buffering", true};
        cfg::_int<4, 250> desired_buffer_duration{"Desired Audio Buffer Duration", 100};
        cfg::_bool enable_time_stretching{"Enable Time Stretching"};
        cfg::_bool convert_to_s16{"Convert to 16 bit"};
        cfg::_bool disable_sampling_skip{"Disable Sampling Skip"};
        cfg::section_end m_node_end;
    } audio;

    struct node_vfs
    {
        cfg::section m_node{"VFS"};
        cfg::_bool host_root{"Enable /host_root/"};
        cfg::_bool init_dirs{"Initialize Directories", true};
        cfg::_bool limit_cache_size{"Limit disk cache size"};
        cfg::_int<0, 10240> cache_max_size{"Disk cache maximum size (MB)", 5120};
        cfg::section_end m_node_end;
    } vfs;

    struct node_net
    {
        cfg::section m_node{"Net"};
        cfg::_enum<np_internet_status> net_active{"Internet enabled", np_internet_status::disabled};
        cfg::_bool upnp_enabled{"UPNP Enabled"};
        cfg::_enum<np_psn_status> psn_status{"PSN status", np_psn_status::disabled};
        cfg::section_end m_node_end;
    } net;

    struct node_io
    {
        cfg::section m_node{"Input/Output"};
        cfg::uint<0, 100000> pad_sleep{"Pad handler sleep (microseconds)", 1000};
        cfg::section_end m_node_end;
    } io;

    struct node_sys
    {
        cfg::section m_node{"System"};
        cfg::_enum<CellSysutilLang> language{"Language", CellSysutilLang{1}};
        cfg::section_end m_node_end;
    } sys;

    struct node_misc
    {
        cfg::section m_node{"Miscellaneous"};
        cfg::_bool autostart{"Automatically start games after boot", true};
        cfg::_bool autoexit{"Exit RPCS3 when process finishes"};
        cfg::_bool use_native_interface{"Use native user interface", true};
        cfg::section_end m_node_end;
    } misc;

    struct registry_end
    {
        registry_end() { cfg::g_registry = nullptr; }
    } m_registry_end;

    // The flat mock has no section nodes: the root lists every setting.
    const std::vector<cfg::_base*>& get_nodes() const { return m_settings; }

    std::string to_string() const
    {
        std::string text;
        for (const cfg::_base* setting : m_settings)
        {
            if (setting->get_type() != cfg::type::set)
            {
                text += setting->get_name() + ": " + setting->to_string() + "\n";
                continue;
            }
            const std::vector<std::string> items = setting->to_list();
            text += setting->get_name() + (items.empty() ? ": []\n" : ":\n");
            for (const std::string& item : items) text += "  - " + item + "\n";
        }
        return text;
    }

    // Applies every known "Name: value" line; unknown names and section headers are skipped. False when a value
    // was rejected.
    bool from_string(std::string_view text)
    {
        return parse(text, false);
    }

    // Like cfg::node::validate: also false for an unknown setting name; the known values are applied all the same.
    bool validate(std::string_view text)
    {
        return parse(text, true);
    }

    void from_default()
    {
        for (cfg::_base* setting : m_settings) setting->from_default();
    }

private:
    cfg::_base* find(std::string_view name) const
    {
        for (cfg::_base* setting : m_settings)
            if (setting->get_name() == name) return setting;
        return nullptr;
    }

    bool parse(std::string_view text, bool strict)
    {
        bool ok = true;
        cfg::_base* list = nullptr; // set entry whose "- item" lines follow
        std::vector<std::string> items;
        const auto end_list = [&]()
        {
            if (list) list->from_list(std::move(items));
            list = nullptr;
            items.clear();
        };
        while (!text.empty())
        {
            const auto nl = text.find('\n');
            std::string_view line = text.substr(0, nl);
            text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
            while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            if (line.empty()) continue;
            if (list && line.starts_with("- "))
            {
                items.emplace_back(line.substr(2));
                continue;
            }
            end_list();
            if (line.back() == ':')
            {
                // A section header, or a set entry whose items follow.
                cfg::_base* setting = find(line.substr(0, line.size() - 1));
                if (setting && setting->get_type() == cfg::type::set) list = setting;
                continue;
            }
            const auto colon = line.find(": ");
            if (colon == std::string_view::npos) continue;
            const std::string_view name = line.substr(0, colon);
            std::string_view value = line.substr(colon + 2);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            bool known = false;
            for (cfg::_base* setting : m_settings)
            {
                if (setting->get_name() != name) continue;
                known = true;
                if (setting->get_type() == cfg::type::set) ok &= value == "[]" && setting->from_list({});
                else ok &= setting->from_string(value);
            }
            if (strict && !known) ok = false;
        }
        end_list();
        return ok;
    }
};
