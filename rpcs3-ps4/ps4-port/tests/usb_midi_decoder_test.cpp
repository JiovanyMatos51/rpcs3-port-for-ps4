#include "../runtime/usb_midi_decoder.hpp"
#include <cassert>

int main()
{
    usb_midi_decoder decoder;
    std::vector<std::vector<unsigned char>> received;
    auto accept = [&](const unsigned char* bytes, size_t size) { received.emplace_back(bytes, bytes + size); };
    auto send = [&](std::array<unsigned char, 4> packet, bool sysex = false, bool timing = false, bool sense = false)
    { decoder.packet(packet.data(), 0, sysex, timing, sense, accept); };
    send({0x09, 0x90, 60, 100});
    assert(received.back() == (std::vector<unsigned char>{0x90, 60, 100}));
    send({0x0c, 0xc0, 7, 0});
    assert(received.back() == (std::vector<unsigned char>{0xc0, 7}));
    send({0x19, 0x90, 61, 90}); // Other cable.
    assert(received.size() == 2);
    send({0x04, 0xf0, 1, 2});
    send({0x0f, 0xf8, 0, 0}); // Realtime may be interleaved within SysEx.
    send({0x06, 3, 0xf7, 0});
    assert(received[2] == (std::vector<unsigned char>{0xf8}));
    assert(received.back() == (std::vector<unsigned char>{0xf0, 1, 2, 3, 0xf7}));
    send({0x05, 0xf6, 0, 0}); // CIN 5 also carries a one-byte system common event.
    assert(received.back() == (std::vector<unsigned char>{0xf6}));
    const size_t count = received.size();
    send({0x0f, 0xf8, 0, 0}, false, true);
    send({0x0f, 0xfe, 0, 0}, false, false, true);
    send({0x07, 0xf0, 1, 0xf7}, true);
    send({0x00, 0x90, 60, 1});
    send({0x0f, 0x90, 60, 1});
    assert(received.size() == count);
}
