// SPDX-License-Identifier: AGPL-3.0-or-later
#include <assert.h>
#include <stdio.h>
#include "ds5_mic_format.h"
#include "ds5_speaker_tone.h"

// Deliberately non-contiguous interface numbers and alternate settings: ioctl
// indices must follow descriptor order, not these USB-visible identifiers.
static const uint8_t configuration[] = {
    9, 2, 73, 0, 2, 1, 0, 0x80, 50,
    9, 4, 4, 0, 0, 1, 1, 0, 0,
    9, 0x24, 1, 0, 1, 9, 0, 1, 7,
    9, 4, 7, 0, 0, 1, 2, 0, 0,
    9, 4, 7, 3, 1, 1, 2, 0, 0,
    7, 0x24, 1, 6, 1, 1, 0,
    11, 0x24, 2, 1, 2, 2, 16, 1, 0x80, 0xbb, 0,
    10, 5, 0x82, 5, 196, 0, 4, 0, 0, 0,
};

int main(void)
{
    struct ds5_mic_format f;
    uint8_t data[sizeof configuration + 4];
    assert(sizeof configuration == 73);
    assert(ds5_mic_find(configuration, sizeof configuration, &f) == 1);
    assert(f.interface_index == 1 && f.interface_number == 7);
    assert(f.alt_index == 1 && f.alt_setting == 3);
    assert(f.control_index == 0 && f.endpoint == 0x82 && f.packet_bytes == 196);

    // Every truncated prefix must fail, without any out-of-bounds access.
    for (size_t n = 0; n < sizeof configuration; n++)
        assert(ds5_mic_find(configuration, n, &f) == -1);
    memcpy(data, configuration, sizeof configuration);
    data[73] = 1; // Invalid trailing descriptor, even after a valid candidate.
    data[2] = 74;
    assert(ds5_mic_find(data, 74, &f) == -1);
    data[73] = 0;
    assert(ds5_mic_find(data, 74, &f) == -1);

    memcpy(data, configuration, sizeof configuration);
    data[65] = 2; // OUT endpoint is not a mic.
    assert(ds5_mic_find(data, 73, &f) == 0);
    data[65] = 0x82;
    data[66] = 0x11; // Explicit feedback endpoint is not PCM data.
    assert(ds5_mic_find(data, 73, &f) == 0);
    data[66] = 5;
    data[56] = 1; // Mono does not match stereo WAV contract.
    assert(ds5_mic_find(data, 73, &f) == 0);
    memcpy(data, configuration, sizeof configuration);
    data[59] = 0; // Continuous rate needs more descriptor bytes.
    assert(ds5_mic_find(data, 73, &f) == -1);
    memcpy(data, configuration, sizeof configuration);
    data[61] = 0x44; // Unsupported discrete sample rate.
    assert(ds5_mic_find(data, 73, &f) == 0);
    memcpy(data, configuration, sizeof configuration);
    data[25] = 4; // Truncated AudioControl collection list.
    assert(ds5_mic_find(data, 73, &f) == -1);

    memcpy(data, configuration, sizeof configuration);
    data[56] = 4; // Speaker stream is four-channel PCM, not stereo input.
    data[65] = 1;
    ds5_put_le(data + 67, 392, 2);
    assert(ds5_audio_find(data, 73, &f, 4, 0) == 1);
    assert(f.endpoint == 1 && f.packet_bytes == 392 && f.control_index == 0);
    assert(ds5_mic_find(data, 73, &f) == 0);
    ds5_put_le(data + 67, 196, 2);
    assert(ds5_audio_find(data, 73, &f, 4, 0) == 0);

    // Check the entire eight-second signal: no haptic/left-channel samples,
    // capped amplitude, four bounded beeps, and silence between beeps.
    unsigned nonzero[4] = {0};
    uint8_t packet[384];
    for (uint32_t first = 0; first < 48000u * 8u; first += 48) {
        ds5_speaker_packet(packet, first);
        for (unsigned s = 0; s < 48; s++) {
            const uint8_t *p = packet + s * 8;
            assert(ds5_le16(p) == 0 && ds5_le16(p + 4) == 0 && ds5_le16(p + 6) == 0);
            int value = ds5_le16(p + 2);
            if (value >= 32768)
                value -= 65536;
            assert(value >= -DS5_SPEAKER_AMPLITUDE && value <= DS5_SPEAKER_AMPLITUDE);
            if ((first + s) % 96000u >= 24000)
                assert(value == 0);
            nonzero[(first + s) / 96000u] += value != 0;
        }
    }
    for (unsigned i = 0; i < 4; i++)
        assert(nonzero[i] > 23000 && nonzero[i] < 24000);

    uint8_t mono[96];
    for (unsigned s = 0; s < 48; s++)
        ds5_put_le(mono + s * 2, (uint16_t)(s * 1381u), 2);
    ds5_speaker_mono_packet(packet, mono);
    for (unsigned s = 0; s < 48; s++) {
        assert(ds5_le16(packet + s * 8 + 2) == ds5_le16(mono + s * 2));
        assert(ds5_le16(packet + s * 8) == 0);
        assert(ds5_le16(packet + s * 8 + 4) == 0 && ds5_le16(packet + s * 8 + 6) == 0);
    }

    // Exercise every single-byte mutation under ASan/UBSan.
    for (size_t offset = 0; offset < sizeof configuration; offset++)
        for (unsigned value = 0; value <= 255; value++) {
            memcpy(data, configuration, sizeof configuration);
            data[offset] = (uint8_t)value;
            int rc = ds5_mic_find(data, sizeof configuration, &f);
            assert(rc >= -1 && rc <= 1);
        }

    uint8_t header[44];
    ds5_wav_header(header, 1920000);
    assert(!memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVEfmt ", 8));
    assert(!memcmp(header + 36, "data", 4));
    assert(ds5_le16(header + 20) == 1 && ds5_le16(header + 22) == 2);
    assert(ds5_le16(header + 24) == 48000 && ds5_le16(header + 34) == 16);
    assert(header[4] == 0x24 && header[5] == 0x4c && header[6] == 0x1d);
    assert(header[40] == 0 && header[41] == 0x4c && header[42] == 0x1d);
    puts("USB audio: descriptors, index mapping, mutations, WAV header, speaker tones/haptic silence passed");
    return 0;
}
