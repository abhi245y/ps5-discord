// Voice audio (0.5.0): Opus, DAVE frame crypto and RTP for the voice connection, and the
// loopback socket the app streams PCM over (docs/voice-audio.md). voice.c drives the engine;
// the packet, VAD, jitter and mixer functions below are plain and the self-checks call them.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <dave/dave.h>
#include <opus.h>

#define VAU_RATE 48000
#define VAU_FRAME 960 // samples per channel in one 20 ms frame
#define VAU_RTP_PT 0x78 // Opus payload type in RTP
#define VAU_PORT 9042
#define VAU_ID 32 // user id buffer, snowflake plus NUL
#define VAU_FIFO 6 // decoded frames kept per speaker (~120 ms)
#define VAU_MAX_PKT 1500

// ---- pure pieces --------------------------------------------------------------

// Opus for the mic: 48 kHz mono, VOIP, 64 kbps. Playback: 48 kHz stereo. NULL on failure.
OpusEncoder *vau_encoder_new(void);
OpusDecoder *vau_decoder_new(void);

// Energy VAD with hang time. 1 while the frame, or the ~300 ms after speech, is speech.
typedef struct {
    int hang;
} vau_vad;
int vau_vad_frame(vau_vad *v, const int16_t *mono, int n);

// RTP fixed header (version 2, no padding, no extension, no CSRC) for an Opus payload.
void vau_rtp_header(uint8_t out[12], uint16_t seq, uint32_t ts, uint32_t ssrc);

// A plain (transport-opened) RTP packet: where its Opus payload sits. Strips CSRCs, the
// header extension (its body is inside the encrypted part) and padding. -1 when not ours.
typedef struct {
    uint16_t seq;
    uint32_t ts, ssrc;
    size_t off, len;
} vau_rtp;
int vau_rtp_parse(const uint8_t *p, size_t n, vau_rtp *out);

// Sender: DAVE-encrypt the Opus frame (enc NULL, or the silence frame, go out as they are),
// wrap it in RTP and seal it with aead_aes256_gcm_rtpsize. Returns the UDP length, 0 on error.
size_t vau_build_packet(DAVEEncryptorHandle enc, const uint8_t key[32], uint32_t ssrc, uint16_t seq,
                        uint32_t ts, uint32_t counter, const uint8_t *opus, size_t n, uint8_t *out, size_t cap);
// Receiver: transport open, RTP parse, DAVE-decrypt (dec NULL = plain). Returns the Opus
// length in out (0 when dropped) and the packet's ssrc.
size_t vau_open_packet(DAVEDecryptorHandle dec, const uint8_t key[32], const uint8_t *pkt, size_t n,
                       uint8_t *out, size_t cap, uint32_t *ssrc);

// Per-speaker playback queue of decoded stereo frames: starts after two frames, drops the oldest.
typedef struct {
    int16_t pcm[VAU_FIFO][2 * VAU_FRAME];
    int head, count, primed;
} vau_fifo;
void vau_fifo_push(vau_fifo *f, const int16_t *stereo);
int vau_fifo_pop(vau_fifo *f, int16_t *stereo); // 1 when a frame came out

// Mix: sum speakers into acc (2 * VAU_FRAME samples), then clamp to 16 bits.
void vau_mix_add(int32_t *acc, const int16_t *stereo);
void vau_mix_out(const int32_t *acc, int16_t *stereo);

// 1 for an IPv4 loopback address (network byte order): the audio socket accepts only these.
int vau_loopback(uint32_t addr_be);

// ---- engine (voice_audio.c) -----------------------------------------------------

// Opens 127.0.0.1:9042 and starts the 20 ms engine thread. Once, at startup.
void vau_init(void);

// Guards the libdave session against the engine. voice.c holds it around every session call.
void vau_lock(void);
void vau_unlock(void);

// A voice connection is up: UDP socket, transport key, our ssrc, our user id.
// dave_version: the call's; above 0 nothing is sent until vau_dave_state says the group is ready.
void vau_attach(int udp, const uint8_t key[32], uint32_t ssrc, const char *self_id, int dave_version);
// The connection ended: stops the engine from using its socket, keys and session.
void vau_detach(void);
// The attached socket failed for good: the connection must start again (a new socket).
int vau_udp_dead(void);
// The DAVE session and its state (protocol version 0 = plain Opus; ready = group keys exist).
// Call after every change of either, with session NULL before the session is destroyed.
void vau_dave_state(DAVESessionHandle session, int version, int ready);
// Self mute and deafen. Muted: nothing is sent. Deafened: nothing is received or played.
void vau_set_flags(int mute, int deaf);
// Gateway SPEAKING (op 5) from another user: ssrc -> user id. Our own ssrc is ignored.
void vau_map_ssrc(uint32_t ssrc, const char *user_id);
// Client disconnect (op 13): forget the speaker and free its decoder.
// A user in the call (op 11) whose ssrc is not known yet; their audio identifies them.
void vau_expect_user(const char *user_id);
void vau_forget_user(const char *user_id);
// Voice thread, between frames: a SPEAKING op to send (1 start / 0 stop). Returns 0 when none.
int vau_take_speaking(int *speaking, uint32_t *ssrc);
int vau_speaking_pending(void);
// Users heard in the last 300 ms, and us while we send. Returns how many were written.
int vau_speaking(char ids[][VAU_ID], int max);

// An external audio device (the DualSense on USB, controller_voice.c) instead of the
// app: while on, its mic feeds the call (the app's mic bytes are ignored) and the call
// plays on it instead of in the app.
void vau_ext_set(int on);
void vau_ext_mic(const int16_t *mono, size_t frames);       // device thread; copies
size_t vau_ext_playback(int16_t *stereo, size_t frames);    // device thread; frames supplied
// 1 while the app's audio socket is connected: the app is on screen in a call.
int vau_app_connected(void);
