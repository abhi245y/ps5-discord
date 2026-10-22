// Host check of the real libdave, with two members in one MLS group: an external sender
// adds B to A's pending group, A commits, B joins by welcome, and an Opus-sized frame is
// encrypted by A and decrypted by B. Built only when libdave is present (see Makefile).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <dave/dave.h>
#include <opus.h>

#include "dave/external_sender_wrapper.h"
#include "voice_audio.h"

static int reported;

// The engine logs through log_printf; this check keeps quiet.
void log_printf(const char *fmt, ...)
{
    (void)fmt;
}

static void on_failure(const char *source, const char *reason, void *user)
{
    (void)user;
    reported++;
    printf("  libdave reported (%s): %s\n", source, reason);
}

int main(void)
{
    const uint64_t group = 1234567890123ull;
    const char *user_a = "1234123412341234", *user_b = "5678567856785678";
    const char *ids[] = {user_a, user_b};
    assert(daveMaxSupportedProtocolVersion() >= 1);

    DAVEExternalSenderHandle ext = daveExternalSenderCreate(group);
    DAVESessionHandle a = daveSessionCreate(NULL, "", on_failure, NULL);
    DAVESessionHandle b = daveSessionCreate(NULL, "", on_failure, NULL);
    assert(ext && a && b);

    uint8_t *ext_bytes = NULL;
    size_t ext_len = 0;
    daveExternalSenderGetMarshalledExternalSender(ext, &ext_bytes, &ext_len);
    assert(ext_bytes && ext_len > 0);
    daveSessionSetExternalSender(a, ext_bytes, ext_len);
    daveSessionSetExternalSender(b, ext_bytes, ext_len);
    daveFree(ext_bytes);
    daveSessionInit(a, 1, group, user_a);
    daveSessionInit(b, 1, group, user_b);

    uint8_t *kp_b = NULL;
    size_t kp_b_len = 0;
    daveSessionGetMarshalledKeyPackage(b, &kp_b, &kp_b_len);
    assert(kp_b && kp_b_len > 0 && kp_b[0] == 0);
    uint8_t *proposal = NULL;
    size_t proposal_len = 0;
    daveExternalSenderProposeAdd(ext, 0, kp_b, kp_b_len, &proposal, &proposal_len);
    assert(proposal && proposal_len > 0);
    daveFree(kp_b);

    uint8_t *commit_welcome = NULL;
    size_t cw_len = 0;
    daveSessionProcessProposals(a, proposal, proposal_len, ids, 2, &commit_welcome, &cw_len);
    assert(commit_welcome && cw_len > 0);
    daveFree(proposal);

    uint8_t *commit = NULL, *welcome = NULL;
    size_t commit_len = 0, welcome_len = 0;
    daveExternalSenderSplitCommitWelcome(ext, commit_welcome, cw_len, &commit, &commit_len, &welcome,
                                         &welcome_len);
    assert(commit && welcome);
    daveFree(commit_welcome);

    DAVECommitResultHandle cr = daveSessionProcessCommit(a, commit, commit_len);
    assert(cr && !daveCommitResultIsFailed(cr));
    DAVEWelcomeResultHandle wr = daveSessionProcessWelcome(b, welcome, welcome_len, ids, 2);
    assert(wr);
    daveFree(commit);
    daveFree(welcome);

    DAVEKeyRatchetHandle ratchet_a = daveSessionGetKeyRatchet(a, user_a);
    DAVEKeyRatchetHandle ratchet_b = daveSessionGetKeyRatchet(b, user_a);
    assert(ratchet_a && ratchet_b);
    DAVEEncryptorHandle enc = daveEncryptorCreate();
    DAVEDecryptorHandle dec = daveDecryptorCreate();
    daveEncryptorAssignSsrcToCodec(enc, 0, DAVE_CODEC_OPUS);
    daveEncryptorSetPassthroughMode(enc, false);
    daveEncryptorSetKeyRatchet(enc, ratchet_a);
    daveDecryptorTransitionToPassthroughMode(dec, false);
    daveDecryptorTransitionToKeyRatchet(dec, ratchet_b);

    uint8_t frame[80], sealed[256], plain[256];
    for (size_t i = 0; i < sizeof frame; i++)
        frame[i] = (uint8_t)(i * 3 + 1);
    size_t n = 0, m = 0;
    assert(daveEncryptorEncrypt(enc, DAVE_MEDIA_TYPE_AUDIO, 0, frame, sizeof frame, sealed, sizeof sealed, &n) ==
           DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS);
    assert(n > sizeof frame && memcmp(sealed, frame, sizeof frame) != 0);
    assert(daveDecryptorDecrypt(dec, DAVE_MEDIA_TYPE_AUDIO, sealed, n, plain, sizeof plain, &m) ==
           DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);
    assert(m == sizeof frame && memcmp(plain, frame, sizeof frame) == 0);

    printf("dave ok: two members, frame %zu -> %zu encrypted -> %zu decrypted, failures reported %d\n",
           sizeof frame, n, m, reported);

    // The media leg as the engine runs it: mic PCM -> Opus -> DAVE -> RTP -> GCM, then GCM open
    // -> DAVE decrypt -> Opus decode. Ten 20 ms frames of a square tone; later frames must match.
    OpusEncoder *oe = vau_encoder_new();
    OpusDecoder *od = vau_decoder_new();
    assert(oe && od);
    uint8_t key[32];
    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)(i * 11 + 5);
    double in_e = 0, out_e = 0;
    for (int f = 0; f < 10; f++) {
        int16_t pcm[VAU_FRAME], dec_pcm[2 * VAU_FRAME];
        uint8_t opus[VAU_MAX_PKT], pkt[VAU_MAX_PKT], back[VAU_MAX_PKT];
        for (int i = 0; i < VAU_FRAME; i++)
            pcm[i] = (f * VAU_FRAME + i) % 100 < 50 ? 6000 : -6000;
        int k = opus_encode(oe, pcm, VAU_FRAME, opus, sizeof opus);
        assert(k > 0);
        size_t len = vau_build_packet(enc, key, 0, (uint16_t)f, (uint32_t)(f * VAU_FRAME), (uint32_t)(f + 1),
                                      opus, (size_t)k, pkt, sizeof pkt);
        assert(len > 0);
        size_t got = vau_open_packet(dec, key, pkt, len, back, sizeof back, NULL);
        assert(got == (size_t)k && memcmp(back, opus, got) == 0);
        assert(opus_decode(od, back, (opus_int32)got, dec_pcm, VAU_FRAME, 0) == VAU_FRAME);
        if (f >= 5)
            for (int i = 0; i < VAU_FRAME; i++) {
                in_e += (double)pcm[i] * pcm[i];
                out_e += (double)dec_pcm[2 * i] * dec_pcm[2 * i];
            }
    }
    double ratio = out_e / in_e;
    assert(ratio > 0.5 && ratio < 2.0);
    printf("media ok: 10 frames through Opus, DAVE, RTP and GCM, energy ratio %.2f\n", ratio);
    opus_encoder_destroy(oe);
    opus_decoder_destroy(od);
    daveCommitResultDestroy(cr);
    daveWelcomeResultDestroy(wr);
    daveKeyRatchetDestroy(ratchet_a);
    daveKeyRatchetDestroy(ratchet_b);
    daveDecryptorDestroy(dec);
    daveEncryptorDestroy(enc);
    daveSessionDestroy(a);
    daveSessionDestroy(b);
    daveExternalSenderDestroy(ext);
    return 0;
}
