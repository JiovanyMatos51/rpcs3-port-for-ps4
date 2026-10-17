#pragma once

#ifdef RPCS3_PS4

#include "Emu/Io/PadHandler.h"

#include <orbis/Pad.h>

#include <array>

// One seat of rpcs3/ps4_pads.h: device "PS4 Controller N" is player N. The pad handle itself belongs to ps4_pads.
class ps4_pad_device final : public PadDevice
{
public:
	u32 slot = 0;      // 0 = player 1
	s32 user_id = -1;  // PS4 user seated here, -1 = free seat
	OrbisPadData data{};
	u64 last_timestamp = 0;  // timestamp of the last sample that went through get_mapping
	u32 unchanged_polls = 0; // polls answered with no_data since then
	steady_clock::time_point reconnect_after{}; // forced disconnect after the seat changed user
	steady_clock::time_point rumble_retry{};    // a refused rumble value is sent again after this
	steady_clock::time_point exit_chord_start{};
	u32 chord_prev_buttons = 0;                             // buttons of the previous poll, for press edges
	std::array<steady_clock::time_point, 3> chord_press{}; // last press of L1, R1 and Triangle
	steady_clock::time_point motion_log_at{};   // next pad-motion-log.flag line of this seat
};

// 0.33: which of the two vectors that follow the quaternion in OrbisPadData is the accelerometer, and its unit and
// sign. Learned from the samples instead of trusting the field names (see ps4_pad_handler.cpp).
struct ps4_motion_layout
{
	enum class source : u8 { detecting, detected, fallback };

	source state = source::detecting;
	bool accel_first = true;     // the vector right after the quaternion is the acceleration (Sony's order)
	f32 accel_scale = 1.0f;      // to g, signed so that a controller lying flat reads +1 g on Y
	f32 gyro_to_deg = 57.29578f; // angular velocity to degrees/s (the SDK reports rad/s)
	u32 votes[2]{};              // samples where only that vector looked like gravity
	f64 gravity_y[2]{};          // sum of the Y components of those samples
	f64 gravity_mag[2]{};        // sum of their magnitudes
	u32 samples = 0;             // samples with any non-zero motion value
	u32 zero_samples = 0;        // samples with both vectors zero (motion sensors off)
	u32 fast_gyro = 0;           // samples whose angular velocity is impossible in rad/s

	bool known() const { return state != source::detecting; }
	// Feeds one connected sample. Returns true once the layout is known (detected, or the SDK order as a fallback).
	bool observe(const vec_float3& first, const vec_float3& second);
	// Once known: returns true when the angular velocity has just been switched to degrees/s because the values
	// cannot be rad/s.
	bool check_gyro_unit(const vec_float3& gyro);
};

class ps4_pad_handler final : public PadHandlerBase
{
public:
	ps4_pad_handler();
	~ps4_pad_handler() override;

	bool Init() override;
	void process() override;
	void init_config(cfg_pad* cfg) override;
	std::vector<pad_list_entry> list_devices() override;

private:
	enum key_code : u32
	{
		key_up = 1, key_down, key_left, key_right,
		key_cross, key_square, key_circle, key_triangle,
		key_l1, key_l2, key_l3, key_r1, key_r2, key_r3,
		key_options, key_touchpad,
		key_ls_left, key_ls_right, key_ls_down, key_ls_up,
		key_rs_left, key_rs_right, key_rs_down, key_rs_up,
	};

	std::array<std::shared_ptr<ps4_pad_device>, 4> m_devices{};
	unsigned m_generation = 0; // ps4_pads::generation() seen by the last sync_users
	ps4_motion_layout m_motion{};  // one libScePad for every seat, so one layout
	bool m_motion_off = false;     // pad-no-motion.flag: SIXAXIS stays neutral, as before 0.33
	bool m_motion_log = false;     // pad-motion-log.flag: one raw/converted motion line per seat per second
	bool m_motion_zero_warned = false;

	void sync_users(bool force);
	static void reset_device(ps4_pad_device& device);
	std::shared_ptr<PadDevice> get_device(const std::string& device) override;
	connection update_connection(const std::shared_ptr<PadDevice>& device) override;
	std::unordered_map<u32, u16> get_button_values(const std::shared_ptr<PadDevice>& device) override;
	bool get_is_left_trigger(const std::shared_ptr<PadDevice>&, u32 code) override { return code == key_l2; }
	bool get_is_right_trigger(const std::shared_ptr<PadDevice>&, u32 code) override { return code == key_r2; }
	bool get_is_left_stick(const std::shared_ptr<PadDevice>&, u32 code) override;
	bool get_is_right_stick(const std::shared_ptr<PadDevice>&, u32 code) override;
	void get_extended_info(const pad_ensemble& binding) override;
	void apply_pad_data(const pad_ensemble& binding) override;
};

#endif
