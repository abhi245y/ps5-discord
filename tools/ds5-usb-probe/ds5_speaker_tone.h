// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef DS5_SPEAKER_TONE_H
#define DS5_SPEAKER_TONE_H
#include <math.h>
#include "ds5_mic_format.h"

#define DS5_SPEAKER_AMPLITUDE 6000

static inline void ds5_speaker_mono_packet(uint8_t packet[384], const uint8_t mono[96])
{
    memset(packet, 0, 384);
    for (unsigned s = 0; s < 48; s++)
        memcpy(packet + s * 8 + 2, mono + s * 2, 2);
}

// One millisecond of quad-channel S16_LE at 48 kHz. Output path 3 sends
// channel 1 to the built-in speaker; channels 2/3 are haptics and stay zero.
static inline void ds5_speaker_packet(uint8_t packet[384], uint32_t first_sample)
{
    static const unsigned frequency[4] = {440, 660, 880, 660};
    memset(packet, 0, 384);
    for (unsigned s = 0; s < 48; s++) {
        uint32_t sample = first_sample + s;
        unsigned cycle = sample / 96000u, position = sample % 96000u;
        double envelope = 0;
        if (position < 24000) {
            envelope = 1;
            if (position < 480)
                envelope = position / 480.0;
            else if (position >= 23520)
                envelope = (23999 - position) / 480.0;
        }
        int16_t value = (int16_t)(DS5_SPEAKER_AMPLITUDE * envelope * sin(6.283185307179586 * frequency[cycle % 4] *
                                                     position / 48000.0));
        ds5_put_le(packet + s * 8 + 2, (uint16_t)value, 2);
    }
}
#endif
