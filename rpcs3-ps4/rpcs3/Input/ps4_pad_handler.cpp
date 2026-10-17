#include "stdafx.h"
#include "ps4_pad_handler.h"

#ifdef RPCS3_PS4

#include "pad_thread.h"
#include "ps4_launcher.h"
#include "ps4_pads.h"

#include <algorithm>
#include <cmath>

// 0.31: the scePad handles and the PS4 user-to-seat mapping belong to ps4_pads, which keeps them open across games and
// shares them with the XMB. This handler only reads the seats through it: device "PS4 Controller N" is player N.
static_assert(ps4_pads::max_players == 4, "one device per PS4 login seat");

namespace
{
// 0.33: the DS4 motion data is reported to games as DS3 SIXAXIS sensors, like ds4_pad_handler does on PC.
// Present: the sensors stay at the neutral DS3 values (flat and still), as before 0.33.
constexpr char motion_off_flag[] = "/data/rpcs3/pad-no-motion.flag";
// Present: one line per seat per second with the raw motion vectors and the SIXAXIS values sent to the game.
constexpr char motion_log_flag[] = "/data/rpcs3/pad-motion-log.flag";

// OpenOrbis names the two vectors after the quaternion 'vel' and 'acell', while Sony's ScePadData has the
// acceleration (g) first and the angular velocity (rad/s) second there, so the names may be swapped. A controller
// held still tells them apart: only gravity has a magnitude of about 1 g (9.8 if the unit were m/s^2), while the
// angular velocity stays near zero.
constexpr u32 motion_votes_needed = 32;        // a still controller supplies these within a fraction of a second
constexpr u32 motion_fallback_samples = 4096;  // non-zero samples without a clear answer: use the SDK order
constexpr u32 motion_zero_warning = 2048;      // all-zero samples before warning that the sensors look off
constexpr f32 standard_gravity = 9.80665f;

f32 length(const vec_float3& v)
{
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// Factor to g when 'magnitude' fits gravity alone, in g or in m/s^2; 0 otherwise.
f32 gravity_unit(f32 magnitude)
{
	if (magnitude > 0.8f && magnitude < 1.25f) return 1.0f;
	if (magnitude > 7.8f && magnitude < 12.3f) return 1.0f / standard_gravity;
	return 0.0f;
}
} // namespace

bool ps4_motion_layout::observe(const vec_float3& first, const vec_float3& second)
{
	if (known())
	{
		return true;
	}
	const vec_float3* vectors[2] = {&first, &second};
	const f32 magnitude[2] = {length(first), length(second)};
	if (magnitude[0] < 1e-3f && magnitude[1] < 1e-3f)
	{
		++zero_samples;
		return false;
	}
	++samples;
	for (u32 i = 0; i < 2; ++i)
	{
		// Gravity alone: the other vector (angular velocity) is small and the controller is about flat, so Y carries
		// most of it. A turning or shaken controller does not vote, and both vectors can never vote for one sample.
		const vec_float3& v = *vectors[i];
		if (gravity_unit(magnitude[i]) == 0.0f || magnitude[1 - i] >= 0.5f || std::fabs(v.y) < 0.8f * magnitude[i])
		{
			continue;
		}
		++votes[i];
		gravity_y[i] += v.y;
		gravity_mag[i] += magnitude[i];
	}
	for (u32 i = 0; i < 2; ++i)
	{
		if (votes[i] >= motion_votes_needed && votes[1 - i] * 8 <= votes[i])
		{
			state = source::detected;
			accel_first = i == 0;
			// The conversion below follows the DS4 HID convention, where a controller lying flat reads +1 g on Y.
			const f64 mean = gravity_mag[i] / votes[i];
			accel_scale = (mean > 4.0 ? 1.0f / standard_gravity : 1.0f) * (gravity_y[i] >= 0.0 ? 1.0f : -1.0f);
			return true;
		}
	}
	if (samples >= motion_fallback_samples)
	{
		state = source::fallback;
		accel_first = true;
		accel_scale = 1.0f;
		return true;
	}
	return false;
}

bool ps4_motion_layout::check_gyro_unit(const vec_float3& gyro)
{
	// The DS4 gyro saturates at 2000 degrees/s (35 rad/s): several larger values mean the unit is degrees/s.
	if (gyro_to_deg == 1.0f || length(gyro) <= 40.0f || ++fast_gyro < 4)
	{
		return false;
	}
	gyro_to_deg = 1.0f;
	return true;
}

ps4_pad_handler::ps4_pad_handler() : PadHandlerBase(pad_handler::ps4)
{
	button_list = {
		{key_up, "Up"}, {key_down, "Down"}, {key_left, "Left"}, {key_right, "Right"},
		{key_cross, "Cross"}, {key_square, "Square"}, {key_circle, "Circle"}, {key_triangle, "Triangle"},
		{key_l1, "L1"}, {key_l2, "L2"}, {key_l3, "L3"}, {key_r1, "R1"}, {key_r2, "R2"}, {key_r3, "R3"},
		{key_options, "Options"}, {key_touchpad, "Touchpad"},
		{key_ls_left, "LS Left"}, {key_ls_right, "LS Right"}, {key_ls_down, "LS Down"}, {key_ls_up, "LS Up"},
		{key_rs_left, "RS Left"}, {key_rs_right, "RS Right"}, {key_rs_down, "RS Down"}, {key_rs_up, "RS Up"},
	};
	init_configs();
	m_name_string = "PS4 Controller ";
	m_max_devices = m_devices.size();
	thumb_max = 127;
	trigger_max = 255;
	m_thumb_threshold = thumb_max / 2;
	m_trigger_threshold = trigger_max / 2;
	b_has_config = true;
	b_has_deadzones = true;
	b_has_rumble = true;
	// Read when the handler is created: at every game boot and pad reset.
	m_motion_off = fs::is_file(motion_off_flag);
	m_motion_log = !m_motion_off && fs::is_file(motion_log_flag);
	b_has_motion = !m_motion_off;
	// The orientation (PS Move emulation from a pad) works only when "Orientation Enabled" is set in the pad config.
	b_has_orientation = !m_motion_off;

	// Every seat has a device from the start, so a player is never bound to the Null handler just because its seat
	// was empty when the game booted. A free seat reports "disconnected" until someone logs in.
	for (u32 slot = 0; slot < m_devices.size(); ++slot)
	{
		m_devices[slot] = std::make_shared<ps4_pad_device>();
		m_devices[slot]->slot = slot;
	}
}

ps4_pad_handler::~ps4_pad_handler()
{
	if (!m_is_init)
	{
		return;
	}
	// Stop the motors this game may have left running. The handles stay open: ps4_pads owns them.
	for (const auto& device : m_devices)
	{
		if (device->user_id >= 0)
		{
			ps4_pads::set_vibration(device->slot, 0, 0);
		}
	}
}

// Forgets the previous controller of a seat. The motor cache goes to 0 so the current rumble is sent again to the
// next controller, and a held exit chord starts over.
void ps4_pad_handler::reset_device(ps4_pad_device& device)
{
	device.data = {};
	device.last_timestamp = 0;
	device.unchanged_polls = 0;
	device.large_motor = 0;
	device.small_motor = 0;
	device.new_output_data = true;
	device.rumble_retry = {};
	device.exit_chord_start = {};
}

bool ps4_pad_handler::Init()
{
	if (m_is_init)
	{
		return true;
	}
	if (!ps4_pads::initialize())
	{
		// The bindings are kept: process() retries the library once per second and the seats connect then.
		input_log.error("PS4 pads: the controller library is not ready; retrying every second");
	}
	m_is_init = true;
	// Seats are compacted (connected controllers first) only for the boot of a game, before it has seen them. A pad
	// reset during the game (pad::g_reset) creates a new handler, and then a seated player must not move.
	ps4_pads::refresh(!pad::g_started, true);
	sync_users(true);
	input_log.notice("PS4 pads: %s", ps4_pads::summary());
	input_log.notice("PS4 pads: SIXAXIS motion %s", m_motion_off ? "off (pad-no-motion.flag)" :
		m_motion_log ? "on, logged every second (pad-motion-log.flag)" : "on");
	return true;
}

void ps4_pad_handler::sync_users(bool force)
{
	const unsigned generation = ps4_pads::generation();
	if (!force && generation == m_generation)
	{
		return;
	}
	m_generation = generation;

	const auto now = steady_clock::now();
	for (const auto& device : m_devices)
	{
		const s32 user = ps4_pads::user(device->slot);
		if (user == device->user_id)
		{
			continue;
		}
		input_log.notice("PS4 pads: player %u is now user 0x%x (was 0x%x)", device->slot + 1, user, device->user_id);
		// A seat that passes to another user looks unplugged for 100 ms, so the game sees the old controller leave
		// and a new one arrive (CELL_PAD_STATUS_ASSIGN_CHANGES) instead of a silent swap.
		const bool swapped = device->user_id >= 0 && user >= 0;
		device->user_id = user;
		reset_device(*device);
		device->reconnect_after = swapped ? now + 100ms : steady_clock::time_point{};
	}
}

void ps4_pad_handler::process()
{
	// Cheap when nothing is due: ps4_pads only polls the system every 100 ms.
	ps4_pads::refresh(false);
	sync_users(false);
	PadHandlerBase::process();
}

std::vector<pad_list_entry> ps4_pad_handler::list_devices()
{
	std::vector<pad_list_entry> result;
	for (u32 slot = 0; slot < m_devices.size(); ++slot)
	{
		result.emplace_back(m_name_string + std::to_string(slot + 1), false);
	}
	return result;
}

std::shared_ptr<PadDevice> ps4_pad_handler::get_device(const std::string& name)
{
	for (u32 slot = 0; slot < m_devices.size(); ++slot)
	{
		if (name == m_name_string + std::to_string(slot + 1))
		{
			return m_devices[slot];
		}
	}
	return {};
}

PadHandlerBase::connection ps4_pad_handler::update_connection(const std::shared_ptr<PadDevice>& base)
{
	auto* device = static_cast<ps4_pad_device*>(base.get());
	if (!device || device->user_id < 0)
	{
		return connection::disconnected;
	}
	const auto now = steady_clock::now();
	if (now < device->reconnect_after)
	{
		return connection::disconnected;
	}
	if (!ps4_pads::read(device->slot, device->data))
	{
		// Controller off, handle lost or seat being reopened.
		reset_device(*device);
		return connection::disconnected;
	}

	// L1+R1+Triangle pressed together (within 300 ms of each other) and held for 1 s, or L1+R1+Options held for 2 s,
	// on any seat returns to the XMB. Requiring the three presses together keeps games that hold aim (L1) and fire
	// (R1) while using Triangle from closing by accident.
	constexpr u32 exit_chord = ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_OPTIONS;
	constexpr u32 quick_exit_chord = ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_TRIANGLE;
	constexpr std::array<u32, 3> quick_buttons{ORBIS_PAD_BUTTON_L1, ORBIS_PAD_BUTTON_R1, ORBIS_PAD_BUTTON_TRIANGLE};
	const u32 pressed_now = device->data.buttons & ~device->chord_prev_buttons;
	device->chord_prev_buttons = device->data.buttons;
	for (usz i = 0; i < quick_buttons.size(); i++)
	{
		if (pressed_now & quick_buttons[i]) device->chord_press[i] = now;
	}
	const auto [first_press, last_press] = std::minmax({device->chord_press[0], device->chord_press[1], device->chord_press[2]});
	const bool quick_chord = (device->data.buttons & quick_exit_chord) == quick_exit_chord &&
		first_press.time_since_epoch().count() && last_press - first_press <= 300ms;
	if (quick_chord || (device->data.buttons & exit_chord) == exit_chord)
	{
		if (!device->exit_chord_start.time_since_epoch().count()) device->exit_chord_start = now;
		if (now - device->exit_chord_start >= (quick_chord ? 1s : 2s)) ps4_request_return_to_menu();
	}
	else
	{
		device->exit_chord_start = {};
	}

	// The controller reports less often than this poll runs. A sample with an unchanged timestamp keeps the previous
	// button state without rebuilding the mapping. Every 16th unchanged poll is mapped anyway, so a timestamp that
	// never moves (unexpected data layout) delays the input by at most 16 polls but never freezes it.
	const u64 timestamp = device->data.timestamp;
	if (timestamp && timestamp == device->last_timestamp && ++device->unchanged_polls < 16)
	{
		return connection::no_data;
	}
	device->unchanged_polls = 0;
	device->last_timestamp = timestamp;
	return connection::connected;
}

std::unordered_map<u32, u16> ps4_pad_handler::get_button_values(const std::shared_ptr<PadDevice>& base)
{
	std::unordered_map<u32, u16> values;
	const auto* device = static_cast<const ps4_pad_device*>(base.get());
	if (!device || !device->data.connected)
	{
		return values;
	}
	const auto& data = device->data;
	const auto pressed = [&](u32 flag) -> u16 { return data.buttons & flag ? 255 : 0; };
	values[key_up] = pressed(ORBIS_PAD_BUTTON_UP);
	values[key_down] = pressed(ORBIS_PAD_BUTTON_DOWN);
	values[key_left] = pressed(ORBIS_PAD_BUTTON_LEFT);
	values[key_right] = pressed(ORBIS_PAD_BUTTON_RIGHT);
	values[key_cross] = pressed(ORBIS_PAD_BUTTON_CROSS);
	values[key_square] = pressed(ORBIS_PAD_BUTTON_SQUARE);
	values[key_circle] = pressed(ORBIS_PAD_BUTTON_CIRCLE);
	values[key_triangle] = pressed(ORBIS_PAD_BUTTON_TRIANGLE);
	values[key_l1] = pressed(ORBIS_PAD_BUTTON_L1);
	values[key_l2] = data.analogButtons.l2;
	values[key_l3] = pressed(ORBIS_PAD_BUTTON_L3);
	values[key_r1] = pressed(ORBIS_PAD_BUTTON_R1);
	values[key_r2] = data.analogButtons.r2;
	values[key_r3] = pressed(ORBIS_PAD_BUTTON_R3);
	values[key_options] = pressed(ORBIS_PAD_BUTTON_OPTIONS);
	values[key_touchpad] = pressed(ORBIS_PAD_BUTTON_TOUCH_PAD);
	const auto axis_negative = [](u8 value) -> u16 { return value < 128 ? 128 - value : 0; };
	const auto axis_positive = [](u8 value) -> u16 { return value > 128 ? value - 128 : 0; };
	values[key_ls_left] = axis_negative(data.leftStick.x);
	values[key_ls_right] = axis_positive(data.leftStick.x);
	values[key_ls_up] = axis_negative(data.leftStick.y);
	values[key_ls_down] = axis_positive(data.leftStick.y);
	values[key_rs_left] = axis_negative(data.rightStick.x);
	values[key_rs_right] = axis_positive(data.rightStick.x);
	values[key_rs_up] = axis_negative(data.rightStick.y);
	values[key_rs_down] = axis_positive(data.rightStick.y);
	return values;
}

bool ps4_pad_handler::get_is_left_stick(const std::shared_ptr<PadDevice>&, u32 code)
{
	return code >= key_ls_left && code <= key_ls_up;
}

bool ps4_pad_handler::get_is_right_stick(const std::shared_ptr<PadDevice>&, u32 code)
{
	return code >= key_rs_left && code <= key_rs_up;
}

void ps4_pad_handler::get_extended_info(const pad_ensemble& binding)
{
	// Battery level and cable state are not in OrbisPadData; they stay unknown instead of being invented.
	auto* device = static_cast<ps4_pad_device*>(binding.device.get());
	const auto& pad = binding.pad;
	if (m_motion_off || !device || !pad || !device->data.connected)
	{
		return;
	}

	const OrbisPadData& data = device->data;
	const bool was_known = m_motion.known();
	if (!m_motion.observe(data.vel, data.acell))
	{
		// The DS3 defaults (flat and still) stay until the layout is known.
		if (!m_motion_zero_warned && !m_motion.samples && m_motion.zero_samples >= motion_zero_warning)
		{
			m_motion_zero_warned = true;
			input_log.warning("PS4 pads: the motion vectors read zero in %u samples (motion sensors off?); SIXAXIS stays neutral", m_motion.zero_samples);
		}
		return;
	}
	if (!was_known)
	{
		if (m_motion.state == ps4_motion_layout::source::detected)
		{
			input_log.notice("PS4 pads: motion layout: acceleration is the %s vector after the quaternion, in %s, a flat controller reads %s1 g on Y (gravity samples %u/%u of %u)",
				m_motion.accel_first ? "first" : "second", std::fabs(m_motion.accel_scale) < 0.5f ? "m/s^2" : "g", m_motion.accel_scale > 0.0f ? "+" : "-",
				m_motion.votes[0], m_motion.votes[1], m_motion.samples);
		}
		else
		{
			input_log.warning("PS4 pads: motion layout not detected in %u samples (gravity samples %u/%u); using the SDK order: acceleration first, in g",
				m_motion.samples, m_motion.votes[0], m_motion.votes[1]);
		}
	}

	const vec_float3& accel = m_motion.accel_first ? data.vel : data.acell;
	const vec_float3& gyro = m_motion.accel_first ? data.acell : data.vel;
	if (m_motion.check_gyro_unit(gyro))
	{
		input_log.warning("PS4 pads: angular velocity above 40 rad/s; reading it as degrees/s from now on");
	}

	// The layout is shared by every seat, but the sensor state belongs to each handle: a seat whose sensors are off
	// (scePadSetMotionSensorState refused for its handle, or reset when the controller reconnected) reads both vectors
	// as zero. Converted, that is a weightless controller (0 g on every axis, which a DS3 never reports); keep the
	// neutral DS3 values (flat and still) for it instead, as before the layout is known.
	if (length(accel) < 1e-3f && length(gyro) < 1e-3f)
	{
		pad->m_sensors[0].m_value = DEFAULT_MOTION_X;
		pad->m_sensors[1].m_value = DEFAULT_MOTION_Y;
		pad->m_sensors[2].m_value = DEFAULT_MOTION_Z;
		pad->m_sensors[3].m_value = DEFAULT_MOTION_G;
		set_raw_orientation(*pad);
	}
	else
	{
		// Same conversion as ds4_pad_handler from its HID report: DS4 axes in g and degrees/s, negated into the DS3 frame.
		const f32 accel_x = accel.x * m_motion.accel_scale * -1;
		const f32 accel_y = accel.y * m_motion.accel_scale * -1;
		const f32 accel_z = accel.z * m_motion.accel_scale * -1;
		const f32 gyro_x = gyro.x * m_motion.gyro_to_deg * -1;
		const f32 gyro_y = gyro.y * m_motion.gyro_to_deg * -1;
		const f32 gyro_z = gyro.z * m_motion.gyro_to_deg * -1;

		pad->m_sensors[0].m_value = Clamp0To1023(accel_x * MOTION_ONE_G + 512);
		pad->m_sensors[1].m_value = Clamp0To1023(accel_y * MOTION_ONE_G + 512);
		pad->m_sensors[2].m_value = Clamp0To1023(accel_z * MOTION_ONE_G + 512);
		// gyro_y is yaw, the only rate a DS3 reports; its resolution is 123/90 per degree/s.
		pad->m_sensors[3].m_value = Clamp0To1023(gyro_y * (123.f / 90.f) + 512);

		set_raw_orientation(pad->move_data, accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z);
	}

	if (m_motion_log)
	{
		const auto now = steady_clock::now();
		if (now >= device->motion_log_at)
		{
			device->motion_log_at = now + 1s;
			input_log.notice("PS4 pads: J%u motion: accel (%+.2f %+.2f %+.2f) gyro (%+.2f %+.2f %+.2f) -> SIXAXIS X %u Y %u Z %u G %u",
				device->slot + 1, accel.x, accel.y, accel.z, gyro.x, gyro.y, gyro.z,
				pad->m_sensors[0].m_value, pad->m_sensors[1].m_value, pad->m_sensors[2].m_value, pad->m_sensors[3].m_value);
		}
	}
}

void ps4_pad_handler::apply_pad_data(const pad_ensemble& binding)
{
	auto* device = static_cast<ps4_pad_device*>(binding.device.get());
	if (!device || !device->config || !binding.pad || device->user_id < 0)
	{
		return;
	}
	const u8 large = device->config->get_large_motor_speed(binding.pad->m_vibrate_motors);
	const u8 small = device->config->get_small_motor_speed(binding.pad->m_vibrate_motors);
	if (large == device->large_motor && small == device->small_motor)
	{
		return;
	}
	const auto now = steady_clock::now();
	if (now < device->rumble_retry)
	{
		return;
	}
	if (ps4_pads::vibrate(device->slot, large, small))
	{
		device->large_motor = large;
		device->small_motor = small;
	}
	else
	{
		// Not cached, so a refused "stop" is not lost; retried at most every 50 ms instead of on every poll.
		device->rumble_retry = now + 50ms;
	}
}

void ps4_pad_handler::init_config(cfg_pad* cfg)
{
	if (!cfg)
	{
		return;
	}
	const auto name = [&](u32 code) { return ::at32(button_list, code); };
	cfg->ls_left.def = name(key_ls_left);
	cfg->ls_right.def = name(key_ls_right);
	cfg->ls_up.def = name(key_ls_up);
	cfg->ls_down.def = name(key_ls_down);
	cfg->rs_left.def = name(key_rs_left);
	cfg->rs_right.def = name(key_rs_right);
	cfg->rs_up.def = name(key_rs_up);
	cfg->rs_down.def = name(key_rs_down);
	cfg->up.def = name(key_up);
	cfg->down.def = name(key_down);
	cfg->left.def = name(key_left);
	cfg->right.def = name(key_right);
	cfg->cross.def = name(key_cross);
	cfg->square.def = name(key_square);
	cfg->circle.def = name(key_circle);
	cfg->triangle.def = name(key_triangle);
	cfg->l1.def = name(key_l1);
	cfg->l2.def = name(key_l2);
	cfg->l3.def = name(key_l3);
	cfg->r1.def = name(key_r1);
	cfg->r2.def = name(key_r2);
	cfg->r3.def = name(key_r3);
	cfg->start.def = name(key_options);
	cfg->select.def = name(key_touchpad);
	cfg->ps.def = cfg_pad::make_button_string(button_list, {{key_options, key_touchpad}});
	cfg->lstickdeadzone.def = 12;
	cfg->rstickdeadzone.def = 12;
	cfg->ltriggerthreshold.def = 8;
	cfg->rtriggerthreshold.def = 8;
	cfg->from_default();
}

#endif
