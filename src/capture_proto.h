// SPDX-License-Identifier: MIT
// The wire protocol between the core (src/capture_bc.c, the wireless Go Live backend) and the
// capture helper payload (capture/, discord-capture.elf). Shared by both sides, so it stays
// header-only, dependency-free and permissively licensed (the helper is GPL-3.0-only, the core
// AGPL).
//
// Transport: loopback TCP. The CORE listens on 127.0.0.1 and writes "<port> <token hex>\n" to a
// 0600 file (CAP_CONN_FILE) before it sends the helper's ELF to elfldr; the helper reads the file,
// connects and presents the token first (CAP_HELLO). A connection that does not is dropped.
//
// A message: an 8-byte header, then len bytes. Integers are little-endian.
//   u8 type, u8 flags, u16 0, u32 len
// Video only: the game's sound is captured by the core itself (src/bt/game_audio.cpp).
//
// Either side closing the socket ends the session: the helper ends its capture session
// (Stop/Close/ClearCallback/Terminate, scratch restored, detached) and exits; the core treats a
// close it did not ask for as a stop with an error.
#pragma once
#include <stdint.h>
#include <string.h>

#define CAP_CONN_FILE "/data/ps5-discord/capture.conn"
#define CAP_LOCK_FILE "/data/ps5-discord/capture.lock"
#define CAP_PROCESS_NAME "discord-capture.elf"
#define CAP_ELF_NAME "discord-capture.elf" // in the app's assets
// Deadline nesting shared by the separate core and helper. Remote cleanup may make up to
// ten bounded calls (including their stop/restore waits), so recovery reserves 30 seconds.
#define CAP_STARTUP_MS 15000u
#define CAP_PICTURE_MS 10000u
#define CAP_MODE_WAIT_MS 5000u
#define CAP_CLEANUP_BUDGET_MS 30000u
#define CAP_DEADLINE_MARGIN_MS 5000u
#define CAP_FIRST_MEDIA_MS (CAP_STARTUP_MS + CAP_PICTURE_MS + CAP_DEADLINE_MARGIN_MS)
#define CAP_MEDIA_MS (CAP_PICTURE_MS + CAP_DEADLINE_MARGIN_MS)
#define CAP_RECOVERY_MS (CAP_MODE_WAIT_MS + CAP_CLEANUP_BUDGET_MS + CAP_FIRST_MEDIA_MS)

#define CAP_VERSION 1u
#define CAP_TOKEN_BYTES 16
#define CAP_HEADER_BYTES 8
#define CAP_MAX_PAYLOAD (2u * 1024u * 1024u)

enum cap_type {
    // helper -> core
    CAP_HELLO = 1,   // u32 version, token[CAP_TOKEN_BYTES]; always first
    CAP_VIDEO = 2,   // one Annex-B access unit; flags & CAP_KEY: it holds an IDR
    CAP_HEADERS = 3, // the real SPS and PPS (Annex-B, 4-byte start codes); sent before the first IDR and again after a recovery
    CAP_PROFILE = 4, // u32 width, height, fps, output mode: what the encoder actually runs
    CAP_STATE = 5,   // u8 cap_state, then text (no NUL): why, for CAP_ERROR
    // core -> helper
    CAP_START = 16,    // u32 selector (3 720p, 4 1080p), fps flag (0 30, 1 60), bitrate (bit/s), width, height, fps
    CAP_STOP = 17,     // end the session cleanly, then close
    CAP_KEYFRAME = 18, // request an IDR (the encoder emits them only on request)
};
#define CAP_KEY 1u

enum cap_state { CAP_STARTING = 1, CAP_LIVE = 2, CAP_RECOVERING = 3, CAP_ERROR = 4 };

typedef struct {
    uint8_t type, flags;
    uint32_t len;
} cap_header;

static inline void cap_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t cap_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void cap_pack_header(uint8_t out[CAP_HEADER_BYTES], uint8_t type, uint8_t flags, uint32_t len)
{
    out[0] = type, out[1] = flags, out[2] = out[3] = 0;
    cap_put32(out + 4, len);
}

// 0 when the header is well formed (reserved bytes zero, len within CAP_MAX_PAYLOAD).
static inline int cap_parse_header(const uint8_t in[CAP_HEADER_BYTES], cap_header *h)
{
    h->type = in[0], h->flags = in[1], h->len = cap_get32(in + 4);
    return in[2] || in[3] || h->len > CAP_MAX_PAYLOAD ? -1 : 0;
}
