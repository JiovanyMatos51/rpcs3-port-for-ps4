#pragma once

#include "Emu/system_config.h"
#include "Emu/system_utils.hpp"
#include "Utilities/File.h"
#include "util/ps4_cfg_browser.hpp"
#include "util/ps4_cfg_catalog.hpp"
#include "util/ps4_cool_mode.hpp"
#include "util/ps4_i18n.hpp"
#include "util/ps4_policy.hpp"
#include "util/ps4_xmb.hpp"
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>

// 0.39: every text is resolved in the interface language when it is shown (rows() runs every XMB frame), so a
// language change with the menu open shows at once. startup.log lines stay in English.

namespace utils::ps4
{
// 0.31: the online switches, shared by the settings menu and the PlayStation Network column of the XMB. Texts
// stay within the XMB font (Latin-1, the Arabic forms and a few symbols): '>' instead of '›'.
namespace online_rows
{
using i18n::str;

inline std::string online_title(bool on)
{
    return i18n::tr(on ? str::SET_MODO_ONLINE_RPCN_LIGADO : str::SET_MODO_ONLINE_RPCN_DESLIGADO);
}

inline std::string online_help(bool on)
{
    return i18n::tr(on ? str::SET_TODOS_JOGOS_CONECTAM_RPCN_CONTA : str::SET_JOGOS_FICAM_OFFLINE_LIGA_RPCN);
}

inline std::string upnp_title(bool on)
{
    return i18n::tr(on ? str::SET_UPNP_ROTEADOR_LIGADO : str::SET_UPNP_ROTEADOR_DESLIGADO);
}

inline std::string upnp_help(bool online, bool on)
{
    if (!online) return i18n::tr(on ? str::SET_VALE_QUANDO_MODO_ONLINE_ESTIVER : str::SET_SO_FUNCIONA_MODO_ONLINE);
    return i18n::tr(on ? str::SET_ROTEADOR_ABRE_PORTAS_JOGOS_ONLINE : str::SET_PORTAS_ROTEADOR_FECHADAS_PADRAO_LIGA);
}

// Flips online-rpcn.flag. Returns the message to show; 'event' gets a startup.log line (left empty when
// the file could not be written).
inline std::string toggle_online(std::string& event)
{
    const bool enable = !online_enabled();
    if (!set_online_enabled(enable)) return i18n::tr(str::COM_ERRO_GRAVAR) + std::string(online_rpcn_flag);
    event = enable ? "settings: online (RPCN) switched ON (online-rpcn.flag created)"
                   : "settings: online (RPCN) switched OFF (online-rpcn.flag removed)";
    return i18n::tr(enable ? str::SET_MODO_ONLINE_LIGADO_JOGOS_CONECTAM : str::SET_MODO_ONLINE_DESLIGADO_JOGOS_FICAM);
}

// Flips upnp.flag (same contract as toggle_online).
inline std::string toggle_upnp(std::string& event)
{
    const bool enable = !upnp_enabled();
    if (!set_upnp_enabled(enable)) return i18n::tr(str::COM_ERRO_GRAVAR) + std::string(upnp_flag);
    event = enable ? "settings: UPnP switched ON (upnp.flag created)" : "settings: UPnP switched OFF (upnp.flag removed)";
    if (!enable) return i18n::tr(str::SET_UPNP_DESLIGADO_ROTEADOR_NAO_RECEBE);
    return i18n::tr(online_enabled() ? str::SET_UPNP_LIGADO_ROTEADOR_ABRE_PORTAS : str::SET_UPNP_LIGADO_VALE_QUANDO_MODO);
}

// Rows for the PlayStation Network column (paths "@rpcn:online" and "@rpcn:upnp").
inline xmb::entry online_entry(bool on)
{
    return {"@rpcn:online", online_title(on), online_help(on), "tex_psn", {}};
}

inline xmb::entry upnp_entry(bool online, bool on)
{
    return {"@rpcn:upnp", upnp_title(on), upnp_help(online, on), "tex_psn", {}};
}
} // namespace online_rows

// 0.34: the "Modo frio" switch (util/ps4_cool_mode.hpp), next to the online switches in the settings menu. It only
// changes how the emulator waits and how many background compiles it runs; never the fan, clocks or the system.
namespace cool_rows
{
using i18n::str;

inline std::string title(bool on)
{
    return i18n::tr(on ? str::SET_MODO_FRIO_MENOS_CALOR : str::SET_MODO_FRIO_MENOS_CALOR_2);
}

// The subtitle fits one XMB line (~830 px at 17 px); "vale a partir do próximo jogo" is in the toggle message.
inline const char* help()
{
    return i18n::tr(str::SET_MENOS_CPU_GIRANDO_TOA_MENOS);
}

// Flips cool-mode.flag (same contract as online_rows::toggle_online).
inline std::string toggle(std::string& event)
{
    const bool enable = !cool_mode::refresh();
    const bool written = cool_mode::set_enabled(enable);
    cool_mode::refresh(); // the state the file system really has
    if (!written) return i18n::tr(str::COM_ERRO_GRAVAR) + std::string(cool_mode::flag_path);
    event = enable ? "settings: cool mode (Modo frio) switched ON (cool-mode.flag created)"
                   : "settings: cool mode (Modo frio) switched OFF (cool-mode.flag removed)";
    return i18n::tr(enable ? str::SET_MODO_FRIO_LIGADO_VALE_PARTIR : str::SET_MODO_FRIO_DESLIGADO_VELOCIDADE_NORMAL);
}
} // namespace cool_rows

// 0.34: two switches next to "Modo frio", read at the start of every game. "Evitar piscadas" writes
// /data/rpcs3/present-hold-ms.txt (the Vulkan renderer keeps showing the last complete frame, up to that many ms, while
// a frame lost draws to pipelines still compiling). Subtitles fit one XMB line (<= 830 px at 17 px); the rest of the
// explanation is in the toggle messages.
// 0.39: "Áudio contínuo" was automatic by default (cellAudio slows the sound down a little, never below 70 %, while the
// game falls behind); 'Desligado' created /data/rpcs3/audio-no-stretch.flag.
// 0.40: the row follows the audio_continuity row of util/ps4_safe_mode.hpp, off by default (the 0.38 controller).
// 'Automático' creates its enable flag audio-continuity.flag and removes audio-no-stretch.flag (its kill flag, which a
// 0.39 'Desligado' left behind); 'Desligado' removes the enable flag, the 0.34-0.38 alias audio-auto-stretch.flag and
// the row's name in features.txt, and writes the kill flag only when features.txt 'all' would still turn it on.
namespace scene_audio_rows
{
using i18n::str;

inline constexpr char present_hold_path[] = "/data/rpcs3/present-hold-ms.txt";
inline constexpr std::string_view present_hold_text = "750\n";

// A file of the feature rows (same directory as util/ps4_safe_mode.hpp reads)
inline std::string feature_file(const char* name)
{
    return std::string(RPCS3_PS4_SAFE_MODE_DIR) + "/" + name;
}

// Same reading as the renderer (VKPresent.cpp): a file below 16 bytes starting with a number; 1..3000 holds frames.
inline bool present_hold_enabled()
{
    const fs::file file(present_hold_path);
    if (!file || file.size() >= 16) return false;
    const std::string text = file.to_string();
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    return end != text.c_str() && value >= 1 && value <= 3000;
}

// What the next boot of a game without a safe-mode level runs (cellAudio reads the row once per boot): the enable
// flag, its alias, features.txt and the kill flag; the global safe mode has its own row and is left out here.
inline bool audio_stretch_enabled()
{
    safe_mode::inputs files = safe_mode::read_inputs();
    files.global_safe = false;
    return safe_mode::enabled_in(safe_mode::compute(files), safe_mode::feature::audio_continuity);
}

inline std::string present_hold_title(bool on)
{
    return i18n::tr(on ? str::SET_EVITAR_PISCADAS_CENAS_LIGADO : str::SET_EVITAR_PISCADAS_CENAS_DESLIGADO);
}

inline const char* present_hold_help()
{
    return i18n::tr(str::SET_CENA_VISTA_PRIMEIRA_VEZ_SEGURA);
}

inline std::string audio_stretch_title(bool on)
{
    return i18n::tr(on ? str::SET_AUDIO_CONTINUO_AUTOMATICO : str::SET_AUDIO_CONTINUO_DESLIGADO);
}

inline const char* audio_stretch_help()
{
    return i18n::tr(str::SET_AUDIO_CONTINUO_HELP);
}

// Writes present-hold-ms.txt with 750 or removes it. 'on' gets the state the file system really has; the contract
// is that of cool_rows::toggle (message returned, 'event' left empty when the file could not be changed).
inline std::string toggle_present_hold(bool& on, std::string& event)
{
    const bool enable = !present_hold_enabled();
    const bool written = enable ? policy_detail::write_note(present_hold_path, present_hold_text)
                                : (!fs::is_file(present_hold_path) || fs::remove_file(present_hold_path));
    on = present_hold_enabled();
    if (!written || on != enable) return i18n::tr(str::COM_ERRO_GRAVAR) + std::string(present_hold_path);
    event = enable ? "settings: Evitar piscadas switched ON (present-hold-ms.txt = 750)"
                   : "settings: Evitar piscadas switched OFF (present-hold-ms.txt removed)";
    return i18n::tr(enable ? str::SET_EVITAR_PISCADAS_LIGADO_PEQUENA_PAUSA : str::SET_EVITAR_PISCADAS_DESLIGADO_CENAS_NOVAS);
}

// 'Automático' / 'Desligado' (same contract as toggle_present_hold; 'on' = automatic continuity).
inline std::string toggle_audio_stretch(bool& on, std::string& event)
{
    const safe_mode::info& row = safe_mode::row(safe_mode::feature::audio_continuity);
    const std::string enable_path = feature_file(row.enable_flag), kill_path = feature_file(row.kill_flag);
    const auto drop = [](const std::string& path) { return !fs::is_file(path) || fs::remove_file(path); };
    const bool enable = !audio_stretch_enabled();
    bool written = true;
    bool killed = false;
    if (enable)
    {
        static constexpr std::string_view note = "Created by the PS4 settings menu ('Audio continuo: Automatico'): cellAudio slows the sound "
            "down a little (never below 70 %) while the game falls behind the audio clock. Delete it or switch it back to Desligado in the menu.\n";
        written = drop(kill_path) && policy_detail::write_note(enable_path, note);
    }
    else
    {
        written = drop(enable_path);
        for (const safe_mode::alias& a : safe_mode::aliases)
            if (a.f == row.f) written = drop(feature_file(a.enable_flag)) && written;
        written = safe_mode::features_file::set(row.name, false) && written;
        // features.txt 'all' still turns every row on: only the kill flag keeps this one off
        if (written && audio_stretch_enabled())
        {
            static constexpr std::string_view note = "Created by the PS4 settings menu ('Audio continuo: Desligado') while features.txt says 'all': "
                "cellAudio never slows the sound down. Delete it or switch it back to Automatico in the menu.\n";
            written = policy_detail::write_note(kill_path, note);
            killed = true;
        }
    }
    on = audio_stretch_enabled();
    if (!written || on != enable) return i18n::tr(str::COM_ERRO_GRAVAR) + (enable ? enable_path : kill_path);
    event = enable ? "settings: Audio continuo switched to AUTOMATIC (audio-continuity.flag created, audio-no-stretch.flag removed)"
        : killed ? "settings: Audio continuo switched OFF (audio-continuity.flag removed; audio-no-stretch.flag created: features.txt says all)"
                 : "settings: Audio continuo switched OFF (audio-continuity.flag removed; the 0.38 default)";
    return i18n::tr(enable ? str::SET_AUDIO_CONTINUO_AUTO_MSG : str::SET_AUDIO_CONTINUO_OFF_MSG);
}
} // namespace scene_audio_rows

// 0.39: 'Idioma dos jogos' (the language PS3 games get, util/ps4_policy.hpp game_language): global row values
// automatic, then CELL_SYSUTIL_LANG_* 0..19; a game's row starts with 'Igual ao global' (no marker).
// 0.40: 'Automático' is English (US) again; the value 'Idioma do PS4 (<nome>)' (game-language.txt 'ps4') follows the PS4
// language as 0.39's automatic value did, and a safe-mode level turns it off for that boot.
namespace game_language_rows
{
using i18n::str;

// Name of a value in the interface language: "Automático (Inglês (EUA))", "Idioma do PS4 (Português (Brasil))",
// "Francês", ...
inline std::string label(std::int32_t value, std::int32_t automatic, std::int32_t system)
{
    if (value == game_language_system) return i18n::trf(str::GL_PS4_FMT, {i18n::game_language_name(system)});
    if (value < 0) return i18n::trf(str::LANG_AUTO_FMT, {i18n::game_language_name(automatic)});
    return i18n::tr(i18n::game_language_name(value));
}

// The choices of a row in order: nullopt = the global choice (game rows only), then automatic, the PS4 language, then
// 0..19
inline std::vector<std::optional<std::int32_t>> choices(bool game)
{
    std::vector<std::optional<std::int32_t>> out;
    if (game) out.push_back(std::nullopt);
    out.push_back(game_language_auto);
    out.push_back(game_language_system);
    for (std::int32_t v = 0; v < game_language_count; ++v) out.push_back(v);
    return out;
}
} // namespace game_language_rows

// 0.40: the global 'Modo seguro' row (PS4 tab: /data/rpcs3/safe-mode.flag, level 1 for every game, enable flags and
// features.txt ignored) and the per-game 'Modo seguro deste jogo' row (the title's adapt file: safe=0, or fail=user and
// safe=N, util/ps4_adapt.hpp set_user_level). Both apply from the next boot.
namespace safe_mode_rows
{
using i18n::str;

inline std::string global_title(bool on)
{
    return i18n::tr(on ? str::SET_MODO_SEGURO_LIGADO : str::SET_MODO_SEGURO_DESLIGADO);
}

inline const char* global_help()
{
    return i18n::tr(str::SET_MODO_SEGURO_HELP);
}

// Flips safe-mode.flag (same contract as online_rows::toggle_online).
inline std::string toggle_global(std::string& event)
{
    const bool enable = !safe_mode::global_on();
    if (!safe_mode::set_global(enable) || safe_mode::global_on() != enable)
        return i18n::tr(str::COM_ERRO_GRAVAR) + scene_audio_rows::feature_file(safe_mode::global_flag_name);
    event = enable ? "settings: Modo seguro switched ON (safe-mode.flag created)" : "settings: Modo seguro switched OFF (safe-mode.flag removed)";
    return i18n::tr(enable ? str::SET_MODO_SEGURO_LIGADO_MSG : str::SET_MODO_SEGURO_DESLIGADO_MSG);
}

// "Desligado" or "Nível 2"
inline std::string level_name(unsigned level)
{
    if (!level) return i18n::tr(str::COM_DESLIGADO);
    return i18n::trf(str::SET_MODO_SEGURO_NIVEL, {level});
}

inline std::string game_title(unsigned level)
{
    return i18n::trf(str::SET_MODO_SEGURO_JOGO, {i18n::raw(level_name(level))});
}

// The subtitle: what the row does at level 0, else why the title is at its level and for how many boots
inline std::string game_help(const runtime_inputs& in)
{
    if (!in.safe_level) return i18n::tr(str::SET_MODO_SEGURO_JOGO_HELP);
    if (in.safe_exhausted) return i18n::tr(str::SET_MODO_SEGURO_JOGO_EXHAUSTED_HELP);
    if (in.safe_kind == safe_mode::fail_kind::user || in.safe_kind == safe_mode::fail_kind::none)
        return i18n::trf(str::SET_MODO_SEGURO_JOGO_USER_HELP, {in.safe_boots_left});
    return i18n::trf(str::SET_MODO_SEGURO_JOGO_AUTO_HELP, {fail_text(in.safe_kind), in.safe_boots_left});
}
} // namespace safe_mode_rows

// The settings menu (Configurações > Configurações do RPCS3, and Square on a game), a stopped-game editor of a staged
// copy of the RPCS3 config: cancelled edits never reach the live config. The top level holds Salvar, the automatic
// configuration switch, this game's mode and the 10 tabs of the PC dialog; CPU .. Depuração are pages of the settings
// browser (util/ps4_cfg_browser.hpp: every RPCS3 setting, the value that will run and who decides it, all parsers and
// bounds from cfg), the PS4 tab holds the port's own switches. Global scope: /data/rpcs3/ps4-settings.yml, a full dump;
// per game: custom_configs/config_<TITLE_ID>.yml with only the player's own choices (settings format 2). Both files list
// those choices as pins, which the automatic configuration leaves alone (util/ps4_policy.hpp). A per-game YAML of format 1
// (older menus, PC copies) keeps its full-dump rules until 'Modo deste jogo' converts it.
class settings_menu
{
    using str = i18n::str;

public:
    using tab = cfg_catalog::tab;

private:
    enum class top_action { save, auto_switch, use_saved, back_to_auto, adapt, game_mode, safe_level, game_features, tab, apply_profile, reset, remove_game };
    enum class ps4_row { players, online, upnp, cool, present_hold, audio_stretch, game_language, safe_mode };
    static constexpr ps4_row ps4_rows[] = {ps4_row::players, ps4_row::online, ps4_row::upnp, ps4_row::cool, ps4_row::present_hold,
        ps4_row::audio_stretch, ps4_row::game_language, ps4_row::safe_mode};
    // A top-level action that changes files only after a second X on the same row
    enum class pending_action { none, back_to_auto, convert, remove_game, use_saved };
    // A list edit waiting for its warning's confirmation: X on the same row confirms it and repeats the edit
    struct retry_edit
    {
        std::size_t row;
        int direction;
        bool coarse;
    };

    cfg_root m_config;
    cfg_browser::model m_browser;
    std::string m_path, m_title, m_global_baseline, m_event;
    policy_mode m_mode = policy_mode::automatic;
    database_use m_database = database_use::none; // what the PC config database step does for this game
    bool m_auto_enabled = true, m_user_tuned = false;
    bool m_game_file = false, m_game_format2 = false; // the game's YAML exists / is of settings format 2
    int m_tab = -1;                                   // the open tab (-1 = top level)
    std::size_t m_selection = 0;
    bool m_show_hidden = false;                       // Depuração > Mostrar opções ocultas, this session only
    pending_action m_pending = pending_action::none;
    std::size_t m_pending_row = 0;
    std::vector<std::string> m_convert_keys;          // a format-1 game YAML: the settings its conversion pins
    std::vector<std::string> m_legacy_keys;           // the global layer's own values the automatic configuration replaces
    u64 m_title_features = 0;                         // the rows custom_configs/<TITLE_ID>.features turns on
    std::optional<retry_edit> m_retry;
    std::vector<std::size_t> m_list;                  // the page row of each list row (section captions left out)
    runtime_inputs m_inputs;                          // flags and adaptation keys read by open() and the switches
    std::string m_players;                            // ps4_pads::summary() from the launcher
    int m_players_connected = -1;                     // -1 = unknown
    bool m_present_hold = false;                      // present-hold-ms.txt state, read by open() and the switch
    bool m_audio_stretch = false;                     // automatic audio continuity (audio-continuity.flag, 0.40: off by default)
    // 'Idioma dos jogos': the global choice and, in a game's menu, the game's own (nullopt = follows the global)
    std::int32_t m_game_language_global = game_language_auto;
    std::optional<std::int32_t> m_game_language_game;
    inline static bool s_self_checked = false;        // the self-check runs on the first open() of the app session

    // Unavailable desktop backends cannot be offered or saved silently.
    static void fix_backends([[maybe_unused]] cfg_root& cfg)
    {
#ifndef LLVM_AVAILABLE
        if (cfg.core.ppu_decoder == ppu_decoder_type::llvm) cfg.core.ppu_decoder.set(ppu_decoder_type::_static);
        if (cfg.core.spu_decoder == spu_decoder_type::llvm) cfg.core.spu_decoder.set(spu_decoder_type::asmjit);
#endif
    }

    // Every setting of this build has a catalog row and survives its own text: one startup.log line, the first problems,
    // and a toast when there is one.
    void self_check()
    {
        const cfg_browser::self_check_result r = cfg_browser::model::self_check(m_config);
        const std::string line = "cfg browser: self-check " + std::to_string(r.settings) + " settings, " + std::to_string(r.uncatalogued) +
            " without catalog row, " + std::to_string(r.round_trip_failures) + " round-trip failures";
        ps4_startup_checkpoint(line.c_str());
        for (std::size_t i = 0; i < r.problems.size() && i < 8; ++i) ps4_startup_checkpoint(("cfg browser: " + r.problems[i]).c_str());
        if (const unsigned problems = r.uncatalogued + r.round_trip_failures) ps4_notify(i18n::trf(str::CFGU_SELFCHECK_FAIL, {problems}).c_str());
    }

    // The game language row: its current value and its title in the interface language
    std::optional<std::int32_t> game_language_value() const
    {
        return m_title.empty() ? std::optional<std::int32_t>(m_game_language_global) : m_game_language_game;
    }

    std::string game_language_label(std::optional<std::int32_t> value) const
    {
        const std::string global = game_language_rows::label(m_game_language_global, m_inputs.game_language_auto, m_inputs.game_language_system);
        if (!value) return i18n::trf(str::GL_SAME_AS_GLOBAL, {i18n::raw(global)});
        return game_language_rows::label(*value, m_inputs.game_language_auto, m_inputs.game_language_system);
    }

    std::string game_language_title() const
    {
        return i18n::trf(m_title.empty() ? str::GL_TITLE_GLOBAL : str::GL_TITLE_GAME, {i18n::raw(game_language_label(game_language_value()))});
    }

    // Moves the row to the next (direction 1) or previous value and writes it at once
    std::string step_game_language(int direction)
    {
        const auto values = game_language_rows::choices(!m_title.empty());
        const auto now = game_language_value();
        const auto it = std::find(values.begin(), values.end(), now);
        const int index = it == values.end() ? 0 : int(it - values.begin());
        const auto next = values[std::size_t((index + direction + int(values.size())) % int(values.size()))];
        if (!set_game_language(m_title, next))
            return i18n::tr(str::COM_ERRO_GRAVAR) + (m_title.empty() ? std::string(game_language_path) : game_language_marker(m_title));
        if (m_title.empty()) m_game_language_global = next.value_or(game_language_auto);
        else m_game_language_game = next;
        refresh_runtime();
        char token[4];
        m_event = "settings: game language " + (m_title.empty() ? std::string("(global game-language.txt)") : m_title + " (game-language marker)") + " = " +
            (next ? std::string(game_language_token(*next, token)) : std::string("global"));
        std::string message = i18n::trf(str::GL_CHANGED, {i18n::raw(game_language_label(next))});
        if (m_mode == policy_mode::keep_user_settings) message += i18n::tr(str::SET_SEM_EFEITO_KEEP_USER_SETTINGS);
        return message;
    }

    // The per-game menu shows the adaptation of its game (none for the global menu or the keep flag). 0.40: a lone
    // 'safe=0' (the player turned the per-game safe mode off) adjusts nothing.
    bool show_adapt() const
    {
        if (m_title.empty() || m_mode == policy_mode::keep_user_settings) return false;
        return std::any_of(m_inputs.adapt.begin(), m_inputs.adapt.end(), [](const std::string& key) { return key != adapt::key_safe0; });
    }

    // 'Modo seguro deste jogo' (every per-game menu: the feature rows of a level apply in every mode)
    bool show_safe_level() const
    {
        return !m_title.empty();
    }

    // X on 'Modo seguro deste jogo': Desligado -> Nível 1 -> 2 -> 3 -> Desligado, written at once (the player's choice,
    // fail=user), from the next boot of the game
    std::string step_safe_level()
    {
        const unsigned next = (unsigned{m_inputs.safe_level} + 1) % (unsigned{safe_mode::max_level} + 1);
        if (!adapt::set_user_level(m_title, next)) return i18n::tr(str::COM_ERRO_GRAVAR) + adapt::path(m_title);
        refresh_runtime();
        m_event = "settings: " + m_title + " per-game safe mode " + (next ? "level " + std::to_string(next) + " (adapt safe=0, fail=user, safe=" +
            std::to_string(next) + ")" : std::string("off (adapt safe=0)"));
        return i18n::trf(str::SET_MODO_SEGURO_JOGO_CHANGED, {m_title, i18n::raw(safe_mode_rows::level_name(next))});
    }

    // 0.31: what the active adaptation keys do, in one line. 0.40: the per-game safe mode first ("Modo seguro: nível 2
    // (a GPU travou)").
    std::string adapt_text() const
    {
        std::string text;
        const auto append = [&](str part) { text += text.empty() ? "" : ", "; text += i18n::tr(part); };
        if (m_inputs.safe_level) text = i18n::trf(str::SET_MODO_SEGURO_NIVEL_MOTIVO, {unsigned{m_inputs.safe_level}, fail_text(m_inputs.safe_kind)});
        if (adapt_fewer_threads(m_inputs)) append(str::SET_COMPILACAO_1_THREAD);
        if (adapt_has(m_inputs, adapt::key_vram_low)) append(str::SET_VRAM_REDUZIDA);
        if (online_blocked(m_inputs)) append(str::SET_ONLINE_DESLIGADO);
        if (adapt_no_shader_interpreter(m_inputs)) append(str::SET_GPU_TRAVOU_SHADERS);
        if (adapt_has(m_inputs, adapt::key_cb_split) && feature_on(m_inputs, safe_mode::feature::cb_split_after_loss)) append(str::SET_COMANDOS_GPU_DIVIDIDOS);
        if (text.empty()) text = i18n::tr(str::SET_REGISTRO_FALHA);
        return text + i18n::trf(str::SET_VALE_PROXIMOS_BOOT, {m_inputs.adapt_boots_left});
    }

    std::string players_title() const
    {
        if (m_players_connected < 0) return i18n::tr(str::COM_CONTROLES_CONECTADOS);
        return i18n::trf(str::SET_CONTROLES_CONECTADOS_4, {m_players_connected});
    }

    // 0.40: 'Novidades neste jogo' (custom_configs/<TITLE_ID>.features, util/ps4_safe_mode.hpp): the 0.39 audio rules for
    // this game only. The kill flags, the safe-mode levels and the global safe mode still win at boot.
    static constexpr u64 audio_039_rows = safe_mode::bit(safe_mode::feature::audio_wait_policy) | safe_mode::bit(safe_mode::feature::audio_hr_waits) |
        safe_mode::bit(safe_mode::feature::audio_reserve_64);

    std::string game_features_value() const
    {
        if ((m_title_features & audio_039_rows) == audio_039_rows) return i18n::tr(str::SET_NOVIDADES_AUDIO_039);
        // rows written by hand (console A/B tests): their names, which are the same in every language
        std::string names;
        for (const safe_mode::info& r : safe_mode::table)
            if (m_title_features & safe_mode::bit(r.f)) names += (names.empty() ? "" : ", ") + std::string(r.name);
        return names.empty() ? std::string(i18n::tr(str::SET_NOVIDADES_NENHUMA)) : names;
    }

    std::string game_features_title() const
    {
        return i18n::trf(str::CFGU_CHANGED, {str::SET_NOVIDADES_JOGO, i18n::raw(game_features_value())});
    }

    // X: nenhuma <-> the 0.39 audio rules; other names in the file stay
    std::string step_game_features()
    {
        const bool on = (m_title_features & audio_039_rows) != audio_039_rows;
        fs::create_path(rpcs3::utils::get_custom_config_dir());
        if (!safe_mode::title_features_file::set(m_title, audio_039_rows, on))
            return i18n::tr(str::COM_ERRO_GRAVAR) + rpcs3::utils::get_custom_config_dir() + m_title + ".features";
        m_title_features = safe_mode::title_features_file::named(m_title);
        refresh_runtime();
        m_event = "settings: " + m_title + " per-game features " + (on ? "audio_wait_policy, audio_hr_waits, audio_reserve_64 on" : "audio rows off") +
            " (custom_configs/" + m_title + ".features)";
        return i18n::trf(str::SET_NOVIDADES_CHANGED, {m_title, i18n::raw(game_features_value())});
    }

    // 0.40: 'Usar meus valores salvos (N)' (global menu, automatic configuration on): the global layer's own values the
    // automatic configuration replaces (util/ps4_policy.hpp legacy_choices); X twice pins them with the saved values
    std::vector<std::string> legacy_values() const
    {
        const auto saved = std::make_unique<cfg_root>();
        const auto automatic = std::make_unique<cfg_root>();
        saved->from_string(m_global_baseline);
        automatic->from_string(m_global_baseline);
        std::string layer = policy_detail::read_small(global_path, settings_text_limit);
        if (layer.empty()) layer = policy_detail::read_small(fs::get_config_dir(true) + "config.yml", settings_text_limit);
        std::vector<std::string> keys = legacy_choices(*saved, *automatic, m_inputs, layer, read_pins({}).global);
        // the settings the PS4 keeps at one value, or that have no meaning here, are never the player's
        std::erase_if(keys, [](const std::string& key)
        {
            const cfg_catalog::item* item = cfg_catalog::find(key);
            return !item || item->kind == cfg_catalog::klass::unavailable || item->kind == cfg_catalog::klass::hidden;
        });
        return keys;
    }

    bool show_use_saved() const
    {
        return m_title.empty() && !m_legacy_keys.empty();
    }

    // "Frame limit, Resolution Scale, Renderer (+2)": the labels of the listed settings
    std::string legacy_list() const
    {
        std::string out;
        constexpr std::size_t shown = 3;
        for (std::size_t i = 0; i < m_legacy_keys.size() && i < shown; ++i)
        {
            const cfg_catalog::item* item = cfg_catalog::find(m_legacy_keys[i]);
            out += (i ? ", " : "") + (item ? std::string(i18n::tr(item->label)) : m_legacy_keys[i]);
        }
        if (m_legacy_keys.size() > shown) out += " (+" + std::to_string(m_legacy_keys.size() - shown) + ")";
        return out;
    }

    std::string use_saved_help() const
    {
        const std::string text = i18n::trf(str::SET_USAR_VALORES_SALVOS_HELP, {i18n::raw(legacy_list())});
        return m_pending == pending_action::use_saved ? text + confirm_hint() : text;
    }

    void select_use_saved(std::string& message, bool again)
    {
        if (!again)
        {
            m_pending = pending_action::use_saved;
            m_pending_row = m_selection;
            message = use_saved_help();
            return;
        }
        m_browser.pin_keys(m_legacy_keys);
        const std::size_t n = m_legacy_keys.size();
        m_event = "settings: global saved values kept as the player's own (" + std::to_string(n) + " pin(s): " + key_list(m_legacy_keys) + "; written by Salvar)";
        m_legacy_keys.clear();
        select_row(top_action::save);
        message = i18n::trf(str::SET_USAR_VALORES_SALVOS_DONE, {n});
    }

    // After a switch in this menu: read the flags and the adaptation file again and redo the preview of the staged
    // values (the browser keeps this session's choices).
    void refresh_runtime()
    {
        m_inputs = read_runtime_inputs(m_title);
        m_browser.preview(m_inputs);
    }

    // The game's pins: its file's, with this session's changes
    std::size_t game_pins() const
    {
        return m_title.empty() ? 0 : m_browser.current_pins().game.size();
    }

    // A per-game YAML of format 1 (a full dump): today's rules until it is converted
    bool legacy_game_file() const
    {
        return !m_title.empty() && m_game_file && !m_game_format2;
    }

    bool show_back_to_auto() const
    {
        return !m_title.empty() && (m_user_tuned || game_pins() != 0);
    }

    bool show_game_mode() const
    {
        return !m_title.empty() && m_auto_enabled && m_mode != policy_mode::keep_user_settings;
    }

    // Top-level rows, built from the state so rows() and select() always agree.
    std::vector<std::pair<top_action, int>> top_rows() const
    {
        std::vector<std::pair<top_action, int>> rows{{top_action::save, -1}, {top_action::auto_switch, -1}};
        if (show_use_saved()) rows.push_back({top_action::use_saved, -1});
        if (show_back_to_auto()) rows.push_back({top_action::back_to_auto, -1});
        if (show_adapt()) rows.push_back({top_action::adapt, -1});
        if (show_game_mode()) rows.push_back({top_action::game_mode, -1});
        if (show_safe_level()) rows.push_back({top_action::safe_level, -1});
        // next to the per-game safe mode: both apply from the game's next boot, in every mode
        if (show_safe_level()) rows.push_back({top_action::game_features, -1});
        for (int t = 0; t < int(cfg_catalog::tab_count); ++t) rows.push_back({top_action::tab, t});
        rows.push_back({top_action::apply_profile, -1});
        rows.push_back({top_action::reset, -1});
        if (!m_title.empty() && m_game_file) rows.push_back({top_action::remove_game, -1});
        return rows;
    }

    void select_row(top_action action, int which = -1)
    {
        const auto rows = top_rows();
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].first == action && (which < 0 || rows[i].second == which)) { m_selection = i; return; }
        m_selection = 0;
    }

    // --- texts of the top level ---

    // A top-level action runs on a second X; a warned value on a page is changed by it
    std::string confirm_hint() const
    {
        return std::string(" \xC2\xB7 ") + i18n::tr(m_tab < 0 ? str::CFGU_CONFIRM_AGAIN : str::CFGU_WARN_CONFIRM);
    }

    std::string mode_title() const
    {
        if (m_user_tuned) return i18n::tr(str::CFGU_MODE_GAME_MANUAL);
        return i18n::trf(str::CFGU_MODE_GAME_AUTO, {game_pins()});
    }

    // What X does on the mode row: convert a format-1 YAML, else switch between automatic + own settings and manual
    std::string mode_help() const
    {
        if (m_pending == pending_action::convert)
            return i18n::trf(str::CFGU_MODE_GAME_CONVERT, {m_convert_keys.size()}) + confirm_hint();
        std::string next;
        if (legacy_game_file()) next = i18n::trf(str::CFGU_MODE_GAME_CONVERT, {m_convert_keys.size()});
        else if (m_user_tuned) next = i18n::trf(str::CFGU_MODE_GAME_AUTO, {game_pins()});
        else next = i18n::tr(str::CFGU_MODE_GAME_MANUAL);
        return i18n::trf(str::CFGU_CHANGED, {"X", i18n::raw(next)});
    }

    std::string back_to_auto_help() const
    {
        if (m_pending == pending_action::back_to_auto)
            return i18n::trf(str::CFGU_BACK_AUTO_CONFIRM, {m_title, game_pins()}) + confirm_hint();
        // A marker whose saved values equal the automatic configuration is ignored by the boot.
        if (m_user_tuned && game_pins() == 0 && m_mode == policy_mode::automatic) return i18n::tr(str::SET_MARCADOR_ANTIGO_SEM_EFEITO_VALORES);
        if (m_user_tuned && game_pins() == 0) return m_title + i18n::tr(str::SET_USA_AJUSTE_MANUAL_SALVO_VOLTA);
        return i18n::trf(str::CFGU_BACK_AUTO_CONFIRM, {m_title, game_pins()});
    }

    // "Apagar: config_<TITLE_ID>.yml" (the file name stays one left-to-right run in Arabic)
    std::string remove_game_title() const
    {
        return i18n::trf(str::CFGU_CHANGED, {str::COM_APAGAR, "config_" + m_title + ".yml"});
    }

    // The file always goes whole; a format-1 dump (or a file without pins) has no pin count to name
    std::string remove_game_help() const
    {
        const std::string question = legacy_game_file() || game_pins() == 0 ? i18n::trf(str::CFGU_REMOVE_GAME_CONFIRM, {m_title})
                                                                             : i18n::trf(str::CFGU_BACK_AUTO_CONFIRM, {m_title, game_pins()});
        return m_pending == pending_action::remove_game ? question + confirm_hint() : question;
    }

    std::string hidden_switch_title() const
    {
        return i18n::trf(str::CFGU_CHANGED, {i18n::raw(i18n::tr(str::CFGU_SHOW_HIDDEN)), i18n::raw(i18n::tr(m_show_hidden ? str::COM_LIGADO : str::COM_DESLIGADO))});
    }

    // --- the open tab ---

    static bool browser_tab(int t)
    {
        return t >= 0 && t < int(tab::ps4);
    }

    // The list rows of the open browser tab: the page rows without the captions; Depuração ends with the hidden-rows switch
    std::span<const cfg_browser::row> build_list()
    {
        m_list.clear();
        if (!browser_tab(m_tab)) return {};
        const std::span<const cfg_browser::row> rows = m_browser.page(tab(m_tab), m_show_hidden);
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (!rows[i].section) m_list.push_back(i);
        return rows;
    }

    bool hidden_switch_row(std::size_t list_index) const
    {
        return m_tab == int(tab::debug) && list_index == m_list.size();
    }

    std::string toggle_hidden()
    {
        m_show_hidden = !m_show_hidden;
        build_list();
        m_selection = m_list.size();
        return hidden_switch_title();
    }

    void cancel_retry()
    {
        m_retry.reset();
        m_browser.cancel_confirmation();
    }

    // An edit of a list row; a warned row or value asks first (X on the same row confirms, Circle cancels)
    std::string edit_row(std::size_t list_index, int direction, bool coarse)
    {
        build_list();
        if (list_index >= m_list.size()) return {};
        std::string message = m_browser.step(m_list[list_index], direction, coarse);
        if (m_browser.confirmation())
        {
            m_retry = retry_edit{list_index, direction, coarse};
            message += confirm_hint();
        }
        else m_retry.reset();
        return message;
    }

    xmb::entry ps4_entry(ps4_row r) const
    {
        const auto entry = [](std::string title, std::string help) { return xmb::entry{"@setting", std::move(title), std::move(help), "tex_sysconf", {}}; };
        switch (r)
        {
        case ps4_row::players:
            return entry(players_title(), (m_players.empty() ? std::string(i18n::tr(str::SET_JOGADORES_ENTRAM_BOTAO_PS)) : m_players) + i18n::tr(str::SET_COMO_ADICIONAR));
        case ps4_row::online: return entry(online_rows::online_title(m_inputs.online), online_rows::online_help(m_inputs.online));
        case ps4_row::upnp: return entry(online_rows::upnp_title(m_inputs.upnp), online_rows::upnp_help(m_inputs.online, m_inputs.upnp));
        // A relaxed atomic load: the flag file is read once, then again only by refresh().
        case ps4_row::cool: return entry(cool_rows::title(cool_mode::enabled()), cool_rows::help());
        case ps4_row::present_hold: return entry(scene_audio_rows::present_hold_title(m_present_hold), scene_audio_rows::present_hold_help());
        case ps4_row::audio_stretch: return entry(scene_audio_rows::audio_stretch_title(m_audio_stretch), scene_audio_rows::audio_stretch_help());
        case ps4_row::game_language: return entry(game_language_title(), i18n::tr(str::GL_HELP_GLOBAL));
        // The state open() and refresh_runtime() read (no stat per frame)
        case ps4_row::safe_mode: return entry(safe_mode_rows::global_title(m_inputs.global_safe), safe_mode_rows::global_help());
        }
        return entry({}, {});
    }

    // X on a row of the PS4 tab: the switches change a flag file at once and only redo the preview, so pending edits stay
    bool select_ps4(std::string& message)
    {
        if (m_selection >= std::size(ps4_rows)) return false;
        std::string event;
        switch (ps4_rows[m_selection])
        {
        case ps4_row::players: message = i18n::tr(str::SET_APERTE_PS_CONTROLE_EXTRA_ESCOLHA); return false;
        // X moves the game language row to its next value (written at once)
        case ps4_row::game_language: message = step_game_language(1); return false;
        case ps4_row::online:
        case ps4_row::upnp:
            message = ps4_rows[m_selection] == ps4_row::online ? online_rows::toggle_online(event) : online_rows::toggle_upnp(event);
            if (event.empty()) return false;
            refresh_runtime();
            m_event = std::move(event);
            if (m_mode == policy_mode::keep_user_settings) message += i18n::tr(str::SET_SEM_EFEITO_KEEP_USER_SETTINGS);
            return false;
        // A flag file read at the start of every boot; no staged value changes.
        case ps4_row::cool:
            message = cool_rows::toggle(event);
            m_event = std::move(event);
            refresh_runtime();
            return false;
        case ps4_row::present_hold:
        case ps4_row::audio_stretch:
            message = ps4_rows[m_selection] == ps4_row::present_hold ? scene_audio_rows::toggle_present_hold(m_present_hold, event)
                                                                      : scene_audio_rows::toggle_audio_stretch(m_audio_stretch, event);
            m_event = std::move(event);
            return false;
        // safe-mode.flag: read again by every boot; the preview follows it (the game language row's PS4 value, the
        // features the policy decides with)
        case ps4_row::safe_mode:
            message = safe_mode_rows::toggle_global(event);
            if (event.empty()) return false;
            m_event = std::move(event);
            refresh_runtime();
            return false;
        }
        return false;
    }

    // --- files ---

    // Atomic write of a settings file (false: the old file stays)
    static bool write_settings(const std::string& path, std::string_view text)
    {
        fs::create_path(fs::get_parent_dir(path));
        fs::pending_file output(path);
        return output.file && output.file.write(text.data(), text.size()) == text.size() && output.commit();
    }

    // The keys of a list for startup.log (the first 12, then the count)
    static std::string key_list(const std::vector<std::string>& keys)
    {
        std::string out;
        for (std::size_t i = 0; i < keys.size() && i < 12; ++i) out += (i ? ", " : "") + keys[i];
        if (keys.size() > 12) out += " (+" + std::to_string(keys.size() - 12) + ")";
        return out;
    }

    // The conversion of the game's format-1 YAML: the settings that become pins and the format-2 text (empty when the
    // file cannot be read). The stack is loaded again into two scratch trees, so the staged copy is not touched.
    std::pair<std::vector<std::string>, std::string> conversion() const
    {
        const std::string game_yaml = policy_detail::read_small(m_path, settings_text_limit);
        if (game_yaml.empty()) return {};
        const auto saved = std::make_unique<cfg_root>();
        const auto automatic = std::make_unique<cfg_root>();
        saved->from_string(m_global_baseline);
        automatic->from_string(m_global_baseline);
        saved->from_string(game_yaml);
        fix_backends(*saved);
        std::vector<std::string> keys = conversion_pins(*saved, *automatic, m_title, m_inputs, game_yaml, read_pins({}).global);
        // the settings the PS4 keeps at one value, or that have no meaning here, are never the player's
        std::erase_if(keys, [](const std::string& key)
        {
            const cfg_catalog::item* item = cfg_catalog::find(key);
            return item && (item->kind == cfg_catalog::klass::unavailable || item->kind == cfg_catalog::klass::hidden);
        });
        std::string text = with_settings_stamp(cfg_browser::sparse_yaml_for(*saved, keys), keys);
        return {std::move(keys), std::move(text)};
    }

    // The per-game DFMA opt-in is its own marker, because the boot turns the option off without it whatever the YAML
    // says. With accurate-dfma.flag every game keeps a saved 'true' already, so there a save only removes the marker.
    // The staged value is the player's choice when one was made here (the browser then skips the hardware limit for
    // it), else the value the existing marker gives. Returns the note for the save message.
    std::string save_dfma_marker()
    {
        if (utils::has_fma3()) return {};
        const bool dfma = m_config.core.use_accurate_dfma.get();
        if ((!dfma || !fs::is_file(accurate_dfma_flag)) && !set_title_accurate_dfma(m_title, dfma)) return i18n::tr(str::SET_ERRO_GRAVAR_MARCADOR_DFMA);
        if (dfma && title_accurate_dfma(m_title)) return i18n::tr(str::SET_DFMA_PRECISO_SO_NESTE);
        return {};
    }

    // Salvar. Global: the full dump with the pins. Per game: only the pinned settings (no file when there are none);
    // a format-1 YAML keeps its full dump and the user-tuned marker rule until it is converted.
    bool save(std::string& message)
    {
        const std::vector<std::string> pins = m_browser.pins_to_save();
        const std::string dump = m_browser.save_dump();
        if (m_title.empty())
        {
            if (!write_settings(global_path, with_settings_stamp(dump, pins)))
            { message = i18n::tr(str::SET_ERRO_SALVAR_ALTERACOES_CONTINUAM_PENDENTES); return false; }
            g_cfg.from_string(dump);
            m_event = "settings: global settings saved (settings format 2, " + std::to_string(pins.size()) + " pin(s)" +
                (pins.empty() ? std::string{} : ": " + key_list(pins)) + ")";
            message = i18n::tr(str::SET_CONFIGURACOES_SALVAS) + scope_text() + i18n::tr(str::COM_APLICADAS_PROXIMO_JOGO);
            return true;
        }
        // The global baseline, so the game's file never stands alone
        if (!fs::is_file(global_path) && !write_settings(global_path, m_global_baseline))
        { message = i18n::tr(str::SET_ERRO_SALVAR_CONFIGURACAO_GLOBAL_BASE); return false; }
        std::string note;
        if (legacy_game_file())
        {
            // Format 1: the full dump; the user-tuned marker only when the saved values differ from the automatic result
            if (!write_settings(m_path, with_profile_stamp(dump)))
            { message = i18n::tr(str::SET_ERRO_SALVAR_ALTERACOES_CONTINUAM_PENDENTES); return false; }
            const auto check = std::make_unique<cfg_root>();
            check->from_string(m_global_baseline);
            check->from_string(dump);
            const pinned_settings global_pins = resolve_pins(*check, read_pins({}));
            const bool manual = !saved_equals_automatic(*check, m_title, policy_detail::pins_or_null(global_pins));
            if (!set_user_tuned(m_title, manual)) note = i18n::tr(str::SET_ERRO_GRAVAR_MARCADOR_AJUSTE_MANUAL);
            else note = i18n::tr(manual ? str::SET_AJUSTE_MANUAL_VOLTAR_AUTOMATICO_DESFAZ : str::SET_CONFIGURACAO_AUTOMATICA_MANTIDA);
            m_event = "settings: " + m_title + " saved (settings format 1, full dump" + (manual ? ", user-tuned)" : ")");
        }
        else
        {
            // Format 2: only the player's own choices; a game without any has no file
            if (pins.empty())
            {
                if (fs::is_file(m_path) && !fs::remove_file(m_path))
                { message = i18n::tr(str::COM_ERRO_APAGAR) + m_path; return false; }
            }
            else if (!write_settings(m_path, with_settings_stamp(m_browser.sparse_yaml(), pins)))
            { message = i18n::tr(str::SET_ERRO_SALVAR_ALTERACOES_CONTINUAM_PENDENTES); return false; }
            note = m_user_tuned ? std::string(i18n::tr(str::SET_AJUSTE_MANUAL_VOLTAR_AUTOMATICO_DESFAZ))
                                : " \xC2\xB7 " + i18n::trf(str::CFGU_MODE_GAME_AUTO, {pins.size()});
            m_event = "settings: " + m_title + " saved (settings format 2, " + std::to_string(pins.size()) + " pin(s)" +
                (pins.empty() ? std::string{} : ": " + key_list(pins)) + ")";
        }
        note += save_dfma_marker();
        m_mode = get_policy_mode(m_title);
        m_user_tuned = is_user_tuned(m_title);
        // The database entry runs only in automatic mode (with the switch off, only for a game without its own YAML).
        if (m_database != database_use::none && m_database != database_use::rejected)
            m_database = m_mode == policy_mode::automatic ? database_use::applied
                : m_mode == policy_mode::user_tuned ? database_use::manual : database_use::custom_config;
        message = i18n::tr(str::SET_CONFIGURACOES_SALVAS) + scope_text() + note;
        return true;
    }

    std::string auto_switch_help() const
    {
        // m_mode (set by open()) instead of a stat of /data per frame.
        if (m_mode == policy_mode::keep_user_settings) return i18n::tr(str::SET_KEEP_USER_SETTINGS_FLAG_PRESENTE);
        return i18n::tr(m_auto_enabled ? str::SET_CADA_JOGO_INICIA_AJUSTES_RECOMENDADOS : str::SET_JOGOS_USAM_VALORES_SALVOS_LIGA);
    }

    // Reopens the scope after a file changed (mode, conversion, removal), with the cursor back on 'action'
    void reopen(top_action action)
    {
        const std::string title = m_title;
        open(title);
        select_row(action);
    }

    // The top-level actions that change a file at once ask for an empty session first, since reopening drops edits
    bool refuse_pending(std::string& message) const
    {
        if (!m_browser.pending()) return false;
        message = i18n::tr(str::SET_HA_ALTERACOES_PENDENTES_SALVAR_CIRCULO);
        return true;
    }

    // 'Modo deste jogo': a format-1 YAML is converted (second X), else the user-tuned marker is created or removed
    void select_game_mode(std::string& message, bool again)
    {
        if (refuse_pending(message)) return;
        if (legacy_game_file())
        {
            if (!again)
            {
                m_pending = pending_action::convert;
                m_pending_row = m_selection;
                message = i18n::trf(str::CFGU_MODE_GAME_CONVERT, {m_convert_keys.size()}) + confirm_hint();
                return;
            }
            const auto [keys, text] = conversion();
            if (text.empty()) { message = i18n::tr(str::COM_ERRO_GRAVAR) + m_path; return; }
            const bool written = keys.empty() ? fs::remove_file(m_path) : write_settings(m_path, text);
            if (!written) { message = i18n::tr(str::COM_ERRO_GRAVAR) + m_path; return; }
            if (!set_user_tuned(m_title, false)) { message = i18n::tr(str::SET_ERRO_APAGAR_MARCADOR_AJUSTE_MANUAL) + m_title; return; }
            const std::string title = m_title;
            reopen(top_action::game_mode);
            m_event = "settings: " + title + " converted to settings format 2 (" + std::to_string(keys.size()) + " pin(s)" +
                (keys.empty() ? std::string{} : ": " + key_list(keys)) + "); user-tuned marker removed";
            message = i18n::trf(str::CFGU_CHANGED, {title, i18n::raw(i18n::trf(str::CFGU_MODE_GAME_AUTO, {keys.size()}))});
            return;
        }
        const bool manual = !m_user_tuned;
        if (!set_user_tuned(m_title, manual))
        {
            message = i18n::tr(manual ? str::SET_ERRO_GRAVAR_MARCADOR_AJUSTE_MANUAL : str::SET_ERRO_APAGAR_MARCADOR_AJUSTE_MANUAL) + m_title;
            return;
        }
        const std::string title = m_title;
        reopen(top_action::game_mode);
        m_event = "settings: " + title + (manual ? " mode: manual, everything as saved (user-tuned marker created)"
                                                 : " mode: automatic + own settings (user-tuned marker removed)");
        message = i18n::trf(str::CFGU_CHANGED, {title, i18n::raw(mode_title())});
    }

    // 'Voltar para automático' (per game): the user-tuned marker and every pin of the game go (a format-2 file holds only
    // pins, so it goes too); a format-1 file keeps its other values. Asks first when there are pins.
    void select_back_to_auto(std::string& message, bool again)
    {
        if (refuse_pending(message)) return;
        const std::size_t pins = game_pins();
        if (pins && !again)
        {
            m_pending = pending_action::back_to_auto;
            m_pending_row = m_selection;
            message = back_to_auto_help();
            return;
        }
        if (pins && m_game_format2 && m_game_file && !fs::remove_file(m_path)) { message = i18n::tr(str::COM_ERRO_APAGAR) + m_path; return; }
        if (!set_user_tuned(m_title, false)) { message = i18n::tr(str::SET_ERRO_APAGAR_MARCADOR_AJUSTE_MANUAL) + m_title; return; }
        const std::string title = m_title;
        reopen(top_action::save);
        m_event = "settings: " + title + " returned to the automatic configuration (user-tuned marker and " + std::to_string(pins) + " pin(s) removed)";
        message = title + i18n::tr(str::SET_VOLTOU_CONFIGURACAO_AUTOMATICA_VALORES_MOSTRADOS);
    }

    // PC's 'Remove Custom Configuration': the game's YAML, its pins and the user-tuned marker go (second X); the game
    // language and accurate-dfma markers stay.
    void select_remove_game(std::string& message, bool again)
    {
        if (refuse_pending(message)) return;
        if (!again)
        {
            m_pending = pending_action::remove_game;
            m_pending_row = m_selection;
            message = remove_game_help();
            return;
        }
        if (fs::is_file(m_path) && !fs::remove_file(m_path)) { message = i18n::tr(str::COM_ERRO_APAGAR) + m_path; return; }
        if (!set_user_tuned(m_title, false)) { message = i18n::tr(str::SET_ERRO_APAGAR_MARCADOR_AJUSTE_MANUAL) + m_title; return; }
        const std::string title = m_title;
        reopen(top_action::save);
        m_event = "settings: " + title + " custom configuration removed (config_" + title + ".yml and the user-tuned marker)";
        message = title + i18n::tr(str::SET_VOLTOU_CONFIGURACAO_AUTOMATICA_VALORES_MOSTRADOS);
    }

    // 'Aplicar perfil automático': every setting the automatic profile would still change (the pinned ones) goes back
    // to automatic, without touching the others.
    std::string apply_profile()
    {
        std::vector<std::string> keys;
        policy_detail::dry_run([&](auto&& record)
        {
            apply_performance_policy(m_config, m_title, [&](const cfg::_base& setting, const std::string& old, std::string_view why, change_kind kind)
            {
                if (kind == change_kind::profile) keys.push_back(setting_key(setting));
                record(setting, old, why, kind);
            });
        });
        // Counted by the values that changed (a global pin stays in a game's scope)
        unsigned changed = 0;
        for (const std::string& key : keys)
        {
            const cfg::_base* leaf = cfg_browser::find(m_config, key);
            const std::string before = leaf ? setting_text(*leaf) : std::string{};
            m_browser.reset_auto_key(key);
            changed += leaf && setting_text(*leaf) != before;
        }
        m_selection = 0;
        return changed ? i18n::trf(str::SET_PERFIL_AUTOMATICO_VALOR_ES_PREPARADOS, {changed}) : std::string(i18n::tr(str::SET_OPCOES_JA_ESTAO_PERFIL));
    }

    // 'Restaurar valores padrão': the PS4 automatic value of every setting of the scope (upstream default or, per game,
    // the global value, then the profile, this game's rows and the limits), as Square does for one row; every pin goes.
    std::string restore_defaults()
    {
        m_browser.clear_scope_choices();
        if (m_title.empty())
            for (const cfg_catalog::item& item : cfg_catalog::items())
                if ((item.kind == cfg_catalog::klass::expose || item.kind == cfg_catalog::klass::warn) && item.edit != cfg_catalog::editor::readonly &&
                    !(item.flags & cfg_catalog::flag::no_default))
                    m_browser.reset_auto_key(item.key);
        return i18n::tr(str::SET_VALORES_AUTOMATICOS_PS4_PREPARADOS_SELECIONE);
    }

public:
    static constexpr const char* global_path = global_settings_path;
    static bool valid_title_id(std::string_view id)
    {
        return id.size() == 9 && std::all_of(id.begin(), id.end(), [](char c)
        { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
    }

    void open(std::string_view serial = {})
    {
        m_config.from_string(g_cfg.to_string());
        // After a game, g_cfg still holds that game's YAML and boot policy values (an adaptation such as one compile
        // thread, the video memory cap) until the next boot reloads config.yml. Start from config.yml the way
        // Emulator::Init does, so neither the menu nor a save inherits them.
        if (const fs::file base{fs::get_config_dir(true) + "config.yml"}) m_config.from_string(base.to_string());
        if (const fs::file global{global_path}) m_config.from_string(global.to_string());
        fix_backends(m_config);
        m_global_baseline = m_config.to_string();
        m_title = valid_title_id(serial) ? std::string(serial) : std::string{};
        m_path = !m_title.empty() ? rpcs3::utils::get_custom_config_path(m_title) : global_path;
        m_game_file = m_game_format2 = false;
        if (!m_title.empty())
            if (const fs::file custom{m_path})
            {
                const std::string text = custom.to_string();
                m_config.from_string(text);
                m_game_file = true;
                m_game_format2 = saved_settings_format(text) >= settings_format_version;
            }
        fix_backends(m_config);
        if (!s_self_checked)
        {
            s_self_checked = true;
            self_check();
        }
        // The values shown are the values that will run: the browser previews the same boot policy System.cpp's
        // fixup_settings applies (side-effect free: opening the menu is not a boot), with the pins of both files.
        m_auto_enabled = auto_config_enabled();
        m_user_tuned = is_user_tuned(m_title);
        m_inputs = read_runtime_inputs(m_title);
        m_browser.bind(m_config, m_title, m_title.empty() ? std::string_view{} : std::string_view{m_global_baseline}, read_pins(m_title),
            get_policy_mode(m_title));
        const policy_result preview = m_browser.preview(m_inputs);
        m_mode = preview.mode;
        m_database = preview.database.use;
        m_present_hold = scene_audio_rows::present_hold_enabled();
        m_audio_stretch = scene_audio_rows::audio_stretch_enabled();
        m_game_language_global = read_game_language({}, true).value;
        if (!m_title.empty())
        {
            const game_language_choice own = read_game_language(m_title);
            m_game_language_game = own.source == game_language_source::game ? std::optional<std::int32_t>(own.value) : std::nullopt;
        }
        else m_game_language_game.reset();
        m_convert_keys.clear();
        if (legacy_game_file() && show_game_mode()) m_convert_keys = conversion().first;
        // 0.40: the per-game .features file, and the global layer's own values that the automatic configuration replaces
        m_title_features = m_title.empty() ? 0 : safe_mode::title_features_file::named(m_title);
        m_legacy_keys.clear();
        if (m_title.empty() && m_mode == policy_mode::automatic) m_legacy_keys = legacy_values();
        m_tab = -1;
        m_selection = 0;
        m_pending = pending_action::none;
        m_retry.reset();
        m_show_hidden = false;
        m_list.clear();
    }

    // Short state for the launcher's messages (e.g. "Configuração automática").
    std::string status() const
    {
        std::string text = policy_mode_text(m_mode);
        if (m_database == database_use::applied) text += i18n::tr(str::SET_CONFIG_PC);
        if (show_adapt()) text += i18n::tr(str::SET_AJUSTE_APOS_FALHA);
        return text;
    }

    // 0.31: connected players for the 'Controles conectados' row (ps4_pads::summary() text and the launcher's count).
    // 0.39: without a count (< 0) the row shows no number: the summary is translated text and is not parsed.
    void set_players(std::string summary, int connected = -1)
    {
        m_players_connected = connected >= 0 ? connected : -1;
        m_players = std::move(summary);
    }
    // A line for startup.log after an action that changed files without closing the menu (empty if none).
    std::string take_event() { return std::exchange(m_event, std::string{}); }

    std::size_t& selection() { return m_selection; }

    // The rows of the XMB list: the top level, the PS4 tab's switches, or a browser page without its section captions
    // (the value, who decides it and the help line in the subtitle). Built in the interface language.
    std::vector<xmb::entry> rows()
    {
        std::vector<xmb::entry> rows;
        auto row = [&](std::string title, std::string help, const char* path = "@setting")
        { rows.push_back({path, std::move(title), std::move(help), "tex_sysconf", {}}); };
        if (m_tab == int(tab::ps4))
        {
            for (const ps4_row r : ps4_rows) rows.push_back(ps4_entry(r));
            return rows;
        }
        if (m_tab >= 0)
        {
            const std::span<const cfg_browser::row> page = build_list();
            for (const std::size_t i : m_list)
            {
                const cfg_browser::row& r = page[i];
                std::string help = r.value;
                if (!r.badge_text.empty()) help += " \xC2\xB7 " + r.badge_text;
                if (!r.help.empty()) help += " \xC2\xB7 " + r.help;
                row(r.label, std::move(help), r.locked ? "@setting:locked" : "@setting");
            }
            if (m_tab == int(tab::debug)) row(hidden_switch_title(), i18n::tr(str::CFGU_READONLY));
            return rows;
        }
        for (const auto& [action, which] : top_rows()) switch (action)
        {
        case top_action::save:
            // '+ config do PC' when the game's PC config_database.dat entry runs with these values.
            row(i18n::tr(str::SET_SALVAR_CONFIGURACOES), scope_text() + " \xC2\xB7 " + policy_mode_text(m_mode) +
                (m_database == database_use::applied ? i18n::tr(str::SET_CONFIG_PC_2) : "") +
                i18n::tr(m_browser.pending() ? str::SET_ALTERACOES_PENDENTES : str::COM_APLICADAS_PROXIMO_JOGO));
            break;
        case top_action::auto_switch:
            row(i18n::tr(m_auto_enabled ? str::SET_CONFIGURACAO_AUTOMATICA_LIGADA : str::SET_CONFIGURACAO_AUTOMATICA_DESLIGADA), auto_switch_help());
            break;
        case top_action::back_to_auto: row(i18n::tr(str::COM_VOLTAR_AUTOMATICO), back_to_auto_help()); break;
        case top_action::adapt: row(i18n::tr(str::SET_AJUSTE_AUTOMATICO_APOS_FALHA), adapt_text() + i18n::tr(str::SET_REMOVE)); break;
        case top_action::game_mode: row(mode_title(), mode_help()); break;
        case top_action::safe_level: row(safe_mode_rows::game_title(m_inputs.safe_level), safe_mode_rows::game_help(m_inputs)); break;
        case top_action::game_features: row(game_features_title(), i18n::tr(str::SET_NOVIDADES_HELP)); break;
        case top_action::use_saved: row(i18n::trf(str::SET_USAR_VALORES_SALVOS, {m_legacy_keys.size()}), use_saved_help()); break;
        case top_action::tab:
            row(i18n::tr(cfg_catalog::tab_name(tab(which))), which == int(tab::ps4) ? std::string(i18n::tr(str::SET_ABRIR)) : m_browser.tab_summary(tab(which)));
            break;
        case top_action::apply_profile:
            row(i18n::tr(str::SET_APLICAR_PERFIL_AUTOMATICO), i18n::tr(str::SET_LLVM_VULKAN_720P_SHADERS_ASSINCRONOS));
            break;
        case top_action::reset:
            row(i18n::tr(str::SET_RESTAURAR_VALORES_PADRAO), i18n::tr(str::SET_VALORES_AUTOMATICOS_PS4_OPCOES_DESTE));
            break;
        case top_action::remove_game: row(remove_game_title(), remove_game_help()); break;
        }
        return rows;
    }

    // --- for the launcher's page view ---

    cfg_browser::model& browser() { return m_browser; }
    // A browser tab (CPU .. Depuração) is open; the PS4 tab and the top level are XMB lists
    bool page_open() const { return browser_tab(m_tab); }
    std::optional<tab> current_tab() const { return m_tab < 0 ? std::nullopt : std::optional<tab>(tab(m_tab)); }
    void open_tab(tab t)
    {
        cancel_retry();
        m_pending = pending_action::none;
        m_tab = int(t);
        m_selection = 0;
        m_list.clear();
    }
    // The PS4 tab with the cursor on the switch that decides a setting (online, UPnP, game language, Modo frio)
    void open_switch(cfg_browser::ps4_switch which)
    {
        open_tab(tab::ps4);
        const ps4_row target = which == cfg_browser::ps4_switch::upnp ? ps4_row::upnp
            : which == cfg_browser::ps4_switch::game_language ? ps4_row::game_language
            : which == cfg_browser::ps4_switch::cool ? ps4_row::cool : ps4_row::online;
        for (std::size_t i = 0; i < std::size(ps4_rows); ++i)
            if (ps4_rows[i] == target) m_selection = i;
    }
    // The browser page row under the list cursor (SIZE_MAX outside a page or on the hidden-rows switch)
    std::size_t page_row()
    {
        build_list();
        return m_selection < m_list.size() ? m_list[m_selection] : SIZE_MAX;
    }
    bool show_hidden() const { return m_show_hidden; }
    void set_show_hidden(bool on)
    {
        m_show_hidden = on;
        m_list.clear();
    }
    bool game_scope() const { return !m_title.empty(); }
    const std::string& title() const { return m_title; }
    // Settings chosen, reset or (un)pinned in this session and not saved yet
    std::size_t pending() const { return m_browser.pending(); }

    // The menu's scope: the game's title ID, or all games
    std::string scope_text() const
    {
        return m_title.empty() ? std::string(i18n::tr(str::SET_TODOS_JOGOS)) : m_title;
    }

    // Rows the cursor may reach: every row (an unavailable one shows why it is locked)
    std::size_t selectable_rows(std::size_t rows) const
    {
        return rows;
    }

    // Left/Right: a browser row steps its value (R1 = coarse); the PS4 tab's game language row moves (written at once
    // and logged here: the launcher takes events only after X)
    std::string adjust(int direction, bool coarse = false)
    {
        m_pending = pending_action::none;
        if (m_tab == int(tab::ps4))
        {
            if (m_selection < std::size(ps4_rows) && ps4_rows[m_selection] == ps4_row::game_language)
            {
                std::string message = step_game_language(direction < 0 ? -1 : 1);
                if (const std::string event = take_event(); !event.empty()) ps4_startup_checkpoint(event.c_str());
                return message;
            }
            return i18n::tr(str::SET_ABRE_CIRCULO_DESCARTA_ALTERACOES_VOLTA);
        }
        if (m_tab < 0) return i18n::tr(str::SET_ABRE_CIRCULO_DESCARTA_ALTERACOES_VOLTA);
        build_list();
        if (hidden_switch_row(m_selection)) return toggle_hidden();
        cancel_retry();
        return edit_row(m_selection, direction, coarse);
    }

    // Circle: a pending confirmation is cancelled first, then a tab goes back to the top level (false at the top level:
    // the launcher closes the menu)
    bool back()
    {
        m_pending = pending_action::none;
        if (m_retry || m_browser.confirmation())
        {
            cancel_retry();
            if (m_tab >= 0) return true;
        }
        if (m_tab < 0) return false;
        const int t = m_tab;
        m_tab = -1;
        m_list.clear();
        select_row(top_action::tab, t);
        return true;
    }

    // X. A browser row steps its value (a warned one asks first; a row that follows a PS4 switch opens that switch), the
    // PS4 tab's rows act at once, the top level runs its actions. True only after Salvar committed its file.
    bool select(std::string& message)
    {
        if (m_tab == int(tab::ps4))
        {
            m_pending = pending_action::none;
            return select_ps4(message);
        }
        if (m_tab >= 0)
        {
            m_pending = pending_action::none;
            const std::span<const cfg_browser::row> page = build_list();
            if (hidden_switch_row(m_selection)) { message = toggle_hidden(); return false; }
            if (m_selection >= m_list.size()) return false;
            if (m_retry && m_retry->row == m_selection && m_browser.confirmation())
            {
                const retry_edit retry = *m_retry;
                m_browser.confirm();
                message = edit_row(retry.row, retry.direction, retry.coarse);
                return false;
            }
            cancel_retry();
            if (const cfg_browser::ps4_switch which = page[m_list[m_selection]].redirect; which != cfg_browser::ps4_switch::none)
            {
                message = m_browser.step(m_list[m_selection], 1);
                open_switch(which);
                return false;
            }
            message = edit_row(m_selection, 1, false);
            return false;
        }
        const auto top = top_rows();
        if (m_selection >= top.size()) return false;
        const auto [action, which] = top[m_selection];
        // A second X on the same row runs the action that asked; anything else drops the question
        const bool again = m_pending != pending_action::none && m_pending_row == m_selection;
        const pending_action asked = m_pending;
        m_pending = pending_action::none;
        switch (action)
        {
        case top_action::tab:
            open_tab(tab(which));
            message = which == int(tab::ps4) ? std::string{} : std::string(i18n::tr(str::SET_ESQ_DIR_ALTERAM_R1_ACELERA));
            return false;
        case top_action::save:
            return save(message);
        case top_action::adapt:
        {
            std::string keys;
            for (const auto& key : m_inputs.adapt) keys += (keys.empty() ? "" : ",") + key;
            if (!adapt::remove(m_title)) { message = i18n::tr(str::COM_ERRO_APAGAR) + adapt::path(m_title); return false; }
            refresh_runtime();
            select_row(top_action::save);
            m_event = "settings: " + m_title + " adaptation removed from the menu (" + keys + ")";
            message = m_title + i18n::tr(str::SET_AJUSTE_AUTOMATICO_REMOVIDO_PROXIMO_BOOT);
            return false;
        }
        case top_action::auto_switch:
        {
            // The global switch changes a file at once and reloads the menu, which would drop staged edits.
            if (refuse_pending(message)) return false;
            const bool enable = !m_auto_enabled;
            if (!set_auto_config_enabled(enable)) { message = i18n::tr(str::SET_ERRO_GRAVAR_DATA_RPCS3_AUTO); return false; }
            reopen(top_action::auto_switch);
            m_event = enable ? "settings: automatic configuration switched ON (auto-config-off.flag removed)"
                             : "settings: automatic configuration switched OFF (auto-config-off.flag created)";
            message = i18n::tr(enable ? str::SET_CONFIGURACAO_AUTOMATICA_LIGADA_JOGOS_INICIAM : str::SET_CONFIGURACAO_AUTOMATICA_DESLIGADA_JOGOS_USAM);
            return false;
        }
        case top_action::back_to_auto: select_back_to_auto(message, again && asked == pending_action::back_to_auto); return false;
        case top_action::game_mode: select_game_mode(message, again && asked == pending_action::convert); return false;
        case top_action::safe_level:
            message = step_safe_level();
            select_row(top_action::safe_level);
            return false;
        case top_action::game_features:
            message = step_game_features();
            select_row(top_action::game_features);
            return false;
        case top_action::use_saved: select_use_saved(message, again && asked == pending_action::use_saved); return false;
        case top_action::remove_game: select_remove_game(message, again && asked == pending_action::remove_game); return false;
        case top_action::apply_profile: message = apply_profile(); return false;
        case top_action::reset: message = restore_defaults(); return false;
        }
        return false;
    }

    // Square on a browser row: back to the automatic value (its pin goes). False where there is nothing to reset.
    bool reset_option()
    {
        m_pending = pending_action::none;
        if (!browser_tab(m_tab)) return false;
        const std::span<const cfg_browser::row> page = build_list();
        if (m_selection >= m_list.size()) return false;
        // 0.40: an unavailable row whose files hold a value it cannot use: Square removes the value (Salvar writes it)
        if (m_browser.has_unused_saved(m_list[m_selection]))
        {
            cancel_retry();
            m_browser.remove_unused_saved(m_list[m_selection]);
            return true;
        }
        if (page[m_list[m_selection]].locked) return false;
        cancel_retry();
        m_browser.reset_auto(m_list[m_selection]);
        return true;
    }
};
}
