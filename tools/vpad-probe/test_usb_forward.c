// SPDX-License-Identifier: AGPL-3.0-or-later
#include "usb_forward_report.h"
#include "ds5_usb_pcm.h"
#include "usb_test_tone.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

static usb_forward_queue queue;
static void *produce(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 100000; ++i) {
        ds5_usb_controller state = {.buttons = i, .left_trigger = (uint8_t)i,
            .right_trigger = (uint8_t)~i};
        while (!usb_forward_push(&queue, &state, i))
            sched_yield();
    }
    return NULL;
}

int main(void)
{
    int16_t tone[960];
    uint64_t cursor = 0;
    unsigned nonzero = 0;
    for (unsigned block = 0; block < 200; ++block) {
        usb_test_tone(tone, 480, &cursor);
        for (unsigned i = 0; i < 480; ++i) {
            unsigned position = block * 480 + i;
            assert(tone[2*i] == tone[2*i+1]);
            assert(tone[2*i] >= -6000 && tone[2*i] <= 6000);
            if (position >= 24000)
                assert(tone[2*i] == 0);
            else
                nonzero += tone[2*i] != 0;
        }
    }
    assert(cursor == 96000 && nonzero > 23000);
    uint8_t report[160];
    usb_forward_report(report, &(ds5_usb_controller){0});
    assert(report[16] == 128 && report[17] == 128 && report[18] == 128 && report[19] == 128);
    assert(report[23] == 1);
    float quaternion;
    memcpy(&quaternion, report + 0x84, sizeof quaternion);
    assert(quaternion == 1.0f);
    ds5_usb_controller state = {.buttons = DS5_CROSS | DS5_RIGHT | DS5_UP | DS5_OPTIONS |
        DS5_L2 | DS5_R2 | DS5_L3 | DS5_TOUCHPAD | DS5_PS | DS5_CREATE | DS5_MUTE,
        .left_x = -32768, .left_y = 32512, .right_x = -256, .right_y = 256,
        .left_trigger = 231, .right_trigger = 19};
    usb_forward_report(report, &state);
    uint32_t buttons;
    memcpy(&buttons, report + 12, sizeof buttons);
    assert(buttons == 0x11433a); /* PS forwarded; Create and Mute must not leak. */
    assert(report[16] == 0 && report[17] == 255 && report[18] == 127 && report[19] == 129);
    assert(report[20] == 231 && report[21] == 19);
    usb_forward_report(report, &(ds5_usb_controller){.buttons = DS5_CIRCLE | DS5_SQUARE |
        DS5_TRIANGLE | DS5_DOWN | DS5_LEFT | DS5_R3 | DS5_L1 | DS5_R1});
    memcpy(&buttons, report + 12, sizeof buttons);
    assert(buttons == 0xbcc4);
    /* PS press/hold/release must retain duration for shell short/long presses.
     * USB DS5_PS is bit 14; the kernel's PS button is bit 16, not bit 17. */
    uint8_t usb_report[64] = {1, 128, 128, 128, 128};
    usb_report[8] = 8; /* Neutral D-pad. */
    usb_report[10] = 1; /* Physical USB report's PS bit. */
    for (unsigned i = 0; i < 90; ++i) {
        assert(ds5_usb_parse_controller(usb_report, sizeof usb_report, &state));
        assert(state.buttons == DS5_PS);
        usb_forward_report(report, &state);
        memcpy(&buttons, report + 12, sizeof buttons);
        assert(buttons == 0x10000);
    }
    usb_report[10] = 0;
    assert(ds5_usb_parse_controller(usb_report, sizeof usb_report, &state));
    usb_forward_report(report, &state);
    memcpy(&buttons, report + 12, sizeof buttons);
    assert(buttons == 0);
    usb_forward_report(report, &(ds5_usb_controller){.buttons = DS5_CREATE | DS5_MUTE});
    memcpy(&buttons, report + 12, sizeof buttons);
    assert(buttons == 0);
    for (unsigned i = 0; i < USB_FORWARD_QUEUE_SIZE - 1; ++i) {
        state.buttons = i;
        assert(usb_forward_push(&queue, &state, 1000u + i));
    }
    assert(!usb_forward_push(&queue, &state, 2000u));
    assert(atomic_load(&queue.dropped) == 1);
    uint64_t received_ms;
    for (unsigned i = 0; i < USB_FORWARD_QUEUE_SIZE - 1; ++i) {
        assert(usb_forward_pop(&queue, &state, &received_ms));
        assert(state.buttons == i);
        assert(received_ms == 1000u + i);
    }
    assert(!usb_forward_pop(&queue, &state, &received_ms));
    // Consumer resumes after a 300 ms stall: queued held controls are already stale.
    state = (ds5_usb_controller){.buttons = DS5_CROSS, .left_x = 32512, .left_trigger = 231};
    assert(usb_forward_push(&queue, &state, 1000));
    assert(usb_forward_pop(&queue, &state, &received_ms));
    ds5_usb_controller fresh = usb_forward_fresh_state(&state, received_ms, 1250);
    assert(fresh.buttons == DS5_CROSS && fresh.left_x == 32512 && fresh.left_trigger == 231);
    fresh = usb_forward_fresh_state(&state, received_ms, 1300);
    usb_forward_report(report, &fresh);
    memcpy(&buttons, report + 12, sizeof buttons);
    assert(buttons == 0 && report[16] == 128 && report[20] == 0);
    // Fresh reports resume input immediately, retaining their producer timestamp.
    assert(usb_forward_push(&queue, &state, 1301));
    assert(usb_forward_pop(&queue, &state, &received_ms));
    fresh = usb_forward_fresh_state(&state, received_ms, 1302);
    assert(fresh.buttons == DS5_CROSS);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, produce, NULL) == 0);
    for (unsigned i = 0; i < 100000; ++i) {
        while (!usb_forward_pop(&queue, &state, &received_ms))
            sched_yield();
        assert(state.buttons == i);
        assert(received_ms == i);
        assert(state.left_trigger == (uint8_t)i && state.right_trigger == (uint8_t)~i);
    }
    assert(pthread_join(thread, NULL) == 0);
    puts("Tone envelope/stereo/silence, USB mapping, receipt freshness, queue bounds "
         "and 100000 concurrent transfers passed");
    return 0;
}
