#include "stdafx.h"
#include "system_config.h"

#if defined(RPCS3_PS4) && !defined(HAVE_VULKAN)
// ps4_settings.hpp constructs a second cfg_root in the frontend (HAVE_VULKAN=1); both TUs must agree
// on cfg_root's inline constructors (default renderer) or the ODR is violated.
#error "PS4: system_config.cpp must see HAVE_VULKAN like the frontend (cfg_root ODR)"
#endif

#include <random>

cfg_root g_cfg{};
cfg_root g_backup_cfg{};

std::string cfg_root::node_sys::get_random_system_name()
{
	std::srand(static_cast<u32>(std::time(nullptr)));
	return "RPCS3-" + std::to_string(100 + std::rand() % 899);
}

u128 cfg_root::node_sys::get_random_psid()
{
	// Generate seed for each 32-bits for maximum entropy using Standard Library
	std::uniform_int_distribution<u32> uniform_dist;
	std::mt19937 prng;

	u128 result{};
	prng.seed(std::random_device()()), result += u128{uniform_dist(prng)} << 0;
	prng.seed(std::random_device()()), result += u128{uniform_dist(prng)} << 32;
	prng.seed(std::random_device()()), result += u128{uniform_dist(prng)} << 64;
	prng.seed(std::random_device()()), result += u128{uniform_dist(prng)} << 96;

	return result;
}
