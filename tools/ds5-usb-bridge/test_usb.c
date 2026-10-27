// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ds5_usb_pcm.h"
#include "pcm_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_mapping(void)
{
    int16_t stereo[96];
    uint8_t packet[384];
    for (unsigned i = 0; i < 48; i++) {
        stereo[2 * i] = i & 1 ? -32768 : 32767;
        stereo[2 * i + 1] = i & 1 ? 1234 : -9876;
    }
    for (int headphones = 0; headphones < 2; headphones++) {
        ds5_usb_output_packet(packet, stereo, headphones, 32768);
        for (unsigned i = 0; i < 48; i++) {
            int left = stereo[2 * i], right = stereo[2 * i + 1];
            assert(ds5_usb_sample(packet + 8 * i) == (headphones ? left : 0));
            assert(ds5_usb_sample(packet + 8 * i + 2) == (headphones ? right : (left + right) / 2));
            assert(ds5_le16(packet + 8 * i + 4) == 0 && ds5_le16(packet + 8 * i + 6) == 0);
        }
    }
    assert(ds5_usb_gain(-32768, 65536) == -32768 && ds5_usb_gain(32767, 65536) == 32767);
    assert(ds5_usb_gain(-32768, 16384) == -16384 && ds5_usb_gain(32767, 0) == 0);
    ds5_usb_output_packet(packet, stereo, 1, 16384);
    for (unsigned i = 0; i < 48; i++) {
        assert(ds5_usb_sample(packet + i * 8) == stereo[i * 2] / 2);
        assert(ds5_usb_sample(packet + i * 8 + 2) == stereo[i * 2 + 1] / 2);
    }

    uint8_t report[64] = {1};
    report[1] = 0; report[2] = 128; report[3] = 255; report[4] = 127;
    report[5] = 200; report[6] = 250;
    report[8] = 0xf1; report[9] = 0xff; report[10] = 7;
    ds5_usb_controller state;
    assert(ds5_usb_parse_controller(report, 64, &state));
    assert(state.left_x == -32768 && state.left_y == 0 && state.right_x == 32512 && state.right_y == -256);
    assert(state.left_trigger == 200 && state.right_trigger == 250);
    assert((state.buttons & (DS5_UP | DS5_RIGHT)) == (DS5_UP | DS5_RIGHT));
    assert((state.buttons & (DS5_PS | DS5_MUTE | DS5_TOUCHPAD)) == (DS5_PS | DS5_MUTE | DS5_TOUCHPAD));
    assert((state.buttons & (DS5_CROSS | DS5_CIRCLE | DS5_SQUARE | DS5_TRIANGLE)) ==
           (DS5_CROSS | DS5_CIRCLE | DS5_SQUARE | DS5_TRIANGLE));
    assert((state.buttons & (DS5_L2 | DS5_R2 | DS5_L3 | DS5_R3)) == (DS5_L2 | DS5_R2 | DS5_L3 | DS5_R3));
    for (size_t n = 0; n < 64; n++)
        assert(!ds5_usb_parse_controller(report, n, &state));
    report[0] = 0;
    assert(!ds5_usb_parse_controller(report, 64, &state));

    ds5_usb_config config = {.mic_volume = 0x20, .headphone_volume = 0x7f,
        .speaker_volume = 0x64, .speaker_pre_gain = 2};
    uint8_t output[63];
    ds5_usb_route_report(output, &config, 0, 0);
    assert(output[5] == 0 && output[6] == 0x64 && output[7] == 0x20 && output[8] == 0x31);
    ds5_usb_route_report(output, &config, 1, 1);
    assert(output[5] == 0x7f && output[6] == 0 && output[8] == 1 && output[9] == 1 && output[10] == 0x10);
    assert(output[3] == 0 && output[4] == 0 && output[11] == 0 && output[22] == 0);
}

static void test_format(void)
{
    const uint8_t data[] = {9,2,73,0,2,1,0,0x80,50,
        9,4,4,0,0,1,1,0,0, 9,0x24,1,0,1,9,0,1,7,
        9,4,7,0,0,1,2,0,0, 9,4,7,3,1,1,2,0,0,
        7,0x24,1,6,1,1,0, 11,0x24,2,1,2,2,16,1,0x80,0xbb,0,
        10,5,0x82,5,196,0,4,0,0,0};
    struct ds5_mic_format format;
    assert(ds5_mic_find(data, sizeof data, &format) == 1);
    assert(format.interface_index == 1 && format.interface_number == 7 && format.alt_index == 1 && format.alt_setting == 3);
    for (size_t n = 0; n < sizeof data; n++)
        assert(ds5_mic_find(data, n, &format) == -1);
}

static void test_pcm_fragments(void)
{
    static ds5_pcm_stream stream;
    uint8_t bytes[4];
    int16_t stereo[2 * 768];
    for (unsigned i = 0; i < 3000; i++) {
        ds5_put_le(bytes, i, 2);
        ds5_put_le(bytes + 2, (uint16_t)(-(int)i), 2);
        for (unsigned b = 0; b < 4; b++)
            ds5_pcm_receive(&stream, bytes + b, 1);
    }
    assert(stream.count == 3000 && stream.carry_size == 0);
    unsigned received = 0;
    while (received < 3000) {
        size_t n = ds5_pcm_playback(&stream, stereo, 768);
        assert(n > 0);
        for (size_t i = 0; i < n; i++) {
            assert(stereo[2 * i] == (int)(received + i));
            assert(stereo[2 * i + 1] == -(int)(received + i));
        }
        received += (unsigned)n;
    }
    assert(!stream.primed && !stream.count && ds5_pcm_playback(&stream, stereo, 768) == 0);
    memset(&stream, 0, sizeof stream);
    for (unsigned i = 0; i < DS5_PCM_RX_FRAMES + 100; i++) {
        ds5_put_le(bytes, i, 2);
        ds5_put_le(bytes + 2, i, 2);
        ds5_pcm_receive(&stream, bytes, 4);
    }
    assert(stream.count == DS5_PCM_RX_FRAMES && stream.playback_dropped_frames == 100);
    assert(ds5_pcm_playback(&stream, stereo, 1) == 1 && stereo[0] == 100 && stereo[1] == 100);
}

static void test_pcm_socket(void)
{
    static ds5_pcm_stream stream;
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    int16_t mono[] = {-32768, 12345, 32767};
    uint8_t expected[6], received[6];
    for (unsigned i = 0; i < 3; i++)
        ds5_put_le(expected + 2 * i, (uint16_t)mono[i], 2);
    assert(ds5_pcm_microphone(&stream, mono, 3, 0) == 1);
    // Force an odd-byte partial send, then ensure a new grain cannot splice it.
    assert(send(sockets[0], stream.tx, 1, 0) == 1);
    stream.tx_offset = 1;
    assert(ds5_pcm_microphone(&stream, mono, 3, 0) == 0 && stream.mic_dropped_frames == 3);
    assert(ds5_pcm_pump(&stream, sockets[0]) == 0);
    assert(recv(sockets[1], received, sizeof received, MSG_DONTWAIT) == sizeof received);
    assert(!memcmp(received, expected, sizeof expected));
    assert(ds5_pcm_microphone(&stream, mono, 3, 1) == 1);
    assert(ds5_pcm_pump(&stream, sockets[0]) == 0);
    assert(recv(sockets[1], received, sizeof received, MSG_DONTWAIT) == sizeof received);
    for (unsigned i = 0; i < sizeof received; i++)
        assert(received[i] == 0);
    assert(ds5_pcm_microphone(&stream, mono, DS5_USB_MAX_MIC_FRAMES + 1, 0) == -1);
    close(sockets[1]);
    assert(ds5_pcm_pump(&stream, sockets[0]) == 1);
    // A write to a closed peer must not terminate the process with SIGPIPE.
    assert(ds5_pcm_microphone(&stream, mono, 3, 0) == 1);
    assert(ds5_pcm_pump(&stream, sockets[0]) == 1);
    close(sockets[0]);
}

int main(void)
{
    test_mapping();
    test_format();
    test_pcm_fragments();
    test_pcm_socket();
    puts("DS5 bridge: controller mapping, normal speaker gain, haptic silence, descriptors, fragmented PCM, backpressure, mute and disconnect passed");
    return 0;
}
