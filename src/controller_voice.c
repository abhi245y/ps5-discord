// SPDX-License-Identifier: AGPL-3.0-or-later
// Controller audio (controller_voice.h). A supervisor checks five times a second
// whether the controller should be the call's audio and starts or stops the USB
// runtime (tools/ds5-usb-bridge/ds5_usb.c, audio only: the console keeps the buttons,
// the headphone jack and the mute button) to match.
#include "controller_voice.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include "log.h"
#include "voice.h"
#include "voice_audio.h"

static volatile int tv = 1;
static volatile int on, missing;

void cv_set_tv(int pick)
{
    tv = pick;
}

int cv_tv(void)
{
    return tv;
}

const char *cv_state(void)
{
    return on ? "on" : missing ? "missing" : "off";
}

#ifndef __PROSPERO__
void cv_init(void)
{
}
#else

#include "ds5_usb.h"

static volatile int usb_done = 1;

static void usb_log(void *user, const char *message)
{
    (void)user;
    LOG("controller audio: %s", message);
}

static void usb_mic(void *user, const int16_t *mono, size_t frames, int muted)
{
    (void)user;
    (void)muted;
    vau_ext_mic(mono, frames);
}

static size_t usb_playback(void *user, int16_t *stereo, size_t frames)
{
    (void)user;
    return vau_ext_playback(stereo, frames);
}

static void *usb_main(void *arg)
{
    (void)arg;
    ds5_usb_config config;
    ds5_usb_default_config(&config);
    config.duration_ms = 0;       // until the supervisor stops it
    config.audio_only = 1;
    config.notifications = 0;     // the app shows the state
    config.headphone_gain_q15 = 32768; // the console sets the volume
    const ds5_usb_callbacks callbacks = {.log = usb_log, .microphone = usb_mic, .playback = usb_playback};
    ds5_usb_stats stats = {0};
    int r = ds5_usb_run(&config, &callbacks, NULL, &stats);
    if (stats.microphone_frames || r == DS5_USB_OK)
        LOG("controller audio: released (%d): mic %llu frames, played %llu", r,
            (unsigned long long)stats.microphone_frames, (unsigned long long)stats.playback_frames);
    usb_done = 1;
    return NULL;
}

static void *supervise(void *arg)
{
    (void)arg;
    pthread_t usb;
    int running = 0;
    unsigned retry = 0; // ticks to wait after the controller was not found
    for (;;) {
        usleep(200000);
        int want = voice_wanted() && (!tv || !vau_app_connected());
        if (running && usb_done) { // ended by itself: unplugged, or never found
            pthread_join(usb, NULL);
            running = on = 0;
            vau_ext_set(0);
            missing = 1;
            retry = 150; // 30 s: most controllers are on Bluetooth
        }
        if (want && !running && (retry == 0 || --retry == 0)) {
            usb_done = 0;
            vau_ext_set(1);
            running = pthread_create(&usb, NULL, usb_main, NULL) == 0;
            if (!running) {
                vau_ext_set(0);
                usb_done = 1;
            }
            on = running;
            missing = 0;
        } else if (!want && running) {
            ds5_usb_request_stop();
            pthread_join(usb, NULL);
            running = on = 0;
            vau_ext_set(0);
        }
        if (!want)
            missing = 0, retry = 0;
    }
    return NULL;
}

void cv_init(void)
{
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, supervise, NULL) != 0)
        LOG("controller audio: supervisor did not start");
}
#endif
