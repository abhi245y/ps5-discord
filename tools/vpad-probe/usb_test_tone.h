// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef VPAD_USB_TEST_TONE_H
#define VPAD_USB_TEST_TONE_H
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* Same 48 kHz / half-second beep, 1.5-second silence / 10 ms fade pattern
 * used by the hardware-tested ds5_speaker_tone.h. Stereo PCM goes through
 * ds5_usb_run's existing speaker/headphone routing, with no mic loopback. */
static inline void usb_test_tone(int16_t *stereo, size_t frames, uint64_t *cursor)
{
    static const unsigned frequency[] = {440, 660, 880, 660};
    for (size_t i = 0; i < frames; ++i, ++*cursor) {
        unsigned position = (unsigned)(*cursor % 96000u);
        unsigned hz = frequency[(*cursor / 96000u) % 4u];
        double envelope = 0;
        if (position < 24000u) {
            envelope = 1;
            if (position < 480u)
                envelope = position / 480.0;
            else if (position >= 23520u)
                envelope = (23999u - position) / 480.0;
        }
        int16_t sample = (int16_t)(6000 * envelope * sin(6.283185307179586 * hz * position / 48000.0));
        stereo[i * 2] = stereo[i * 2 + 1] = sample;
    }
}
#endif
