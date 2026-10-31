// SPDX-License-Identifier: AGPL-3.0-or-later
#include "resample.h"

#include <math.h>
#include <string.h>

int rs_init(rs_state *r, int up, int down)
{
    if (up < 1 || up > RS_MAX_UP || down < 1)
        return -1;
    memset(r, 0, sizeof *r);
    r->up = up;
    r->down = down;
    // The filter runs at in * up; it cuts at 0.45 of the lower of the two rates.
    const int n = up * RS_TAPS;
    const double cut = 0.45 / (up > down ? up : down);
    double sum = 0;
    for (int k = 0; k < n; k++) {
        const double m = k - (n - 1) / 2.0;
        const double s = m == 0 ? 2 * cut : sin(2 * M_PI * cut * m) / (M_PI * m);
        const double w = 0.42 - 0.5 * cos(2 * M_PI * k / (n - 1)) + 0.08 * cos(4 * M_PI * k / (n - 1)); // Blackman
        r->coef[k] = (float)(s * w);
        sum += s * w;
    }
    for (int k = 0; k < n; k++)
        r->coef[k] = (float)(r->coef[k] * up / sum); // unity gain after the zero stuffing
    return 0;
}

int rs_process(rs_state *r, const float *in, int n, float *out, int cap)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        r->hist[r->pos] = in[i];
        r->pos = (r->pos + 1) % RS_TAPS;
        // Outputs fall on every down-th sample of the up-times rate; phase is where the
        // next one lands after this input sample.
        for (; r->phase < r->up; r->phase += r->down) {
            float y = 0;
            for (int j = 0; j < RS_TAPS; j++) // hist newest first
                y += r->coef[r->phase + j * r->up] * r->hist[(r->pos - 1 - j + RS_TAPS) % RS_TAPS];
            if (w < cap)
                out[w++] = y;
        }
        r->phase -= r->up;
    }
    return w;
}
