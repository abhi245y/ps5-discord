// Voice protocol pieces with no sockets or threads: transport encryption, IP discovery,
// gateway JSON and binary frames. voice.c drives them; tests/selftest.c checks them.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define VOICE_MODE_GCM "aead_aes256_gcm_rtpsize"

// Clear RTP header length for rtpsize: 12 + CSRCs (+ the 4-byte extension preamble, whose
// words are encrypted with the payload). 0 if malformed. Used as the GCM additional data.
size_t voice_rtp_header_len(const uint8_t *pkt, size_t len);

// aead_aes256_gcm_rtpsize. Seal: pkt is a plain RTP packet, out gets
// header | ciphertext | tag(16) | counter(4, big-endian). cap >= len + 20. Returns the
// sealed length, 0 on error. The GCM nonce is the counter in its first 4 bytes, zero after.
size_t voice_gcm_seal(const uint8_t key[32], uint32_t counter, const uint8_t *pkt, size_t len,
                      uint8_t *out, size_t cap);
// Open: the inverse, out gets the plain packet (len - 20 bytes). 0 on bad tag or size.
size_t voice_gcm_open(const uint8_t key[32], const uint8_t *pkt, size_t len, uint8_t *out, size_t cap);

// IP discovery: 74-byte request (type 1, length 70, ssrc), and parsing of the reply
// (type 2) for our external address and port. Returns 0 on success.
size_t voice_ip_discovery_request(uint8_t out[74], uint32_t ssrc);
int voice_ip_discovery_parse(const uint8_t *in, size_t len, char address[64], uint16_t *port);

// Text opcodes, each a {"op":N,"d":...} JSON string the caller frees.
char *voice_json_identify(const char *server_id, const char *user_id, const char *session_id,
                          const char *token, int dave_max_version);
char *voice_json_resume(const char *server_id, const char *session_id, const char *token, int64_t seq_ack);
char *voice_json_heartbeat(int64_t nonce, int64_t seq_ack);
char *voice_json_select_udp(const char *address, uint16_t port, const char *mode);
// SPEAKING (op 5): {speaking: 1 voice | 2 soundshare | 0, delay: 0, ssrc}.
char *voice_json_speaking(int speaking, uint32_t ssrc);
// Any opcode with a {"transition_id":N} payload (op 23 ready, op 31 invalid commit).
char *voice_json_transition(int op, int64_t transition_id);

// Binary frames: server to client is [seq u16 BE][op u8][payload]; client to server is
// [op u8][payload]. Returns 0 on success, -1 if msg is too short for the header.
typedef struct {
    int seq;                 // sequence number, -1 when absent
    int op;
    const uint8_t *payload;
    size_t len;
} voice_bin;
int voice_bin_parse(const uint8_t *msg, size_t len, voice_bin *out);
// Client frame [op][payload] into out; returns its length, 0 if cap is too small.
size_t voice_bin_build(uint8_t *out, size_t cap, int op, const uint8_t *payload, size_t len);
