#include "stdafx.h"
#include "pad_config.h"
#include "Emu/system_utils.hpp"
#include "Emu/Io/PadHandler.h"

extern std::string g_input_config_override;

std::vector<pad::combo> cfg_pad::get_combos(std::string_view button_string)
{
	if (button_string.empty())
		return {};

	// Handle special case: string contains separator itself as configured value (it's why I don't use fmt::split here)
	const auto split = [](std::string_view str, char sep)
	{
		std::set<std::string> buttons;
		bool was_sep = true;
		usz btn_start = 0ULL;
		usz i = 0ULL;

		for (; i < str.size(); i++)
		{
			const char c = str[i];

			if (c == sep)
			{
				if (!was_sep)
				{
					was_sep = true;
					buttons.insert(std::string(str.substr(btn_start, i - btn_start)));
					continue;
				}
			}

			if (was_sep)
			{
				was_sep = false;
				btn_start = i;
			}
		
			if (i == (str.size() - 1))
			{
				buttons.insert(std::string(str.substr(btn_start, i - btn_start + 1)));
			}
		}

		return buttons;
	};

	std::vector<pad::combo> combos;

	// Get all combos (seperated by ',')
	const std::set<std::string> combo_strings = split(button_string, ',');

	for (const std::string& combo_string : combo_strings)
	{
		// Get all keys for this combo (seperated by '&')
		std::set<std::string> combo = split(combo_string, '&');
		if (!combo.empty())
		{
			combos.push_back(pad::combo{std::move(combo)});
		}
	}

	return combos;
}

std::string cfg_pad::get_button_string(std::vector<pad::combo>& combos)
{
	std::vector<std::string> combo_strings;

	// Remove duplicates
	std::sort(combos.begin(), combos.end());
	combos.erase(std::unique(combos.begin(), combos.end()), combos.end());

	for (const pad::combo& combo : combos)
	{
		// Merge all keys for this combo (seperated by '&')
		combo_strings.push_back(combo.to_string());
	}

	// Merge combos (seperated by ',')
	return fmt::merge(combo_strings, ",");
}

std::string cfg_pad::make_button_string(const std::unordered_map<u32, std::string>& button_list, const std::vector<std::set<u32>>& button_combos)
{
	std::vector<pad::combo> combos;

	for (const std::set<u32>& button_combo : button_combos)
	{
		if (button_combo.empty()) continue;

		pad::combo combo {};

		for (u32 button : button_combo)
		{
			combo.add_button(::at32(button_list, button));
		}

		combos.push_back(std::move(combo));
	}

	return get_button_string(combos);
}

u8 cfg_pad::get_motor_speed(VibrateMotor& motor, f32 multiplier) const
{
	// If motor is small, use either 0 or 255.
	const u8 value = motor.is_large_motor ? motor.value : (motor.value > 0 ? 255 : 0);

	// Ignore lower range. Scale remaining range to full range.
	const f32 adjusted = PadHandlerBase::ScaledInput(value, static_cast<f32>(vibration_threshold.get()), 255.0f, 0.0f, 255.0f);

	// Apply multiplier
	motor.adjusted_value = static_cast<u8>(std::clamp(adjusted * multiplier, 0.0f, 255.0f));
	return motor.adjusted_value;
}

u8 cfg_pad::get_large_motor_speed(std::array<VibrateMotor, 2>& motors) const
{
	return get_motor_speed(motors[switch_vibration_motors ? 1 : 0], multiplier_vibration_motor_large / 100.0f);
}

u8 cfg_pad::get_small_motor_speed(std::array<VibrateMotor, 2>& motors) const
{
	return get_motor_speed(motors[switch_vibration_motors ? 0 : 1], multiplier_vibration_motor_small / 100.0f);
}

#ifdef RPCS3_PS4
// Players 1-4 are the console's DualShock 4 seats (rpcs3/ps4_pads.h) and players 5-7 stay empty. A desktop input
// profile names handlers and devices that cannot be opened on the console and would leave the game without any
// controller, so the handler and device of each player are forced. Button mappings and options are kept when the
// profile already used the PS4 handler; a mapping written for another handler (key names, or the empty mapping
// of a Null player) means nothing to it, so that player goes back to the defaults of its new handler.
static void ps4_force_native_players(cfg_input& input, bool from_profile)
{
	for (u32 i = 0; i < input.player.size(); ++i)
	{
		cfg_player& player = *input.player[i];
		const bool seat = i < 4;
		const pad_handler handler = seat ? pad_handler::ps4 : pad_handler::null;
		const std::string device = seat ? fmt::format("PS4 Controller %u", i + 1) : fmt::format("%s", pad_handler::null);
		const bool foreign_mapping = from_profile && player.handler.get() != handler;

		if (from_profile && (foreign_mapping || player.device.to_string() != device))
		{
			input_log.notice("PS4: player %u uses %s / '%s' instead of %s / '%s'%s", i + 1, handler, device, player.handler.get(), player.device.to_string(),
				foreign_mapping ? " (button mapping reset to its defaults)" : "");
		}

		if (foreign_mapping)
		{
			// The defaults are the ones pad_thread::InitPadConfig set for this player's handler.
			player.config.from_default();
		}

		player.handler.set(handler);
		player.device.from_string(device);
		player.buddy_device.from_string(""sv);
	}
}
#endif

bool cfg_input::load(const std::string& title_id, const std::string& config_file, bool strict)
{
	input_log.notice("Loading pad config (title_id='%s', config_file='%s', strict=%d)", title_id, config_file, strict);

	std::string cfg_name;

	// Check configuration override first
	if (!strict && !g_input_config_override.empty())
	{
		cfg_name = rpcs3::utils::get_input_config_dir() + g_input_config_override + ".yml";
	}

	// Check custom config next
	if (!title_id.empty() && !fs::is_file(cfg_name))
	{
		cfg_name = rpcs3::utils::get_custom_input_config_path(title_id);
	}

	// Check active global configuration next
	if ((title_id.empty() || !strict) && !config_file.empty() && !fs::is_file(cfg_name))
	{
		cfg_name = rpcs3::utils::get_input_config_dir() + config_file + ".yml";
	}

	// Fallback to default configuration
	if (!strict && !fs::is_file(cfg_name))
	{
		cfg_name = rpcs3::utils::get_input_config_dir() + g_cfg_input_configs.default_config + ".yml";
	}

	from_default();

	if (fs::file cfg_file{ cfg_name, fs::read })
	{
		input_log.notice("Loading input configuration: '%s'", cfg_name);

		if (const std::string content = cfg_file.to_string(); !content.empty())
		{
#ifdef RPCS3_PS4
			const bool loaded = from_string(content);
			ps4_force_native_players(*this, true);
			return loaded;
#else
			return from_string(content);
#endif
		}
	}

	// Choose the native input source when no saved profile exists.
#ifdef RPCS3_PS4
	input_log.notice("Input configuration empty. Adding native PS4 pad handlers for players 1-4");
	ps4_force_native_players(*this, false);
#else
	input_log.notice("Input configuration empty. Adding default keyboard pad handler");
	player[0]->handler.from_string(fmt::format("%s", pad_handler::keyboard));
	player[0]->device.from_string(pad::keyboard_device_name.data());
#endif
	player[0]->buddy_device.from_string(""sv);

	return false;
}

void cfg_input::save(const std::string& title_id, const std::string& config_file) const
{
	std::string cfg_name;

	if (title_id.empty())
	{
		cfg_name = rpcs3::utils::get_input_config_dir() + config_file + ".yml";
		input_log.notice("Saving input configuration '%s' to '%s'", config_file, cfg_name);
	}
	else
	{
		cfg_name = rpcs3::utils::get_custom_input_config_path(title_id);
		input_log.notice("Saving custom pad config for '%s' to '%s'", title_id, cfg_name);
	}

	if (!fs::create_path(fs::get_parent_dir(cfg_name)))
	{
		input_log.fatal("Failed to create path: %s (%s)", cfg_name, fs::g_tls_error);
	}

	if (!cfg::node::save(cfg_name))
	{
		input_log.error("Failed to save pad config to '%s' (error=%s)", cfg_name, fs::g_tls_error);
	}
}

cfg_input_configurations::cfg_input_configurations()
	: path(rpcs3::utils::get_input_config_root() + "/active_input_configurations.yml")
{
}

bool cfg_input_configurations::load()
{
	if (fs::file cfg_file{ path, fs::read })
	{
		return from_string(cfg_file.to_string());
	}

	from_default();
	return false;
}

void cfg_input_configurations::save() const
{
	input_log.notice("Saving input configurations config to '%s'", path);

	if (!cfg::node::save(path))
	{
		input_log.error("Failed to save input configurations config to '%s' (error=%s)", path, fs::g_tls_error);
	}
}
