// SPDX-License-Identifier: AGPL-3.0-or-later
// The console's game sound (Avcap2 audio capture, 48 kHz stereo float), opened ONCE in the core
// and shared: the Bluetooth headset (A2DP, src/bt/headset.cpp) and Go Live (src/capture_bc.c)
// each acquire it and release it. The capture stays open while any consumer holds it and is
// terminated only when the last one lets go. A reader thread of its own fills a bounded queue
// per consumer (the oldest records go when a consumer is slow; the capture is never blocked
// by one), straight from the capture, before the headset's call mix is added. If the reads
// fail or stop, the capture is opened again (0.5 s, then 1, 2, 5 s apart) while anyone holds it.
// The Bluetooth link is never touched by any of this.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A consumer id (0 or more), or -1 when all slots are taken. Starts the capture if this is the
// first holder; it opens in the background (ga_live tells when).
int ga_acquire(void);
// Lets go; when the last holder leaves, waits (up to 3 s) until the capture is terminated.
void ga_release(int id);
// Copies up to max_bytes of interleaved stereo float (a whole number of 8-byte frames; one record
// at a time). Returns the bytes, 0 when nothing is waiting, -1 while the capture is down.
int ga_read(int id, float *pcm, unsigned max_bytes);
// As ga_read, for a consumer that turns the sound into timed output (Go Live): records older than
// stale_ms (0: keep all) are discarded when read, and *discontinuity (may be NULL) is set to 1 when
// the sound returned now does not follow the sound returned before it (records dropped or discarded,
// or the capture was opened again): start a clean frame, never join the two inside one.
int ga_read2(int id, float *pcm, unsigned max_bytes, unsigned stale_ms, int *discontinuity);
// 1 while the capture is open.
int ga_live(void);

#ifdef __cplusplus
}
#endif
