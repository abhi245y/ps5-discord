// SPDX-License-Identifier: AGPL-3.0-or-later
// Rational sample-rate conversion for the Bluetooth headset (mono float): a polyphase
// windowed-sinc filter, out = in * up / down. The AirPods mic is 64 kHz (up 3, down 4
// gives 48 kHz), the hands-free link 16 kHz (3/1 and 1/3) or 8 kHz (6/1 and 1/6).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RS_TAPS 16 // per phase
#define RS_MAX_UP 6

typedef struct {
    int up, down, phase;
    float coef[RS_MAX_UP * RS_TAPS];
    float hist[RS_TAPS];
    int pos; // next slot in hist
} rs_state;

// 0, or -1 when up is out of range.
int rs_init(rs_state *r, int up, int down);
// Converts n input samples; writes at most cap outputs and returns how many it wrote.
int rs_process(rs_state *r, const float *in, int n, float *out, int cap);

#ifdef __cplusplus
}
#endif
