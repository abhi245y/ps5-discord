// SPDX-License-Identifier: AGPL-3.0-or-later
#include <assert.h>
#include <stdio.h>
#include "ds5_loopback_audio.h"

int main(void)
{
    static struct ds5_loop_ring ring;
    ds5_loop_reset(&ring);
    for (unsigned i = 0; i < DS5_LOOP_PREFILL; i++)
        assert(ds5_loop_pop(&ring) == 0);
    assert(ring.count == 0 && ring.underruns == 0);
    assert(ds5_loop_pop(&ring) == 0 && ring.underruns == 1);
    for (unsigned i = 0; i < DS5_LOOP_CAPACITY + 100; i++)
        ds5_loop_push(&ring, (int)i);
    assert(ring.count == DS5_LOOP_CAPACITY && ring.overruns == 100);
    for (unsigned i = 100; i < DS5_LOOP_CAPACITY + 100; i++)
        assert(ds5_loop_pop(&ring) == (int)i);
    assert(ring.count == 0);

    uint8_t packet[384], status[64] = {0}, report[63];
    for (int headphones = 0; headphones < 2; headphones++)
        for (int muted = 0; muted < 2; muted++) {
            for (unsigned i = 0; i < 48; i++)
                ds5_loop_push(&ring, i & 1 ? -32768 : 32767);
            ds5_loop_packet(&ring, packet, headphones, muted);
            for (unsigned i = 0; i < 48; i++) {
                int expected = muted ? 0 : (i & 1 ? -32768 : 32767) / (headphones ? 2 : 1);
                int limit = headphones ? DS5_LOOP_HEADPHONE_LIMIT : DS5_LOOP_LIMIT;
                if (expected > limit) expected = limit;
                if (expected < -limit) expected = -limit;
                assert(ds5_signed_sample(packet + i * 8 + 2) == expected);
                assert(ds5_signed_sample(packet + i * 8) == (headphones ? expected : 0));
                assert(ds5_le16(packet + i * 8 + 4) == 0 && ds5_le16(packet + i * 8 + 6) == 0);
            }
            ds5_loop_route(report, headphones, muted);
            assert(report[0] == 2 && report[1] == 0xf0 && report[2] == 0x83);
            assert(report[5] == (headphones ? 0x7f : 0) && report[6] == (headphones ? 0 : 0x64));
            assert(report[7] == 0x20 && report[8] == (headphones ? 1 : 0x31));
            assert(report[9] == muted && report[10] == (muted ? 0x10 : 0));
            assert(report[3] == 0 && report[4] == 0 && report[11] == 0 && report[22] == 0);
        }
    int hp, muted;
    uint8_t buttons;
    assert(!ds5_loop_status(status, 64, &hp, &muted, &buttons));
    status[0] = 1;
    status[10] = 5;
    status[54] = 7;
    assert(ds5_loop_status(status, 64, &hp, &muted, &buttons) && hp && muted && buttons == 5);
    for (size_t size = 0; size < 64; size++)
        assert(!ds5_loop_status(status, size, &hp, &muted, &buttons));
    status[54] = 2; // A mic-detect bit alone must not be treated as headphones.
    assert(ds5_loop_status(status, 64, &hp, &muted, &buttons) && !hp && !muted);
    ring.overruns = 17;
    ring.underruns = 23;
    ds5_loop_reset(&ring);
    assert(ring.count == DS5_LOOP_PREFILL && ring.overruns == 17 && ring.underruns == 23);
    puts("loopback: ring wrap/overrun/underrun, gain limits, mute, zero haptics, routes and HID status passed");
    return 0;
}
