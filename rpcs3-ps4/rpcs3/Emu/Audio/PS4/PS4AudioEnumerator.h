#pragma once

#ifdef RPCS3_PS4

#include "Emu/Audio/audio_device_enumerator.h"

class PS4AudioEnumerator final : public audio_device_enumerator
{
public:
	std::vector<audio_device> get_output_devices() override
	{
		return {{std::string(DEFAULT_DEV_ID), "PS4 Main Audio", 2}};
	}
};

#endif
