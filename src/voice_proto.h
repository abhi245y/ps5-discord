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

// RTCP from the media server (rtpsize): the first 8 bytes (header, sender SSRC) are the clear
// additional data. Returns the plain length, 0 on bad tag or size.
size_t voice_gcm_open_rtcp(const uint8_t key[32], const uint8_t *pkt, size_t len, uint8_t *out, size_t cap);

// What viewers report about one sent SSRC, summed over the RTCP packets given to rtcp_parse.
typedef struct {
    unsigned reports;        // receiver report blocks about this SSRC
    unsigned max_fraction;   // largest fraction lost in a block, out of 256
    int32_t cumulative;      // the latest cumulative packets lost
    uint32_t max_jitter;     // largest interarrival jitter, in RTP timestamp units
    unsigned nack_packets;   // packets asked for again (generic NACK)
    unsigned pli, fir;       // picture loss and full intra requests
    uint32_t remb_bps;       // the latest receiver estimated maximum bitrate, 0 if none
} rtcp_stats;
// Adds one plain (compound) RTCP packet's reports about `ssrc` to st. Returns -1 if malformed.
int rtcp_parse(const uint8_t *p, size_t len, uint32_t ssrc, rtcp_stats *st);
#define RTCP_NACK_MAX 256
// Also extracts PID + BLP sequences for ssrc, up to min(cap, RTCP_NACK_MAX). All NACKs
// still count in st. *n_nacks is zero on malformed input; ignore the output then.
int rtcp_parse_nacks(const uint8_t *p, size_t len, uint32_t ssrc, rtcp_stats *st,
                     uint16_t *nacks, size_t cap, size_t *n_nacks);

#define VOICE_VIDEO_MTU 1200
#define VOICE_RTX_PACKETS 1024
typedef struct {
    uint8_t payload[VOICE_VIDEO_MTU + 2]; // FU-A adds two bytes to the fragment's data
    uint16_t len, seq;
    uint32_t ts;
    int marker;
    int64_t last_rtx_ms; // -1 until the first attempt, so time zero also works
} voice_rtx_packet;
// Zero-initialize with its connection; sequence-indexed slots make NACK lookup constant time.
typedef struct {
    voice_rtx_packet packets[VOICE_RTX_PACKETS];
} voice_rtx_cache;
// Stores a copy, returning 0 if the payload is empty or too large, 1 otherwise.
int voice_rtx_store(voice_rtx_cache *cache, uint16_t seq, uint32_t ts, int marker,
                     const uint8_t *payload, size_t len);
voice_rtx_packet *voice_rtx_find(voice_rtx_cache *cache, uint16_t seq);
// RFC 4588 plain RTP: independent sequence/SSRC, original timestamp/marker, OSN then payload.
// Returns the packet length, or 0 if cap is too small or the cached packet is invalid.
size_t voice_rtx_build(const voice_rtx_packet *packet, uint8_t pt, uint32_t ssrc, uint16_t seq,
                       uint8_t *out, size_t cap);

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

// H.264 for Go Live. The next NAL unit in Annex-B data from *pos: its start (after the
// start code) and length, or NULL at the end.
const uint8_t *h264_next_nal(const uint8_t *b, size_t n, size_t *pos, size_t *len);
// RFC 6184 payloads of one access unit: a NAL up to mtu bytes whole, a larger one as FU-A
// fragments (mtu + 2 bytes each). emit gets each payload and whether it ends the unit
// (the RTP marker); the buffer is valid during the call only.
void h264_packetize(const uint8_t *au, size_t n, size_t mtu,
                    void (*emit)(void *user, const uint8_t *payload, size_t len, int marker), void *user);
