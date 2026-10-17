#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class usb_midi_decoder
{
    std::vector<unsigned char> sysex;

public:
    template <typename Emit>
    void packet(const unsigned char* bytes, unsigned int cable, bool ignore_sysex,
        bool ignore_timing, bool ignore_sense, Emit&& emit)
    {
        if ((bytes[0] >> 4) != cable) return;
        constexpr std::array<unsigned int, 16> lengths{0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
        const unsigned int cin = bytes[0] & 15;
        const unsigned int length = lengths[cin];
        if (!length) return;
        if (cin == 15)
        {
            if (bytes[1] < 0xf8 || (ignore_timing && bytes[1] == 0xf8) ||
                (ignore_sense && bytes[1] == 0xfe)) return;
            emit(bytes + 1, length);
            return;
        }
        if (cin >= 4 && cin <= 7 && (cin != 5 || bytes[1] == 0xf7 || !sysex.empty()))
        {
            if (bytes[1] == 0xf0) sysex.clear();
            if (!ignore_sysex)
            {
                if (sysex.empty() && bytes[1] != 0xf0) return;
                if (sysex.size() + length > 65536) { sysex.clear(); return; }
                sysex.insert(sysex.end(), bytes + 1, bytes + 1 + length);
            }
            if (cin != 4)
            {
                if (!ignore_sysex && !sysex.empty() && sysex.back() == 0xf7)
                    emit(sysex.data(), sysex.size());
                sysex.clear();
            }
            return;
        }
        if (ignore_timing && (bytes[1] == 0xf1 || bytes[1] == 0xf8)) return;
        if (ignore_sense && bytes[1] == 0xfe) return;
        emit(bytes + 1, length);
    }
};
